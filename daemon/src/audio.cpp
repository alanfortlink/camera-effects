#include "audio.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/pod/builder.h>
#include <spa/utils/dict.h>
#include <string.h>

#include <algorithm>
#include <cstdio>

const char* AudioEngine::kNodeName = "camera-effects-mic";

namespace {
const char* dictGet(const spa_dict* d, const char* k) {
  const char* v = d ? spa_dict_lookup(d, k) : nullptr;
  return v ? v : "";
}
}  // namespace

struct AudioCallbacks {
  static void captureProcess(void* d) { static_cast<AudioEngine*>(d)->onCaptureProcess(); }
  static void playbackProcess(void* d) { static_cast<AudioEngine*>(d)->onPlaybackProcess(); }
  static void listenProcess(void* d) { static_cast<AudioEngine*>(d)->onListenProcess(); }
  static void listenState(void* d, pw_stream_state o, pw_stream_state s, const char* e) { static_cast<AudioEngine*>(d)->onListenState((int)o, (int)s, e); }
  static void nudgeProcess(void* d) { static_cast<AudioEngine*>(d)->onNudgeProcess(); }
  static void captureParam(void* d, uint32_t id, const spa_pod* p) { static_cast<AudioEngine*>(d)->onCaptureParam(id, p); }
  static void playbackParam(void* d, uint32_t id, const spa_pod* p) { static_cast<AudioEngine*>(d)->onPlaybackParam(id, p); }
  static void captureState(void* d, pw_stream_state o, pw_stream_state s, const char* e) { static_cast<AudioEngine*>(d)->onCaptureState((int)o, (int)s, e); }
  static void playbackState(void* d, pw_stream_state o, pw_stream_state s, const char* e) { static_cast<AudioEngine*>(d)->onPlaybackState((int)o, (int)s, e); }
  static void coreError(void* d, uint32_t id, int seq, int res, const char* m) { static_cast<AudioEngine*>(d)->onCoreError(id, seq, res, m); }
  static void global(void* d, uint32_t id, uint32_t, const char* type, uint32_t, const spa_dict* props) {
    static_cast<AudioEngine*>(d)->onGlobal(id, type, props);
  }
  static void globalRemove(void* d, uint32_t id) { static_cast<AudioEngine*>(d)->onGlobalRemove(id); }
};

static const pw_core_events kCoreEvents = [] {
  pw_core_events ev{};
  ev.version = PW_VERSION_CORE_EVENTS;
  ev.error = AudioCallbacks::coreError;
  return ev;
}();

static const pw_registry_events kRegistryEvents = [] {
  pw_registry_events ev{};
  ev.version = PW_VERSION_REGISTRY_EVENTS;
  ev.global = AudioCallbacks::global;
  ev.global_remove = AudioCallbacks::globalRemove;
  return ev;
}();

static const pw_stream_events kCaptureEvents = [] {
  pw_stream_events ev{};
  ev.version = PW_VERSION_STREAM_EVENTS;
  ev.state_changed = AudioCallbacks::captureState;
  ev.param_changed = AudioCallbacks::captureParam;
  ev.process = AudioCallbacks::captureProcess;
  return ev;
}();

static const pw_stream_events kPlaybackEvents = [] {
  pw_stream_events ev{};
  ev.version = PW_VERSION_STREAM_EVENTS;
  ev.state_changed = AudioCallbacks::playbackState;
  ev.param_changed = AudioCallbacks::playbackParam;
  ev.process = AudioCallbacks::playbackProcess;
  return ev;
}();

static const pw_stream_events kListenEvents = [] {
  pw_stream_events ev{};
  ev.version = PW_VERSION_STREAM_EVENTS;
  ev.state_changed = AudioCallbacks::listenState;
  ev.process = AudioCallbacks::listenProcess;
  return ev;
}();

static const pw_stream_events kNudgeEvents = [] {
  pw_stream_events ev{};
  ev.version = PW_VERSION_STREAM_EVENTS;
  ev.process = AudioCallbacks::nudgeProcess;
  return ev;
}();

AudioEngine::~AudioEngine() { stop(); }

// ---------------------------------------------------------------------------
// Lifecycle

