#include "capture.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <chrono>
#include <thread>

static int xioctl(int fd, unsigned long req, void* arg) {
  int r;
  do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
  return r;
}

static std::string fourccStr(unsigned f) {
  char s[5] = { char(f & 0xff), char((f >> 8) & 0xff), char((f >> 16) & 0xff), char((f >> 24) & 0xff), 0 };
  return s;
}

std::mutex g_credMutex;

// Identify a USB camera by vendor/product/serial from sysfs so udev rules and
// settings can refer to the physical unit rather than /dev/videoN.
static std::string readSysAttr(const std::string& p) {
  std::ifstream f(p);
  std::string v;
  std::getline(f, v);
  while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
  return v;
}
static bool isHex4(const std::string& s) {
  if (s.size() != 4) return false;
  for (char c : s) if (!isxdigit((unsigned char)c)) return false;
  return true;
}
// The key ends up in a udev rule and a file name (root, via the setup script),
// so it is built only from characters that are safe there. The setup script
// re-validates it with the same rule; a serial that does not fit is dropped.
static bool isSafeSerial(const std::string& s) {
  if (s.empty() || s.size() > 64) return false;
  for (char c : s) if (!(isalnum((unsigned char)c) || c == '.' || c == '_')) return false;
  return true;
}
static std::string usbKeyFor(const std::string& node) {
  std::string base = "/sys/class/video4linux/" + node + "/device/../";
  std::string vid = readSysAttr(base + "idVendor"), pid = readSysAttr(base + "idProduct");
  if (!isHex4(vid) || !isHex4(pid)) return "";
  std::string serial = readSysAttr(base + "serial");
  return "usb-" + vid + "-" + pid + (isSafeSerial(serial) ? "-" + serial : "");
}

// Open the node once and decide whether it is a real capture camera.
enum class ProbeResult { NoAccess, NotCamera, Camera };
static ProbeResult probeCamera(const std::string& path, const std::string& node, CameraInfo& info) {
  int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) return ProbeResult::NoAccess;
  v4l2_capability cap{};
  if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) { ::close(fd); return ProbeResult::NotCamera; }
  unsigned caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
  if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING) || (caps & V4L2_CAP_META_CAPTURE)) { ::close(fd); return ProbeResult::NotCamera; }
  // A node without any capture pixel format is a metadata/control node.
  v4l2_fmtdesc fd_desc{};
  fd_desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  bool hasFmt = xioctl(fd, VIDIOC_ENUM_FMT, &fd_desc) == 0;
  ::close(fd);
  if (!hasFmt) return ProbeResult::NotCamera;
  // Loopback devices (ours or OBS's) are outputs of other software, not cameras.
  if (strncmp((const char*)cap.driver, "v4l2 loopback", 13) == 0) return ProbeResult::NotCamera;
  info = CameraInfo{ path, (const char*)cap.card, (const char*)cap.bus_info, usbKeyFor(node) };
  return ProbeResult::Camera;
}

std::vector<CameraInfo> CameraEnumerator::scan() {
  std::vector<CameraInfo> out;
  DIR* d = opendir("/sys/class/video4linux");
  if (!d) return out;
  std::vector<std::string> nodes;
  while (dirent* e = readdir(d)) {
    if (strncmp(e->d_name, "video", 5) == 0) nodes.push_back(e->d_name);
  }
  closedir(d);
  // Sort numerically so /dev/video0 comes before /dev/video10.
  std::sort(nodes.begin(), nodes.end(), [](const std::string& a, const std::string& b) {
    return atoi(a.c_str() + 5) < atoi(b.c_str() + 5);
  });
  std::map<std::string, Probe> fresh;
  std::map<std::string, int> seenBus;
  std::lock_guard<std::mutex> credLk(g_credMutex);
  for (const auto& node : nodes) {
    std::string sys = "/sys/class/video4linux/" + node;
    std::string path = "/dev/" + node;
    // Virtual (loopback) devices have no physical parent: skip without opening.
    char real[PATH_MAX];
    if (!realpath((sys + "/device").c_str(), real) || strstr(real, "/devices/virtual/")) continue;
    struct stat st{};
    if (stat(path.c_str(), &st) != 0 || !S_ISCHR(st.st_mode)) continue;
    // Identity of what sits behind /dev/videoN: reprobe only when it changes.
    std::string ident = readSysAttr(sys + "/name") + "|" + real + "|" + std::to_string(st.st_rdev);
    Probe pr;
    auto it = cache_.find(path);
    if (it != cache_.end() && it->second.ident == ident) pr = it->second;
    else {
      pr.ident = ident;
      ProbeResult r = probeCamera(path, node, pr.info);
      // Not cached when the open itself failed (e.g. udev has not applied the
      // ACL yet): retry on the next scan.
      if (r == ProbeResult::NoAccess) continue;
      pr.isCamera = r == ProbeResult::Camera;
    }
    fresh[path] = pr;
    if (!pr.isCamera) continue;
    if (seenBus.count(pr.info.bus)) continue;
    seenBus[pr.info.bus] = (int)out.size();
    out.push_back(pr.info);
  }
  cache_.swap(fresh);
  return out;
}

