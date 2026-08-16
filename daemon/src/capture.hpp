#pragma once
#include <opencv2/core.hpp>
#include <memory>
#include <string>
#include <vector>

struct CameraInfo {
  std::string path;     // /dev/videoN
  std::string name;     // card name
  std::string bus;      // bus_info (stable across reboots for the same port)
  std::string key;      // "usb-<vid>-<pid>-<serial>" for USB cameras (used for per-camera hide rules), else ""
};

// Enumerate real capture-capable cameras. Skips metadata nodes, non-capture
// nodes and the given loopback path. One entry per physical device.
std::vector<CameraInfo> enumerateCameras(const std::string& excludePath);

// Test/dev source: loops a video file or shows a still image at a fixed rate.
class FileCapture {
public:
  bool open(const std::string& path, int fps, std::string* err);
  void close();
  bool isOpen() const { return open_; }
  bool grab(cv::Mat& bgr, int timeoutMs);
  int width() const { return width_; }
  int height() const { return height_; }

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
  bool open_ = false;
  int width_ = 0, height_ = 0;
  double period_ = 1.0 / 30;
  double next_ = 0;
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
  bool grab(cv::Mat& bgr, int timeoutMs);
  int width() const { return width_; }
  int height() const { return height_; }
  std::string format() const { return fourcc_; }
  std::string path() const { return path_; }

private:
  struct Buf { void* start = nullptr; size_t length = 0; };
  int fd_ = -1;
  std::vector<Buf> bufs_;
  int width_ = 0, height_ = 0;
  unsigned pixfmt_ = 0;
  std::string fourcc_, path_;
  bool streaming_ = false;
  bool decode(const unsigned char* data, size_t len, cv::Mat& bgr);
};