bool AudioEngine::start(const std::string& label, std::string* err) {
  std::string e;
  bool ok = startImpl(label, &e);
  status_ = ok ? "ok" : e;
  if (ok) { everConnected_ = true; okSince_ = -1; }
  if (err) *err = e;
  return ok;
}

bool AudioEngine::startImpl(const std::string& label, std::string* err) {
  stop();
  label_ = label;
  failed_ = false;
  ring_.assign(kRing, 0.f);
  work_.assign(1 << 14, 0.f);
  rw_ = rr_ = rl_ = 0;
  pw_init(nullptr, nullptr);
  loop_ = pw_thread_loop_new("camera-effects-audio", nullptr);
  if (!loop_) { if (err) *err = "pw_thread_loop_new failed"; return false; }
  context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
  if (!context_) { if (err) *err = "pw_context_new failed"; stop(); return false; }
  pw_thread_loop_lock(loop_);
  if (pw_thread_loop_start(loop_) < 0) { pw_thread_loop_unlock(loop_); if (err) *err = "pw_thread_loop_start failed"; stop(); return false; }
  // The client name is how the "hide microphones" WirePlumber rule recognises
  // us and leaves the real mics visible to this connection alone.
  core_ = pw_context_connect(context_, pw_properties_new(PW_KEY_APP_NAME, "Camera Effects",
                                                        PW_KEY_APP_ID, "alanfortlink.camera-effects",
                                                        "camera-effects.client", "true", nullptr), 0);
  if (!core_) { pw_thread_loop_unlock(loop_); if (err) *err = "cannot connect to PipeWire"; stop(); return false; }
  coreHook_ = new spa_hook{};
  pw_core_add_listener(core_, coreHook_, &kCoreEvents, this);
  registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
  if (registry_) {
    registryHook_ = new spa_hook{};
    pw_registry_add_listener(registry_, registryHook_, &kRegistryEvents, this);
  }

  // The node apps pick as their microphone.
  pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_CLASS, "Audio/Source",
      PW_KEY_MEDIA_TYPE, "Audio",
      PW_KEY_MEDIA_CATEGORY, "Capture",
      PW_KEY_MEDIA_ROLE, "Communication",
      PW_KEY_NODE_NAME, kNodeName,
      PW_KEY_NODE_DESCRIPTION, label.c_str(),
      PW_KEY_NODE_NICK, label.c_str(),
      PW_KEY_NODE_LINK_GROUP, "camera-effects-mic",
      PW_KEY_DEVICE_API, "camera-effects-server",
      nullptr);
  source_ = pw_stream_new(core_, label.c_str(), props);
  if (!source_) { pw_thread_loop_unlock(loop_); if (err) *err = "pw_stream_new failed"; stop(); return false; }
  sourceHook_ = new spa_hook{};
  pw_stream_add_listener(source_, sourceHook_, &kPlaybackEvents, this);

  uint8_t buffer[512];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
  spa_audio_info_raw info{};
  info.format = SPA_AUDIO_FORMAT_F32;
  info.rate = 48000;
  info.channels = 1;
  info.position[0] = SPA_AUDIO_CHANNEL_MONO;
  const spa_pod* params[1] = { spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info) };
  int r = pw_stream_connect(source_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                            (pw_stream_flags)(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), params, 1);
  pw_thread_loop_unlock(loop_);
  if (r < 0) { if (err) *err = "pw_stream_connect failed"; stop(); return false; }
  return true;
}

