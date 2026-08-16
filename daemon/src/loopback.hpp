#pragma once
#include <atomic>
#include <functional>
#include <opencv2/core.hpp>
#include <string>
#include <thread>

#include <mutex>

// When running setgid (hide-raw mode) the effective gid differs from the
// user's, which makes /proc/<pid>/fd of the user's other processes
// unreadable. The scan drops to the real gid for its duration; anything that
// needs the privileged gid (opening a raw camera) takes this lock meanwhile.
extern std::mutex g_credMutex;

// Find /dev/videoN whose v4l2loopback card label matches. Empty if none.
std::string findLoopbackByLabel(const std::string& label);

// Writer side of a v4l2loopback device. Opened once, format set once, frames
// pushed with write(). Keeping the fd open is what makes the device announce
// capture caps to apps (exclusive_caps=1), even while no frames are flowing.
class LoopbackWriter {
public:
  ~LoopbackWriter() { close(); }
  bool open(const std::string& path, int w, int h, std::string* err);
  void close();
  bool isOpen() const { return fd_ >= 0; }
  bool write(const cv::Mat& bgr);
  int width() const { return w_; }
  int height() const { return h_; }
  std::string path() const { return path_; }

private:
  int fd_ = -1;
  int w_ = 0, h_ = 0;
  std::string path_;
  cv::Mat yuyv_;
};

// Watches who has the loopback device open. Uses inotify (IN_OPEN/IN_CLOSE)
// for immediacy and a periodic /proc scan to reconcile (apps that were
// already attached when we started, missed events).
class ConsumerWatcher {
public:
  ~ConsumerWatcher() { stop(); }
  // ownFdCount: how many fds *this process* holds on the device (excluded).
  void start(const std::string& path, std::function<void(int)> onChange);
  void stop();
  int consumers() const { return consumers_.load(); }

private:
  void run();
  int scanProc();
  std::string path_;
  std::function<void(int)> onChange_;
  std::atomic<int> consumers_{0};
  std::atomic<bool> stop_{false};
  std::thread thread_;
};
