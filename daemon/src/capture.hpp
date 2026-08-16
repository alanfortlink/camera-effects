#pragma once
#include <opencv2/core.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Process-wide credential lock. When running setgid (hide-raw mode) the
// effective gid differs from the user's, which makes /proc/<pid>/fd of the
// user's other processes unreadable; the consumer scan (loopback.cpp) drops to
// the real gid for its duration under this lock, and everything that needs the
// privileged gid (opening or probing a raw camera, below) holds it meanwhile.
extern std::mutex g_credMutex;

struct CameraInfo {
  std::string path;     // /dev/videoN
  std::string name;     // card name
  std::string bus;      // bus_info (stable across reboots for the same port)
  std::string key;      // "usb-<vid>-<pid>[-<serial>]" for USB cameras (used for per-camera hide rules), else ""
                        // vid/pid are 4 hex digits; the serial is included only if it is [A-Za-z0-9._]{1,64}
};

// Enumerates real capture-capable cameras: one entry per physical device,
// skipping virtual (loopback) devices, metadata and non-capture nodes.
// Nodes are listed from sysfs; a node is opened (VIDIOC_QUERYCAP) only the
// first time it is seen, so idle rescans do not wake cameras from autosuspend.
class CameraEnumerator {
public:
  std::vector<CameraInfo> scan();

private:
  struct Probe { std::string ident; bool isCamera = false; CameraInfo info; };
  std::map<std::string, Probe> cache_;  // /dev/videoN -> result of the last probe
};

// Test/dev source: loops a video file or shows a still image at a fixed rate.
class FileCapture {
public:
  bool open(const std::string& path, int fps, std::string* err);
  void close();
  bool isOpen() const { return open_; }
  bool grab(cv::Mat& bgr, int timeoutMs);
  int width() const { return width_; }
  int height() const { return height_; }
  double decodeMs() const { return decodeMs_; }  // read/decode time of the last frame

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
  bool open_ = false;
  int width_ = 0, height_ = 0;
  double period_ = 1.0 / 30;
  double next_ = 0;
  double decodeMs_ = 0;
};

class V4L2Capture {
public:
  ~V4L2Capture() { close(); }
  // Opens the device and negotiates the closest format to w x h @ fps.
  // MJPEG is preferred (bandwidth), YUYV is the fallback.
  bool open(const std::string& path, int w, int h, int fps, std::string* err);
  void close();
  bool isOpen() const { return fd_ >= 0; }
  // Blocks up to timeoutMs for a frame. Returns false on timeout/error.
  // `bgr` is reused as the decode target when it already has the right size.
  bool grab(cv::Mat& bgr, int timeoutMs);
  int width() const { return width_; }
  int height() const { return height_; }
  std::string format() const { return fourcc_; }
  std::string path() const { return path_; }
  double decodeMs() const { return decodeMs_; }  // decode time of the last frame (excludes the wait)

private:
  struct Buf { void* start = nullptr; size_t length = 0; };
  int fd_ = -1;
  std::vector<Buf> bufs_;
  int width_ = 0, height_ = 0;
  unsigned pixfmt_ = 0;
  std::string fourcc_, path_;
  bool streaming_ = false;
  double decodeMs_ = 0;
  bool decode(const unsigned char* data, size_t len, cv::Mat& bgr);
};