bool V4L2Capture::open(const std::string& path, int w, int h, int fps, std::string* err) {
  close();
  std::lock_guard<std::mutex> credLk(g_credMutex);  // needs the privileged gid for hidden cameras
  fd_ = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd_ < 0) { if (err) *err = std::string("open: ") + strerror(errno); return false; }
  path_ = path;

  // Prefer MJPEG (USB 2 cameras cannot do 1080p30 raw), then YUYV, then whatever.
  const unsigned prefs[] = { V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_UYVY };
  std::vector<unsigned> supported;
  for (int i = 0;; i++) {
    v4l2_fmtdesc fdsc{};
    fdsc.index = i; fdsc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_ENUM_FMT, &fdsc) < 0) break;
    supported.push_back(fdsc.pixelformat);
  }
  unsigned chosen = 0;
  for (unsigned p : prefs) if (std::find(supported.begin(), supported.end(), p) != supported.end()) { chosen = p; break; }
  if (!chosen && !supported.empty()) chosen = supported[0];
  if (!chosen) { if (err) *err = "no capture formats"; close(); return false; }

  v4l2_format fmt{};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = w; fmt.fmt.pix.height = h;
  fmt.fmt.pix.pixelformat = chosen;
  fmt.fmt.pix.field = V4L2_FIELD_NONE;
  if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) { if (err) *err = std::string("S_FMT: ") + strerror(errno); close(); return false; }
  width_ = fmt.fmt.pix.width; height_ = fmt.fmt.pix.height; pixfmt_ = fmt.fmt.pix.pixelformat;
  fourcc_ = fourccStr(pixfmt_);
  if (pixfmt_ != V4L2_PIX_FMT_MJPEG && pixfmt_ != V4L2_PIX_FMT_YUYV && pixfmt_ != V4L2_PIX_FMT_NV12 && pixfmt_ != V4L2_PIX_FMT_UYVY) {
    if (err) *err = "unsupported pixel format " + fourcc_;
    close();
    return false;
  }

  v4l2_streamparm parm{};
  parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  parm.parm.capture.timeperframe.numerator = 1;
  parm.parm.capture.timeperframe.denominator = fps > 0 ? fps : 30;
  xioctl(fd_, VIDIOC_S_PARM, &parm);  // best effort

  v4l2_requestbuffers req{};
  req.count = 4; req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; req.memory = V4L2_MEMORY_MMAP;
  if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) { if (err) *err = std::string("REQBUFS: ") + strerror(errno); close(); return false; }
  bufs_.resize(req.count);
  for (unsigned i = 0; i < req.count; i++) {
    v4l2_buffer b{};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP; b.index = i;
    if (xioctl(fd_, VIDIOC_QUERYBUF, &b) < 0) { if (err) *err = "QUERYBUF"; close(); return false; }
    bufs_[i].length = b.length;
    bufs_[i].start = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, b.m.offset);
    if (bufs_[i].start == MAP_FAILED) { bufs_[i].start = nullptr; if (err) *err = "mmap"; close(); return false; }
  }
  for (unsigned i = 0; i < req.count; i++) {
    v4l2_buffer b{};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP; b.index = i;
    if (xioctl(fd_, VIDIOC_QBUF, &b) < 0) { if (err) *err = "QBUF"; close(); return false; }
  }
  v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) { if (err) *err = std::string("STREAMON: ") + strerror(errno); close(); return false; }
  streaming_ = true;
  return true;
}

