#include "capture.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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

// Identify a USB camera by vendor/product/serial from sysfs so udev rules and
// settings can refer to the physical unit rather than /dev/videoN.
static std::string readSysAttr(const std::string& p) {
  std::ifstream f(p);
  std::string v;
  std::getline(f, v);
  while (!v.empty() && (v.back() == '\n' || v.back() == ' ')) v.pop_back();
  for (auto& c : v) if (c == '/' || c == ' ') c = '_';
  return v;
}
static std::string usbKeyFor(const std::string& node) {
  std::string base = "/sys/class/video4linux/" + node + "/device/../";
  std::string vid = readSysAttr(base + "idVendor"), pid = readSysAttr(base + "idProduct");
  if (vid.empty() || pid.empty()) return "";
  std::string serial = readSysAttr(base + "serial");
  return "usb-" + vid + "-" + pid + (serial.empty() ? "" : "-" + serial);
}

std::vector<CameraInfo> enumerateCameras(const std::string& excludePath) {
  std::vector<CameraInfo> out;
  std::map<std::string, int> seenBus;  // bus -> index in out
  DIR* d = opendir("/dev");
  if (!d) return out;
  std::vector<std::string> names;
  while (dirent* e = readdir(d)) {
    if (strncmp(e->d_name, "video", 5) == 0) names.push_back(e->d_name);
  }
  closedir(d);
  // Sort numerically so /dev/video0 comes before /dev/video10.
  std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
    return atoi(a.c_str() + 5) < atoi(b.c_str() + 5);
  });
  for (const auto& n : names) {
    std::string path = "/dev/" + n;
    if (path == excludePath) continue;
    int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) continue;
    v4l2_capability cap{};
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) { ::close(fd); continue; }
    unsigned caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) { ::close(fd); continue; }
    if (caps & V4L2_CAP_META_CAPTURE) { ::close(fd); continue; }
    // A node without any capture pixel format is a metadata/control node.
    v4l2_fmtdesc fd_desc{};
    fd_desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bool hasFmt = xioctl(fd, VIDIOC_ENUM_FMT, &fd_desc) == 0;
    ::close(fd);
    if (!hasFmt) continue;
    // Skip loopback devices in general (ours or OBS's): they are outputs of
    // other software, not physical cameras.
    if (strncmp((const char*)cap.driver, "v4l2 loopback", 13) == 0) continue;
    CameraInfo ci{ path, (const char*)cap.card, (const char*)cap.bus_info, usbKeyFor(n) };
    if (seenBus.count(ci.bus)) continue;
    seenBus[ci.bus] = (int)out.size();
    out.push_back(ci);
  }
  return out;
}

bool V4L2Capture::open(const std::string& path, int w, int h, int fps, std::string* err) {
  close();
  fd_ = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
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
    if (err) *err = "unsupported pixel format " + fourcc_; close(); return false;
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
      cv::Mat img = cv::imdecode(buf, cv::IMREAD_COLOR);
      if (img.empty()) return false;
      bgr = img;
      return true;
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
  if (!(b.flags & V4L2_BUF_FLAG_ERROR) && b.bytesused > 0)
    ok = decode((const unsigned char*)bufs_[b.index].start, b.bytesused, bgr);
  xioctl(fd_, VIDIOC_QBUF, &b);
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
  if (!impl_->still.empty()) { bgr = impl_->still.clone(); return true; }
  if (!impl_->cap.read(bgr) || bgr.empty()) {
    impl_->cap.set(cv::CAP_PROP_POS_FRAMES, 0);
    if (!impl_->cap.read(bgr) || bgr.empty()) return false;
  }
  return true;
}