void AudioEngine::stop() {
  if (loop_) pw_thread_loop_lock(loop_);
  closeListen();
  closeNudge();
  closeCapture();
  if (source_) { pw_stream_destroy(source_); source_ = nullptr; }
  if (sourceHook_) { delete sourceHook_; sourceHook_ = nullptr; }
  if (registryHook_) { spa_hook_remove(registryHook_); delete registryHook_; registryHook_ = nullptr; }
  if (registry_) { pw_proxy_destroy((pw_proxy*)registry_); registry_ = nullptr; }
  if (coreHook_) { spa_hook_remove(coreHook_); delete coreHook_; coreHook_ = nullptr; }
  if (core_) { pw_core_disconnect(core_); core_ = nullptr; }
  if (loop_) pw_thread_loop_unlock(loop_);
  if (loop_) pw_thread_loop_stop(loop_);
  if (context_) { pw_context_destroy(context_); context_ = nullptr; }
  if (loop_) { pw_thread_loop_destroy(loop_); loop_ = nullptr; }
  active_ = false;
  capturing_ = false;
  listening_ = false;
  consumers_ = 0;
  failed_ = false;
  // Retry timers describe a world that no longer exists after a teardown.
  captureRetryAt_ = nudgeRetryAt_ = listenRetryAt_ = 0;
  listenFails_ = 0;
  sourceDirty_ = false;
  captureLost_ = false;
  listenLost_ = false;
  { std::lock_guard<std::mutex> lk(mu_); sources_.clear(); links_.clear(); currentSource_.clear(); sourceNode_ = captureNode_ = 0xffffffffu; }
}

// The capture stream exists only while the mic is wanted, so the recording
// light (and every app's "in use" indicator) follows real use.
bool AudioEngine::openCapture(std::string* err) {
  if (capture_) return true;
  std::string target;
  { std::lock_guard<std::mutex> lk(mu_); target = wantedSource_; }
  pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio",
      PW_KEY_MEDIA_CATEGORY, "Capture",
      PW_KEY_MEDIA_ROLE, "Communication",
      PW_KEY_NODE_NAME, "camera-effects-mic-input",
      PW_KEY_NODE_DESCRIPTION, (label_ + " input").c_str(),
      PW_KEY_NODE_LINK_GROUP, "camera-effects-mic",
      PW_KEY_NODE_AUTOCONNECT, "true",
      nullptr);
  if (!target.empty()) pw_properties_set(props, PW_KEY_TARGET_OBJECT, target.c_str());
  capture_ = pw_stream_new(core_, "Microphone Effects input", props);
  if (!capture_) { if (err) *err = "capture pw_stream_new failed"; return false; }
  captureHook_ = new spa_hook{};
  pw_stream_add_listener(capture_, captureHook_, &kCaptureEvents, this);
  uint8_t buffer[512];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
  spa_audio_info_raw info{};
  info.format = SPA_AUDIO_FORMAT_F32;
  info.rate = 48000;
  info.channels = 1;
  info.position[0] = SPA_AUDIO_CHANNEL_MONO;
  const spa_pod* params[1] = { spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info) };
  int r = pw_stream_connect(capture_, PW_DIRECTION_INPUT, PW_ID_ANY,
                            (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
                            params, 1);
  if (r < 0) { if (err) *err = "capture pw_stream_connect failed"; closeCapture(); return false; }
  // Size the DSP here, on the loop thread, while no callback can be running:
  // doing it lazily from the first block would malloc ~170 KB inside the
  // real-time callback. The lazy path in MicProcessor::process stays as a
  // backstop for a format we did not ask for.
  proc_.setRate(procRate_.load());
  { std::lock_guard<std::mutex> lk(mu_); currentSource_ = target; }
  return true;
}

void AudioEngine::closeCapture() {
  if (capture_) { pw_stream_destroy(capture_); capture_ = nullptr; }
  if (captureHook_) { delete captureHook_; captureHook_ = nullptr; }
  capturing_ = false;
  captureLost_ = false;   // our own teardown is not a loss (see closeListen)
  // The indices are deliberately left alone: they are monotonic, the readers
  // run in the data thread and resetting them here would make a reader replay
  // whatever was last written. They simply stop advancing until we write again.
  proc_.clearLevels();
  proc_.reset();   // no reverb tail or denoise backlog from before the mic closed
  { std::lock_guard<std::mutex> lk(mu_); currentSource_.clear(); captureNode_ = 0xffffffffu; }
}

