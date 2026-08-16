#include "pwout.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/pod/builder.h>
#include <string.h>

#include <algorithm>
#include <cstdio>

struct PwCallbacks {
  static void process(void* data) { static_cast<PipeWireOutput*>(data)->onProcess(); }
  static void paramChanged(void* data, uint32_t id, const spa_pod* param) { static_cast<PipeWireOutput*>(data)->onParamChanged(id, param); }
  static void stateChanged(void* data, pw_stream_state old, pw_stream_state state, const char* error) {
    static_cast<PipeWireOutput*>(data)->onStateChanged((int)old, (int)state, error);
  }
  static void coreError(void* data, uint32_t id, int seq, int res, const char* message) {
    static_cast<PipeWireOutput*>(data)->onCoreError(id, seq, res, message);
  }
};

static const pw_core_events kCoreEvents = [] {
  pw_core_events ev{};
  ev.version = PW_VERSION_CORE_EVENTS;
  ev.error = PwCallbacks::coreError;
  return ev;
}();

static const pw_stream_events kStreamEvents = {
  PW_VERSION_STREAM_EVENTS,
  nullptr,                     // destroy
  PwCallbacks::stateChanged,   // state_changed
  nullptr,                     // control_info
  nullptr,                     // io_changed
  PwCallbacks::paramChanged,   // param_changed
  nullptr,                     // add_buffer
  nullptr,                     // remove_buffer
  PwCallbacks::process,        // process
  nullptr,                     // drained
  nullptr,                     // command
  nullptr,                     // trigger_done
};

bool PipeWireOutput::start(int w, int h, int fps, const std::string& label, std::function<void(bool)> onActive, std::string* err) {
  std::string e;
  bool ok = startImpl(w, h, fps, label, std::move(onActive), &e);
  status_ = ok ? "ok" : e;
  if (ok) { everConnected_ = true; okSince_ = -1; }
  if (err) *err = e;
  return ok;
}

bool PipeWireOutput::startImpl(int w, int h, int fps, const std::string& label, std::function<void(bool)> onActive, std::string* err) {
  stop();
  w_ = w; h_ = h; fps_ = fps > 0 ? fps : 30;
  label_ = label;
  onActive_ = std::move(onActive);
  failed_ = false;
  pw_init(nullptr, nullptr);
  loop_ = pw_thread_loop_new("camera-effects-pw", nullptr);
  if (!loop_) { if (err) *err = "pw_thread_loop_new failed"; return false; }
  context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
  if (!context_) { if (err) *err = "pw_context_new failed"; stop(); return false; }
  pw_thread_loop_lock(loop_);
  if (pw_thread_loop_start(loop_) < 0) { pw_thread_loop_unlock(loop_); if (err) *err = "pw_thread_loop_start failed"; stop(); return false; }
  core_ = pw_context_connect(context_, nullptr, 0);
  if (!core_) { pw_thread_loop_unlock(loop_); if (err) *err = "cannot connect to PipeWire"; stop(); return false; }
  coreListener_ = new spa_hook{};
  pw_core_add_listener(core_, coreListener_, &kCoreEvents, this);

  pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_CLASS, "Video/Source",
      PW_KEY_MEDIA_ROLE, "Camera",
      PW_KEY_MEDIA_TYPE, "Video",
      PW_KEY_MEDIA_CATEGORY, "Capture",
      PW_KEY_NODE_NAME, "camera-effects",
      PW_KEY_NODE_DESCRIPTION, label.c_str(),
      PW_KEY_NODE_NICK, label.c_str(),
      PW_KEY_DEVICE_API, "camera-effects-server",
      PW_KEY_NODE_DRIVER, "true",
      nullptr);
  pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%d", fps_);
  stream_ = pw_stream_new(core_, label.c_str(), props);
  if (!stream_) { pw_thread_loop_unlock(loop_); if (err) *err = "pw_stream_new failed"; stop(); return false; }
  listener_ = new spa_hook{};
  pw_stream_add_listener(stream_, listener_, &kStreamEvents, this);

  // One fixed format per entry: browsers (libwebrtc) ignore entries whose size
  // is a range. YUY2 is what every camera consumer accepts.
  uint8_t buffer[1024];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
  const spa_pod* params[1];
  spa_video_info_raw info{};
  info.format = SPA_VIDEO_FORMAT_YUY2;
  info.size = SPA_RECTANGLE((uint32_t)w_, (uint32_t)h_);
  info.framerate = SPA_FRACTION((uint32_t)fps_, 1);
  params[0] = spa_format_video_raw_build(&b, SPA_PARAM_EnumFormat, &info);

  int r = pw_stream_connect(stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                            (pw_stream_flags)(PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);
  pw_thread_loop_unlock(loop_);
  if (r < 0) { if (err) *err = "pw_stream_connect failed"; stop(); return false; }
  return true;
}

