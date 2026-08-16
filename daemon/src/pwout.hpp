#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <opencv2/core.hpp>
#include <string>

struct pw_thread_loop;
struct pw_stream;
struct pw_context;
struct pw_core;
struct spa_hook;

// Native PipeWire camera: a Video/Source node with media.role=Camera, which is
// what portal-based apps (OBS "Camera (PipeWire)", Firefox/Chromium with
// PipeWire camera enabled, GNOME apps) enumerate. Runs next to the v4l2loopback
// output; same frames.
class PipeWireOutput {
public:
  ~PipeWireOutput() { stop(); }
  bool start(int w, int h, int fps, const std::string& label, std::function<void(bool)> onActive, std::string* err);
  void stop();
  bool started() const { return stream_ != nullptr; }
  bool active() const { return active_.load(); }  // some consumer is linked
  // Publish a frame already converted to YUY2 (CV_8UC2, w x h); it is copied
  // for the PipeWire thread and pushed to linked consumers.
  void pushYuyv(const cv::Mat& yuyv);
  // Forget the last frame (consumers get black until the next push).
  void clear();

private:
  friend struct PwCallbacks;
  void onProcess();
  void onParamChanged(uint32_t id, const void* param);
  void onStateChanged(int old, int state, const char* error);

  pw_thread_loop* loop_ = nullptr;
  pw_context* context_ = nullptr;
  pw_core* core_ = nullptr;
  pw_stream* stream_ = nullptr;
  spa_hook* listener_ = nullptr;
  int w_ = 0, h_ = 0, fps_ = 30;
  std::atomic<bool> active_{false};
  std::function<void(bool)> onActive_;
  std::mutex frameMu_;
  cv::Mat yuyv_;         // latest frame, ready to copy into a pw buffer
  bool haveFrame_ = false;
  uint64_t seq_ = 0;
};