// "Listen to this microphone": a plain playback stream on the default sink fed
// from the same ring the virtual source reads. It is deliberately not in the
// mic link group — it follows the speakers' clock, not the microphone's — so a
// little drift is absorbed by the ring (see readRing).
bool AudioEngine::openListen(std::string* err) {
  if (listen_) return true;
  pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio",
      PW_KEY_MEDIA_CATEGORY, "Playback",
      PW_KEY_NODE_NAME, "camera-effects-mic-listen",
      PW_KEY_NODE_DESCRIPTION, (label_ + " (listening)").c_str(),
      PW_KEY_NODE_AUTOCONNECT, "true",
      nullptr);
  listen_ = pw_stream_new(core_, "Microphone Effects listen", props);
  if (!listen_) { if (err) *err = "listen pw_stream_new failed"; return false; }
  listenHook_ = new spa_hook{};
  pw_stream_add_listener(listen_, listenHook_, &kListenEvents, this);
  uint8_t buffer[512];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
  spa_audio_info_raw info{};
  info.format = SPA_AUDIO_FORMAT_F32;
  info.rate = 48000;
  info.channels = 1;
  info.position[0] = SPA_AUDIO_CHANNEL_MONO;
  const spa_pod* params[1] = { spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info) };
  // Start just behind the write head: at the live edge every cycle in which
  // the sink runs before the microphone underruns and clicks. This has to
  // happen before connect(), while no callback can be reading it yet.
  unsigned w = rw_.load();
  rl_.store(w - std::min(w, 960u));
  listenLost_ = false;     // (see closeListen: our own teardown must not look like a loss)
  int r = pw_stream_connect(listen_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                            (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
                            params, 1);
  if (r < 0) { if (err) *err = "listen pw_stream_connect failed"; closeListen(); return false; }
  return true;
}

void AudioEngine::closeListen() {
  if (listen_) { pw_stream_destroy(listen_); listen_ = nullptr; }
  if (listenHook_) { delete listenHook_; listenHook_ = nullptr; }
  listening_ = false;
  // Destroying a stream walks it back to UNCONNECTED, which reaches
  // onListenState like any other loss. Left set, that flag would make the next
  // maintain() tear down the stream we are about to open, and the one after
  // that, for ever: a burst of sound every few seconds instead of playback.
  listenLost_ = false;
}

// Bluetooth microphones stay silent until the headset (HSP/HFP) profile is
// selected, and WirePlumber only switches into it when a plain capture stream
// links to the device's source node. Our own capture is part of a filter (it
// carries node.link-group, like every filter chain does) and WirePlumber
// deliberately skips those, so metering or listening to a headset mic with no
// app on the virtual one would record silence — the profile would never
// change. This second, tiny stream on the same node is a plain one, so it asks
// for the switch; its audio is thrown away, we only keep the profile it buys.
bool AudioEngine::openNudge(const std::string& target, std::string* err) {
  if (nudge_) return true;
  pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio",
      PW_KEY_MEDIA_CATEGORY, "Capture",
      PW_KEY_MEDIA_ROLE, "Communication",
      PW_KEY_NODE_NAME, "camera-effects-mic-headset",
      PW_KEY_NODE_DESCRIPTION, (label_ + " (headset profile)").c_str(),
      PW_KEY_TARGET_OBJECT, target.c_str(),
      PW_KEY_NODE_AUTOCONNECT, "true",
      nullptr);
  nudge_ = pw_stream_new(core_, "Microphone Effects headset profile", props);
  if (!nudge_) { if (err) *err = "headset pw_stream_new failed"; return false; }
  nudgeHook_ = new spa_hook{};
  pw_stream_add_listener(nudge_, nudgeHook_, &kNudgeEvents, this);
  uint8_t buffer[512];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
  spa_audio_info_raw info{};
  info.format = SPA_AUDIO_FORMAT_F32;
  info.rate = 48000;
  info.channels = 1;
  info.position[0] = SPA_AUDIO_CHANNEL_MONO;
  const spa_pod* params[1] = { spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info) };
  int r = pw_stream_connect(nudge_, PW_DIRECTION_INPUT, PW_ID_ANY,
                            (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
                            params, 1);
  if (r < 0) { if (err) *err = "headset pw_stream_connect failed"; closeNudge(); return false; }
  nudgeTarget_ = target;
  return true;
}

void AudioEngine::closeNudge() {
  if (nudge_) { pw_stream_destroy(nudge_); nudge_ = nullptr; }
  if (nudgeHook_) { delete nudgeHook_; nudgeHook_ = nullptr; }
  nudgeTarget_.clear();
}