void PipeWireOutput::stop() {
  if (loop_) pw_thread_loop_lock(loop_);
  if (stream_) { pw_stream_destroy(stream_); stream_ = nullptr; }
  if (listener_) { delete listener_; listener_ = nullptr; }
  if (coreListener_) { spa_hook_remove(coreListener_); delete coreListener_; coreListener_ = nullptr; }
  if (core_) { pw_core_disconnect(core_); core_ = nullptr; }
  if (loop_) pw_thread_loop_unlock(loop_);
  if (loop_) pw_thread_loop_stop(loop_);
  if (context_) { pw_context_destroy(context_); context_ = nullptr; }
  if (loop_) { pw_thread_loop_destroy(loop_); loop_ = nullptr; }
  failed_ = false;
  // Consumers that were linked are gone with the node: tell the owner so it
  // does not keep the camera running for them.
  if (active_.exchange(false) && onActive_) onActive_(false);
}

std::string PipeWireOutput::maintain(double now) {
  bool healthy = stream_ && !failed_.load();
  if (healthy) {
    if (okSince_ < 0) okSince_ = now;
    if (now - okSince_ > 30) backoff_ = 2;  // stable for a while: forget the backoff
    return status_ = "ok";
  }
  okSince_ = -1;
  if (stream_) {  // just failed: tear down now, come back after the backoff
    fprintf(stderr, "camera-effects-server: pipewire stream lost, reconnecting in %.0f s\n", backoff_);
    stop();
    status_ = "reconnecting";
    retryAt_ = now + backoff_;
    backoff_ = std::min(backoff_ * 2, 30.0);
    return status_;
  }
  if (!everConnected_ && retryAt_ == 0) retryAt_ = now + backoff_;  // first start failed: same schedule
  if (now < retryAt_) return status_;
  std::string err;
  if (start(w_, h_, fps_, label_, onActive_, &err)) {
    fprintf(stderr, "camera-effects-server: pipewire output back\n");
    return status_;  // "ok"
  }
  status_ = err;
  retryAt_ = now + backoff_;
  backoff_ = std::min(backoff_ * 2, 30.0);
  return status_;
}

void PipeWireOutput::onStateChanged(int old, int state, const char* error) {
  bool streaming = state == PW_STREAM_STATE_STREAMING;
  if (error) fprintf(stderr, "camera-effects-server: pipewire stream error: %s\n", error);
  // Error, or dropped back to unconnected after having been up: the owner's
  // maintain() re-creates the node (nothing may be torn down from this thread).
  if (state == PW_STREAM_STATE_ERROR || (state == PW_STREAM_STATE_UNCONNECTED && old > PW_STREAM_STATE_CONNECTING)) failed_ = true;
  if (streaming != active_.load()) {
    active_ = streaming;
    if (onActive_) onActive_(streaming);
  }
}

void PipeWireOutput::onCoreError(uint32_t id, int, int res, const char* message) {
  if (id != PW_ID_CORE) return;
  fprintf(stderr, "camera-effects-server: pipewire core error: %s (%d)\n", message ? message : "", res);
  failed_ = true;
}

void PipeWireOutput::onParamChanged(uint32_t id, const void* param) {
  if (!param || id != SPA_PARAM_Format) return;
  spa_video_info_raw info{};
  if (spa_format_video_raw_parse((const spa_pod*)param, &info) < 0) return;
  // Consumers accepted the format; tell them how buffers look.
  uint8_t buffer[1024];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
  const int stride = w_ * 2, size = stride * h_;
  const spa_pod* params[2];
  params[0] = (const spa_pod*)spa_pod_builder_add_object(&b,
      SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
      SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
      SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
      SPA_PARAM_BUFFERS_size, SPA_POD_Int(size),
      SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride),
      SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
  params[1] = (const spa_pod*)spa_pod_builder_add_object(&b,
      SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
      SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
      SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_header)));
  pw_stream_update_params(stream_, params, 2);
}

void PipeWireOutput::onProcess() {
  pw_buffer* pb = pw_stream_dequeue_buffer(stream_);
  if (!pb) return;
  spa_buffer* buf = pb->buffer;
  spa_data& d = buf->datas[0];
  const int stride = w_ * 2, size = stride * h_;
  if (d.data && (int)d.maxsize >= size) {
    std::lock_guard<std::mutex> lk(frameMu_);
    if (haveFrame_ && yuyv_.cols == w_ && yuyv_.rows == h_) memcpy(d.data, yuyv_.data, size);
    else memset(d.data, 0, size);
    d.chunk->offset = 0;
    d.chunk->stride = stride;
    d.chunk->size = size;
    if (auto* h = (spa_meta_header*)spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(spa_meta_header))) {
      h->pts = -1; h->flags = 0; h->seq = (uint32_t)seq_++; h->dts_offset = 0;
    }
  }
  pw_stream_queue_buffer(stream_, pb);
}

void PipeWireOutput::pushYuyv(const cv::Mat& yuyv) {
  if (!stream_ || !active_.load()) return;
  if (yuyv.type() != CV_8UC2 || yuyv.cols != w_ || yuyv.rows != h_) return;
  {
    std::lock_guard<std::mutex> lk(frameMu_);
    yuyv.copyTo(yuyv_);  // reuses the allocation
    haveFrame_ = true;
  }
  // We are the driver: schedule one graph cycle so process() runs now.
  pw_thread_loop_lock(loop_);
  pw_stream_trigger_process(stream_);
  pw_thread_loop_unlock(loop_);
}

void PipeWireOutput::clear() {
  std::lock_guard<std::mutex> lk(frameMu_);
  haveFrame_ = false;
}