void V4L2Capture::close() {
  if (fd_ >= 0) {
    if (streaming_) {
      v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      xioctl(fd_, VIDIOC_STREAMOFF, &type);
      streaming_ = false;
    }
    for (auto& b : bufs_) if (b.start) munmap(b.start, b.length);
    bufs_.clear();
    ::close(fd_);
    fd_ = -1;
  }
}

bool V4L2Capture::decode(const unsigned char* data, size_t len, cv::Mat& bgr) {
  switch (pixfmt_) {
    case V4L2_PIX_FMT_MJPEG: {
      cv::Mat buf(1, (int)len, CV_8UC1, const_cast<unsigned char*>(data));
      // Decode into the caller's Mat: reused across frames instead of a fresh
      // multi-MB allocation per frame.
      cv::Mat img = cv::imdecode(buf, cv::IMREAD_COLOR, &bgr);
      return !img.empty();
    }
    case V4L2_PIX_FMT_YUYV: {
      if (len < size_t(width_ * height_ * 2)) return false;
      cv::Mat yuv(height_, width_, CV_8UC2, const_cast<unsigned char*>(data));
      cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_YUYV);
      return true;
    }
    case V4L2_PIX_FMT_UYVY: {
      if (len < size_t(width_ * height_ * 2)) return false;
      cv::Mat yuv(height_, width_, CV_8UC2, const_cast<unsigned char*>(data));
      cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_UYVY);
      return true;
    }
    case V4L2_PIX_FMT_NV12: {
      if (len < size_t(width_ * height_ * 3 / 2)) return false;
      cv::Mat yuv(height_ * 3 / 2, width_, CV_8UC1, const_cast<unsigned char*>(data));
      cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_NV12);
      return true;
    }
  }
  return false;
}

bool V4L2Capture::grab(cv::Mat& bgr, int timeoutMs) {
  if (fd_ < 0) return false;
  pollfd p{ fd_, POLLIN, 0 };
  int r = poll(&p, 1, timeoutMs);
  if (r <= 0) return false;
  v4l2_buffer b{};
  b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP;
  if (xioctl(fd_, VIDIOC_DQBUF, &b) < 0) return false;
  bool ok = false;
  auto t0 = std::chrono::steady_clock::now();
  if (!(b.flags & V4L2_BUF_FLAG_ERROR) && b.bytesused > 0)
    ok = decode((const unsigned char*)bufs_[b.index].start, b.bytesused, bgr);
  xioctl(fd_, VIDIOC_QBUF, &b);
  decodeMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return ok;
}

// ---------------------------------------------------------------------------

struct FileCapture::Impl {
  cv::VideoCapture cap;
  cv::Mat still;
};

static double monoNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

bool FileCapture::open(const std::string& path, int fps, std::string* err) {
  close();
  impl_ = std::make_shared<Impl>();
  impl_->still = cv::imread(path, cv::IMREAD_COLOR);
  if (impl_->still.empty()) {
    if (!impl_->cap.open(path) || !impl_->cap.isOpened()) { if (err) *err = "cannot open " + path; impl_.reset(); return false; }
    width_ = (int)impl_->cap.get(cv::CAP_PROP_FRAME_WIDTH);
    height_ = (int)impl_->cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    double f = impl_->cap.get(cv::CAP_PROP_FPS);
    if (f > 1 && f < 120) fps = (int)f;
  } else {
    width_ = impl_->still.cols; height_ = impl_->still.rows;
  }
  period_ = 1.0 / (fps > 0 ? fps : 30);
  next_ = monoNow();
  open_ = true;
  return true;
}

void FileCapture::close() { impl_.reset(); open_ = false; }

bool FileCapture::grab(cv::Mat& bgr, int) {
  if (!open_) return false;
  double now = monoNow();
  if (now < next_) std::this_thread::sleep_for(std::chrono::duration<double>(next_ - now));
  next_ = std::max(next_ + period_, monoNow() - period_);
  double t0 = monoNow();
  if (!impl_->still.empty()) { impl_->still.copyTo(bgr); decodeMs_ = 0; return true; }
  if (!impl_->cap.read(bgr) || bgr.empty()) {
    impl_->cap.set(cv::CAP_PROP_POS_FRAMES, 0);
    if (!impl_->cap.read(bgr) || bgr.empty()) return false;
  }
  decodeMs_ = (monoNow() - t0) * 1000;
  return true;
}