std::string AudioEngine::maintain(double now) {
  bool healthy = source_ && !failed_.load();
  if (!healthy) {
    okSince_ = -1;
    if (source_) {
      fprintf(stderr, "camera-effects-server: mic stream lost, reconnecting in %.0f s\n", backoff_);
      stop();
      status_ = "reconnecting";
      retryAt_ = now + backoff_;
      backoff_ = std::min(backoff_ * 2, 30.0);
      return status_;
    }
    if (!everConnected_ && retryAt_ == 0) retryAt_ = now + backoff_;
    if (now < retryAt_) return status_;
    std::string err;
    if (start(label_, &err)) { fprintf(stderr, "camera-effects-server: microphone node back\n"); return status_; }
    status_ = err;
    retryAt_ = now + backoff_;
    backoff_ = std::min(backoff_ * 2, 30.0);
    return status_;
  }
  if (okSince_ < 0) okSince_ = now;
  if (now - okSince_ > 30) backoff_ = 2;
  status_ = "ok";

  // Open / close / re-target the real mic as the demand changes. Muted means
  // the microphone is not opened at all (apps get silence from the virtual
  // one): a mute that leaves the recording light on is not a mute.
  // Muted means the microphone is not opened at all, so listening to it while
  // muted would only play silence: the switch waits until the mute is lifted.
  bool wantListen = listenWanted_.load() && !muted_.load();
  bool want = (active_.load() || monitor_.load() || wantListen) && !muted_.load();
  bool retarget = sourceDirty_.exchange(false) || captureLost_.exchange(false);
  if (capture_ && (!want || retarget)) {
    pw_thread_loop_lock(loop_);
    closeCapture();
    pw_thread_loop_unlock(loop_);
    if (!want) fprintf(stderr, "camera-effects-server: microphone released\n");
  }
  if (want && !capture_ && now >= captureRetryAt_) {
    std::string err;
    pw_thread_loop_lock(loop_);
    bool ok = openCapture(&err);
    pw_thread_loop_unlock(loop_);
    // Without this a microphone that refuses to open would be created and
    // destroyed on every tick, under the loop lock, for as long as it is wanted.
    if (!ok) { status_ = err; captureRetryAt_ = now + 2; return status_; }
    captureRetryAt_ = 0;
    std::string cur;
    { std::lock_guard<std::mutex> lk(mu_); cur = currentSource_; }
    fprintf(stderr, "camera-effects-server: microphone open (%s)\n", cur.empty() ? "default source" : cur.c_str());
  }
  // Bluetooth: hold the headset profile open for as long as we read that mic.
  std::string bt;
  if (capture_) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [id, src] : sources_)
      if (src.bluetooth && src.name == (currentSource_.empty() ? wantedSource_ : currentSource_)) { bt = src.name; break; }
  }
  if (nudge_ && bt != nudgeTarget_) {
    pw_thread_loop_lock(loop_);
    closeNudge();
    pw_thread_loop_unlock(loop_);
  }
  if (!bt.empty() && !nudge_ && now >= nudgeRetryAt_) {
    std::string err;
    pw_thread_loop_lock(loop_);
    bool ok = openNudge(bt, &err);
    pw_thread_loop_unlock(loop_);
    if (ok) fprintf(stderr, "camera-effects-server: asking for the headset profile on %s\n", bt.c_str());
    else nudgeRetryAt_ = now + 5;   // the profile is a nicety; do not thrash for it
  }

  if (listen_ && !wantListen) {
    pw_thread_loop_lock(loop_);
    closeListen();
    pw_thread_loop_unlock(loop_);
  }
  if (!wantListen) { listenFails_ = 0; listenRetryAt_ = 0; }   // a fresh switch-on starts fresh
  // The speakers can go away under us (a Bluetooth profile switch destroys the
  // sink node, and the switch above is one cause of that): reopen the stream so
  // it lands on whatever the default sink is now. A stream that keeps dying is
  // backed off instead of retried at speed — chopped sound every couple of
  // seconds is worse than none.
  if (listen_ && wantListen && listenLost_.exchange(false)) {
    bool quick = now - listenOpenedAt_ < 5;
    pw_thread_loop_lock(loop_);
    closeListen();
    pw_thread_loop_unlock(loop_);
    listenFails_ = quick ? listenFails_ + 1 : 0;
    double wait = std::min(15.0, 1.0 * (1 << std::min(4, listenFails_)));
    listenRetryAt_ = now + (listenFails_ ? wait : 1.0);
    fprintf(stderr, "camera-effects-server: listen stream lost, retrying in %.0f s\n", listenRetryAt_ - now);
  }
  if (wantListen && !listen_ && now >= listenRetryAt_) {
    std::string err;
    pw_thread_loop_lock(loop_);
    bool ok = openListen(&err);
    pw_thread_loop_unlock(loop_);
    if (!ok) { status_ = err; listenRetryAt_ = now + 5; return status_; }
    listenOpenedAt_ = now;
  }
  return status_;
}

