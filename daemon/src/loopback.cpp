#include "loopback.hpp"

#include "capture.hpp"  // g_credMutex

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <opencv2/imgproc.hpp>

static int xioctl(int fd, unsigned long req, void* arg) {
  int r;
  do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
  return r;
}

std::string findLoopbackByLabel(const std::string& label) {
  DIR* d = opendir("/sys/class/video4linux");
  if (!d) return "";
  std::string found;
  while (dirent* e = readdir(d)) {
    if (strncmp(e->d_name, "video", 5) != 0) continue;
    std::ifstream f(std::string("/sys/class/video4linux/") + e->d_name + "/name");
    std::string name;
    std::getline(f, name);
    if (name == label) { found = std::string("/dev/") + e->d_name; break; }
  }
  closedir(d);
  return found;
}

bool LoopbackWriter::open(const std::string& path, int w, int h, std::string* err) {
  close();
  fd_ = ::open(path.c_str(), O_RDWR);
  if (fd_ < 0) { if (err) *err = std::string("open ") + path + ": " + strerror(errno); return false; }
  v4l2_format fmt{};
  fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  fmt.fmt.pix.width = w;
  fmt.fmt.pix.height = h;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
  fmt.fmt.pix.field = V4L2_FIELD_NONE;
  fmt.fmt.pix.bytesperline = w * 2;
  fmt.fmt.pix.sizeimage = w * h * 2;
  fmt.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
  if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) { if (err) *err = std::string("S_FMT: ") + strerror(errno); close(); return false; }
  w_ = w; h_ = h; path_ = path;
  // With exclusive_caps the device only looks like a camera to apps once the
  // writer has pushed a frame; prime it with a dark frame so apps list it
  // while we sit idle (verified: one write() flips it and it stays that way
  // for as long as this fd is open).
  cv::Mat black(h, w, CV_8UC3, cv::Scalar(0, 0, 0));
  write(black);
  return true;
}

void LoopbackWriter::close() {
  if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

bool LoopbackWriter::write(const cv::Mat& bgr) {
  if (fd_ < 0) return false;
  cv::Mat src = bgr;
  if (src.cols != w_ || src.rows != h_) cv::resize(bgr, src, cv::Size(w_, h_), 0, 0, cv::INTER_AREA);
  cv::cvtColor(src, yuyv_, cv::COLOR_BGR2YUV_YUYV);
  return writeYuyv(yuyv_);
}

bool LoopbackWriter::writeYuyv(const cv::Mat& yuyv) {
  if (fd_ < 0 || yuyv.type() != CV_8UC2 || yuyv.cols != w_ || yuyv.rows != h_ || !yuyv.isContinuous()) return false;
  size_t len = yuyv.total() * yuyv.elemSize();
  ssize_t n = ::write(fd_, yuyv.data, len);
  return n == (ssize_t)len;
}

// ---------------------------------------------------------------------------

void ConsumerWatcher::start(const std::string& path, std::function<void(int)> onChange) {
  stop();
  path_ = path;
  onChange_ = std::move(onChange);
  stop_ = false;
  thread_ = std::thread([this] { run(); });
}

void ConsumerWatcher::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
}

// Count fds on the device across all processes except ourselves. Only
// processes of our own uid are readable anyway; that is also exactly the set
// that can open the loopback (apps of the logged-in user). When running
// setgid the scan drops to the real gid for its duration (see g_credMutex).
// glibc applies setegid() to every thread, and the kernel clears
// PR_SET_PDEATHSIG (and the dumpable flag) on each such credential change, so
// the "die with the shell" flag from main.cpp is re-armed after the restore; it
// is per thread and one armed thread is enough for the whole process.
int ConsumerWatcher::scanProc() {
  struct stat target{};
  if (stat(path_.c_str(), &target) != 0) return 0;
  std::lock_guard<std::mutex> lk(g_credMutex);
  gid_t egid = getegid(), rgid = getgid();
  bool dropped = egid != rgid && setegid(rgid) == 0;
  struct Restore { bool on; gid_t g; ~Restore() { if (on) { (void)!setegid(g); prctl(PR_SET_PDEATHSIG, SIGTERM); } } } restore{ dropped, egid };
  pid_t self = getpid();
  int count = 0;
  DIR* proc = opendir("/proc");
  if (!proc) return 0;
  while (dirent* pe = readdir(proc)) {
    if (pe->d_name[0] < '0' || pe->d_name[0] > '9') continue;
    pid_t pid = atoi(pe->d_name);
    if (pid == self) continue;
    std::string fdDir = std::string("/proc/") + pe->d_name + "/fd";
    DIR* fds = opendir(fdDir.c_str());
    if (!fds) continue;
    while (dirent* fe = readdir(fds)) {
      if (fe->d_name[0] == '.') continue;
      struct stat st{};
      if (stat((fdDir + "/" + fe->d_name).c_str(), &st) != 0) continue;
      if (S_ISCHR(st.st_mode) && st.st_rdev == target.st_rdev) { count++; break; }  // one per process
    }
    closedir(fds);
  }
  closedir(proc);
  return count;
}

void ConsumerWatcher::run() {
  int in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  int wd = in >= 0 ? inotify_add_watch(in, path_.c_str(), IN_OPEN | IN_CLOSE) : -1;
  // Event-based delta since the last reconcile. Our own opens/closes are
  // excluded by scanProc; on the event path we cannot tell who opened, so the
  // periodic scan corrects any drift.
  int base = scanProc();
  int delta = 0;
  auto publish = [&](int c) {
    if (c < 0) c = 0;
    if (c != consumers_.load()) { consumers_ = c; if (onChange_) onChange_(c); }
  };
  publish(base);
  auto lastScan = std::chrono::steady_clock::now();
  char buf[4096];
  while (!stop_) {
    pollfd p{ in, POLLIN, 0 };
    int r = in >= 0 ? poll(&p, 1, 250) : (usleep(250000), 0);
    if (r > 0) {
      auto drain = [&] {
        for (;;) {
          ssize_t n = read(in, buf, sizeof buf);
          if (n <= 0) break;
          for (ssize_t off = 0; off < n;) {
            auto* ev = (inotify_event*)(buf + off);
            if (ev->mask & IN_OPEN) delta++;
            if (ev->mask & IN_CLOSE) delta--;
            off += sizeof(inotify_event) + ev->len;
          }
        }
      };
      drain();
      // Debounce: apps enumerate cameras with a quick open/close; wait for the
      // dust to settle, then pick up the CLOSE that may have landed meanwhile
      // before treating it as a real consumer.
      usleep(150000);
      drain();
      publish(base + delta);
    }
    auto now = std::chrono::steady_clock::now();
    if (now - lastScan > std::chrono::seconds(5)) {
      base = scanProc();
      delta = 0;
      publish(base);
      lastScan = now;
    }
  }
  if (wd >= 0) inotify_rm_watch(in, wd);
  if (in >= 0) ::close(in);
}