// ---------------------------------------------------------------------------
// Settings and source selection

void AudioEngine::setSettings(const MicSettings& s) {
  std::lock_guard<std::mutex> lk(setMu_);
  pending_ = s;
  pendingDirty_ = true;
}

void AudioEngine::setSource(const std::string& nodeName) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (wantedSource_ == nodeName) return;
    wantedSource_ = nodeName;
  }
  sourceDirty_ = true;   // maintain() reconnects the capture stream
}

std::vector<MicSourceInfo> AudioEngine::sources() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<MicSourceInfo> v;
  v.reserve(sources_.size());
  for (auto& [id, s] : sources_) v.push_back(s);
  std::sort(v.begin(), v.end(), [](const MicSourceInfo& a, const MicSourceInfo& b) { return a.name < b.name; });
  return v;
}

MicSourceInfo AudioEngine::currentSource() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::string want = currentSource_.empty() ? wantedSource_ : currentSource_;
  for (auto& [id, s] : sources_) if (s.name == want) return s;
  MicSourceInfo s;
  s.name = want;
  return s;
}

// ---------------------------------------------------------------------------
// Registry: which microphones exist

void AudioEngine::onGlobal(unsigned id, const char* type, const spa_dict* props) {
  if (type && strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
    unsigned out = (unsigned)strtoul(dictGet(props, PW_KEY_LINK_OUTPUT_NODE), nullptr, 10);
    unsigned in = (unsigned)strtoul(dictGet(props, PW_KEY_LINK_INPUT_NODE), nullptr, 10);
    std::lock_guard<std::mutex> lk(mu_);
    links_[id] = { out, in };
    updateLinks();
    return;
  }
  if (!type || strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;
  std::string cls = dictGet(props, PW_KEY_MEDIA_CLASS);
  if (cls != "Audio/Source" && cls != "Audio/Source/Virtual") return;
  std::string name = dictGet(props, PW_KEY_NODE_NAME);
  if (name.rfind("camera-effects", 0) == 0) return;   // our own virtual mic
  MicSourceInfo s;
  s.id = id;
  s.name = name;
  s.description = dictGet(props, PW_KEY_NODE_DESCRIPTION);
  if (s.description.empty()) s.description = dictGet(props, PW_KEY_NODE_NICK);
  if (s.description.empty()) s.description = name;
  s.bluetooth = std::string(dictGet(props, PW_KEY_DEVICE_API)) == "bluez5" || name.rfind("bluez", 0) == 0;
  std::lock_guard<std::mutex> lk(mu_);
  sources_[id] = s;
  updateLinks();   // a link may have arrived before the node it names
}

void AudioEngine::onGlobalRemove(unsigned id) {
  std::lock_guard<std::mutex> lk(mu_);
  sources_.erase(id);
  if (links_.erase(id)) updateLinks();
}

// Who is listening to the virtual mic (there may be several apps), and which
// real microphone ended up feeding us — with "Default" selected that is the
// only way to name it.
void AudioEngine::updateLinks() {
  int n = 0;
  std::string feeding;
  for (auto& [id, l] : links_) {
    if (sourceNode_ != 0xffffffffu && l.first == sourceNode_) n++;
    if (captureNode_ != 0xffffffffu && l.second == captureNode_) {
      auto it = sources_.find(l.first);
      if (it != sources_.end()) feeding = it->second.name;
    }
  }
  consumers_ = n;
  active_ = n > 0;
  if (!feeding.empty()) currentSource_ = feeding;
}

// ---------------------------------------------------------------------------
// Stream callbacks

void AudioEngine::onCoreError(unsigned id, int, int res, const char* message) {
  if (id != PW_ID_CORE) return;
  fprintf(stderr, "camera-effects-server: pipewire (audio) core error: %s (%d)\n", message ? message : "", res);
  failed_ = true;
}

void AudioEngine::onPlaybackState(int old, int state, const char* error) {
  if (error) fprintf(stderr, "camera-effects-server: microphone node error: %s\n", error);
  if (state == PW_STREAM_STATE_ERROR || (state == PW_STREAM_STATE_UNCONNECTED && old > PW_STREAM_STATE_CONNECTING)) failed_ = true;
  // active_ comes from the links (see updateLinks): a source stream sits in
  // STREAMING with or without anyone listening.
  if (state != PW_STREAM_STATE_STREAMING) { active_ = false; consumers_ = 0; }
  // The node id only exists once the stream is configured.
  if (state >= PW_STREAM_STATE_PAUSED) {
    std::lock_guard<std::mutex> lk(mu_);
    sourceNode_ = pw_stream_get_node_id(source_);
    updateLinks();
  }
}

void AudioEngine::onListenState(int old, int state, const char* error) {
  if (error) fprintf(stderr, "camera-effects-server: microphone listen error: %s\n", error);
  listening_ = state == PW_STREAM_STATE_STREAMING;
  if (state == PW_STREAM_STATE_ERROR || (state == PW_STREAM_STATE_UNCONNECTED && old > PW_STREAM_STATE_CONNECTING)) listenLost_ = true;
}

// The headset-profile stream's audio is not wanted: recycle the buffers.
void AudioEngine::onNudgeProcess() {
  pw_buffer* pb = pw_stream_dequeue_buffer(nudge_);
  if (pb) pw_stream_queue_buffer(nudge_, pb);
}

void AudioEngine::onCaptureState(int old, int state, const char* error) {
  if (error) fprintf(stderr, "camera-effects-server: microphone input error: %s\n", error);
  // A microphone can be unplugged mid-call. Without this the dead stream is
  // never torn down (maintain only closes it when it is no longer wanted), and
  // apps would get silence for ever while everything still reported "ok".
  if (state == PW_STREAM_STATE_ERROR || (state == PW_STREAM_STATE_UNCONNECTED && old > PW_STREAM_STATE_CONNECTING)) captureLost_ = true;
  capturing_ = state == PW_STREAM_STATE_STREAMING;
  if (!capturing_.load()) proc_.clearLevels();
  if (capture_ && state >= PW_STREAM_STATE_PAUSED) {
    std::lock_guard<std::mutex> lk(mu_);
    captureNode_ = pw_stream_get_node_id(capture_);
    updateLinks();
  }
}

void AudioEngine::onPlaybackParam(unsigned id, const void* param) {
  if (!param || id != SPA_PARAM_Format) return;
  spa_audio_info_raw info{};
  if (spa_format_audio_raw_parse((const spa_pod*)param, &info) < 0) return;
  if (info.rate > 0) rate_ = (int)info.rate;
}

void AudioEngine::onCaptureParam(unsigned id, const void* param) {
  if (!param || id != SPA_PARAM_Format) return;
  spa_audio_info_raw info{};
  if (spa_format_audio_raw_parse((const spa_pod*)param, &info) < 0) return;
  // The processor's buffers are sized by the data thread itself: resizing them
  // here would be a resize under a callback that may be running right now.
  if (info.rate > 0) { rate_ = (int)info.rate; procRate_ = (int)info.rate; }
}

// Real-time: read the mic block, run the effects, hand the result to the
// source stream through the ring.
void AudioEngine::onCaptureProcess() {
  pw_buffer* pb = pw_stream_dequeue_buffer(capture_);
  if (!pb) return;
  spa_buffer* buf = pb->buffer;
  spa_data& d = buf->datas[0];
  if (d.data && d.chunk) {
    uint32_t off = std::min(d.chunk->offset, d.maxsize);
    uint32_t size = std::min(d.chunk->size, d.maxsize - off);
    int n = (int)(size / sizeof(float));
    if (n > (int)work_.size()) n = (int)work_.size();
    if (n > 0) {
      memcpy(work_.data(), (const uint8_t*)d.data + off, n * sizeof(float));
      proc_.setRate(procRate_.load());   // no-op unless the format changed
      // Settings arrive from the control thread; never block the data thread
      // for them (the next block picks them up instead).
      if (pendingDirty_.load() && setMu_.try_lock()) {
        proc_.setSettings(pending_);
        pendingDirty_ = false;
        setMu_.unlock();
      }
      if (muted_.load()) {
        std::fill(work_.begin(), work_.begin() + n, 0.f);
        // Drop what the effects were still holding (a reverb tail, the denoise
        // queue), so unmuting cannot hand apps a scrap of pre-mute audio.
        if (!wasMuted_) { proc_.reset(); wasMuted_ = true; }
        proc_.clearLevels();
      } else {
        wasMuted_ = false;
        proc_.process(work_.data(), n);
      }
      // Append only: each reader trims its own backlog (readRing), so a reader
      // that is not being called cannot drag the other one back.
      unsigned w = rw_.load(std::memory_order_relaxed);
      for (int i = 0; i < n; i++) ring_[(w + i) & (kRing - 1)] = work_[i];
      rw_.store(w + (unsigned)n, std::memory_order_release);
    }
  }
  pw_stream_queue_buffer(capture_, pb);
}

// Real-time: one reader's share of what the capture side left; returns how
// many samples it actually filled (never more than the ring can serve). A reader that
// falls more than half a ring behind (nothing linked to it for a while, or the
// speakers' clock running slower than the microphone's) is snapped back to the
// live edge rather than allowed to play further and further into the past.
int AudioEngine::readRing(std::atomic<unsigned>& rp, float* out, int n) {
  if (n <= 0) return 0;
  if (n > kRing / 4) n = kRing / 4;   // a quantum that big cannot be served from history
  unsigned w = rw_.load(std::memory_order_acquire);
  unsigned r = rp.load(std::memory_order_relaxed);
  int avail = (int)(w - r);
  if (avail < 0) { r = w; avail = 0; }                                  // capture restarted
  if (avail > kRing / 2) { r = w - (unsigned)n; avail = n; }
  int take = std::min(n, avail);
  for (int i = 0; i < take; i++) out[i] = ring_[(r + i) & (kRing - 1)];
  for (int i = take; i < n; i++) out[i] = 0.f;   // mic not open (or not there yet): silence
  rp.store(r + (unsigned)take, std::memory_order_release);
  return n;
}

// Real-time: fill the block apps read, from whatever the capture side left.
void AudioEngine::onPlaybackProcess() {
  pw_buffer* pb = pw_stream_dequeue_buffer(source_);
  if (!pb) return;
  spa_buffer* buf = pb->buffer;
  spa_data& d = buf->datas[0];
  const int stride = sizeof(float);
  int n = d.data ? (int)(d.maxsize / stride) : 0;
  if (pb->requested > 0) n = std::min<int>(n, (int)pb->requested);
  if (d.chunk) {
    n = readRing(rr_, (float*)d.data, n);   // the reader clamps; the chunk must say so
    d.chunk->offset = 0;
    d.chunk->stride = stride;
    d.chunk->size = std::max(0, n) * stride;
  }
  pw_stream_queue_buffer(source_, pb);
}

// Real-time: the same block again, for the speakers, while "listen" is on.
void AudioEngine::onListenProcess() {
  pw_buffer* pb = pw_stream_dequeue_buffer(listen_);
  if (!pb) return;
  spa_buffer* buf = pb->buffer;
  spa_data& d = buf->datas[0];
  const int stride = sizeof(float);
  int n = d.data ? (int)(d.maxsize / stride) : 0;
  if (pb->requested > 0) n = std::min<int>(n, (int)pb->requested);
  if (d.chunk) {
    n = readRing(rl_, (float*)d.data, n);
    d.chunk->offset = 0;
    d.chunk->stride = stride;
    d.chunk->size = std::max(0, n) * stride;
  }
  pw_stream_queue_buffer(listen_, pb);
}
