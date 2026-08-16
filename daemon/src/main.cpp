// camera-effects-server — system-wide camera effects daemon for Omarchy.
//
// Reads a physical webcam, applies framing (rotate, fit, zoom, pan) and
// effects (Center Stage, Portrait, Studio Light, background replacement,
// colour filters, fun face filters, Reactions) and publishes the result as a
// virtual V4L2 camera ("Camera Effects") that every app can use. Runs the camera only while some app has the virtual camera
// open (or a control client asked for the preview); with "block" on it never
// opens the camera and feeds a placeholder instead. The panel's preview is a
// small JPEG the daemon writes to the runtime dir while a preview is wanted:
// the loopback lets a single reader negotiate the format, so a second reader
// (the panel) would see nothing while an app streams. A snapshot request
// saves the next output frame as a PNG under ~/Pictures/Camera Effects.
#include <fcntl.h>
#include <linux/videodev2.h>
#include <pwd.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <sys/prctl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <map>
#include <set>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <thread>

#include "block.hpp"
#include "capture.hpp"
#include "effects.hpp"
#include "json.hpp"
#include "loopback.hpp"
#include "pwout.hpp"
#include "server.hpp"

using json = nlohmann::json;
using clk = std::chrono::steady_clock;

namespace {

std::atomic<bool> g_quit{false};
void onSignal(int) { g_quit = true; }

std::string homeDir() {
  const char* h = getenv("HOME");
  if (h && *h) return h;
  passwd* pw = getpwuid(getuid());
  return pw ? pw->pw_dir : "/tmp";
}
std::string xdgConfig() { const char* c = getenv("XDG_CONFIG_HOME"); return c && *c ? c : homeDir() + "/.config"; }
std::string xdgData() { const char* c = getenv("XDG_DATA_HOME"); return c && *c ? c : homeDir() + "/.local/share"; }
std::string xdgRuntime() { const char* c = getenv("XDG_RUNTIME_DIR"); return c && *c ? c : "/tmp"; }
double nowSec() { return std::chrono::duration<double>(clk::now().time_since_epoch()).count(); }
bool fileExists(const std::string& p) { struct stat st{}; return stat(p.c_str(), &st) == 0; }

// ---------------------------------------------------------------------------
// Settings <-> JSON

json settingsToJson(const Settings& s) {
  return json{ { "centerStage", s.centerStage }, { "centerStageIntensity", s.centerStageIntensity }, { "portrait", s.portrait }, { "portraitIntensity", s.portraitIntensity },
               { "studioLight", s.studioLight }, { "studioLightIntensity", s.studioLightIntensity }, { "background", s.background },
               { "backgroundImage", s.backgroundImage }, { "backgroundColor", s.backgroundColor }, { "reactions", s.reactions }, { "mirror", s.mirror },
               { "rotate", s.rotate }, { "filter", s.filter }, { "fun", s.fun },
               { "fit", s.fit }, { "zoom", s.zoom }, { "panX", s.panX }, { "panY", s.panY } };
}

// Framing fields (`prefix` "" for a camera's settings, "block" for the
// placeholder's blockZoom/blockPanX/blockPanY/blockFit), validated and clamped.
void framingFromJson(const json& j, const std::string& prefix, std::string& fit, float& zoom, float& panX, float& panY) {
  auto key = [&](const char* k) { return prefix.empty() ? std::string(k) : prefix + (char)toupper(k[0]) + (k + 1); };
  auto getf = [&](const char* k, float& v, float lo, float hi) {
    std::string kk = key(k);
    if (j.contains(kk) && j[kk].is_number()) { float f = j[kk].get<float>(); if (std::isfinite(f)) v = std::clamp(f, lo, hi); }
  };
  getf("zoom", zoom, 1.f, 4.f); getf("panX", panX, -1.f, 1.f); getf("panY", panY, -1.f, 1.f);
  std::string fk = key("fit");
  if (j.contains(fk) && j[fk].is_string()) fit = j[fk];
  const auto& names = Framing::fitNames();
  if (std::find(names.begin(), names.end(), fit) == names.end()) fit = names[0];
}

void settingsFromJson(const json& j, Settings& s) {
  if (!j.is_object()) return;
  auto getb = [&](const char* k, bool& v) { if (j.contains(k) && j[k].is_boolean()) v = j[k]; };
  auto getf = [&](const char* k, float& v) { if (j.contains(k) && j[k].is_number()) v = std::clamp(j[k].get<float>(), 0.f, 1.f); };
  auto gets = [&](const char* k, std::string& v) { if (j.contains(k) && j[k].is_string()) v = j[k]; };
  getb("centerStage", s.centerStage); getf("centerStageIntensity", s.centerStageIntensity); getb("portrait", s.portrait); getf("portraitIntensity", s.portraitIntensity);
  getb("studioLight", s.studioLight); getf("studioLightIntensity", s.studioLightIntensity);
  gets("background", s.background); gets("backgroundImage", s.backgroundImage); gets("backgroundColor", s.backgroundColor);
  getb("reactions", s.reactions); getb("mirror", s.mirror);
  gets("filter", s.filter); gets("fun", s.fun);
  if (j.contains("rotate") && j["rotate"].is_number()) {
    double r = j["rotate"].get<double>();
    s.rotate = r == 90 || r == 180 || r == 270 ? (int)r : 0;
  }
  framingFromJson(j, "", s.fit, s.zoom, s.panX, s.panY);
  if (s.background != "none" && s.background != "image" && s.background != "color") s.background = "none";
  unsigned rgb;
  if (!parseHexColor(s.backgroundColor, rgb)) s.backgroundColor = Settings().backgroundColor;
  auto oneOf = [](const std::vector<std::string>& names, std::string& v) { if (std::find(names.begin(), names.end(), v) == names.end()) v = "none"; };
  oneOf(Settings::filterNames(), s.filter);
  oneOf(Settings::funNames(), s.fun);
}

// ---------------------------------------------------------------------------
// Client mode: talk to a running daemon.

// `hold`: stay connected after the reply until the daemon goes away or we are
// killed (used by `preview on`, whose effect lasts as long as the connection).
// `untilType`: the reply is not the next line but the first one whose "type"
// is this (a snapshot reply comes with the next frame, after any state pushes);
// its "path" is printed, or its "error" and the exit code is 1.
int cmdClient(const std::string& sockPath, const std::string& request, bool wait, bool hold = false, const char* untilType = nullptr) {
  sockaddr_un addr{};
  if (sockPath.size() >= sizeof(addr.sun_path)) { fprintf(stderr, "socket path too long: %s\n", sockPath.c_str()); return 1; }
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);
  if (connect(fd, (sockaddr*)&addr, sizeof addr) < 0) { fprintf(stderr, "daemon not running (%s)\n", sockPath.c_str()); return 1; }
  timeval tv{ untilType ? 15 : 5, 0 };  // never hang the CLI on a wedged daemon (a snapshot may first have to start the camera)
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  std::string msg = request + "\n";
  send(fd, msg.data(), msg.size(), 0);
  // Read the greeting state + reply.
  std::string acc;
  char buf[8192];
  int lines = 0, rc = 0;
  bool done = false;
  while (!done) {
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n <= 0) { if (untilType) { fprintf(stderr, "no reply from the daemon\n"); rc = 1; } break; }
    acc.append(buf, n);
    size_t pos;
    while (!done && (pos = acc.find('\n')) != std::string::npos) {
      std::string line = acc.substr(0, pos);
      acc.erase(0, pos + 1);
      lines++;
      if (untilType) {
        json j = json::parse(line, nullptr, false);
        if (!j.is_object() || (j.value("type", "") != untilType && j.value("type", "") != "error")) continue;  // an "error" reply ends it too (an older daemon: unknown cmd)
        std::string err = j.value("error", "");
        if (err.empty()) printf("%s\n", j.value("path", "").c_str());
        else { fprintf(stderr, "%s\n", err.c_str()); rc = 1; }
        fflush(stdout);
        done = true;
      } else if (lines == (wait ? 2 : 1)) { printf("%s\n", line.c_str()); fflush(stdout); done = true; }
    }
  }
  if (hold) {
    tv = timeval{ 0, 0 };  // block for good; state pushes are read and dropped
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (recv(fd, buf, sizeof buf, 0) > 0) {}
  }
  close(fd);
  return rc;
}

// ---------------------------------------------------------------------------
// Daemon

struct Config {
  std::string label = "Camera Effects";
  int outW = 1280, outH = 720, fps = 30;
  // Capture size used while Center Stage is on (it needs room to zoom).
  // Otherwise the camera is asked for the output size: a 1080p MJPEG decode
  // costs 3x a 720p one and is the single biggest item on a weak laptop.
  int capW = 1920, capH = 1080;
  std::string preferredCamera;  // bus info or path
  // Block camera (privacy shutter): global, not per camera. While on, no
  // physical camera is opened; consumers get blockSource (image or video
  // file; "" = the built-in "Camera paused" card) instead.
  bool block = false;
  bool previewMirror = true;   // panel-only: show the preview as a mirror (self-view convention); apps are unaffected
  std::string blockSource;
  Framing blockFraming;         // fit/zoom/pan of the placeholder image or video
  Settings settings;            // global settings (used when sameForAll, and as the template for new cameras)
  bool sameForAll = true;
  std::map<std::string, Settings> settingsByCamera;  // bus -> settings
  std::string modelsDir, assetsDir, configPath;
};

void loadConfig(Config& c) {
  std::ifstream f(c.configPath);
  if (!f) return;
  try {
    json j = json::parse(f);
    if (j.contains("loopback") && j["loopback"].is_object() && j["loopback"].contains("label") && j["loopback"]["label"].is_string()) c.label = j["loopback"]["label"];
    // Sizes/fps are hand-editable: accept only numbers in a sane range.
    auto geti = [](const json& o, const char* k, int& v, int lo, int hi) {
      if (o.is_object() && o.contains(k) && o[k].is_number()) v = std::clamp(o[k].get<int>(), lo, hi);
    };
    if (j.contains("output")) {
      geti(j["output"], "width", c.outW, 160, 4096);
      geti(j["output"], "height", c.outH, 120, 4096);
      geti(j["output"], "fps", c.fps, 1, 120);
    }
    if (j.contains("capture")) {
      geti(j["capture"], "width", c.capW, 160, 4096);
      geti(j["capture"], "height", c.capH, 120, 4096);
    }
    // YUYV packs two pixels per sample: the BGR->YUYV conversion (and most
    // cameras) need even sizes, so a hand-edited odd value is rounded down.
    c.outW &= ~1; c.outH &= ~1; c.capW &= ~1; c.capH &= ~1;
    if (j.contains("camera") && j["camera"].is_string()) c.preferredCamera = j["camera"];
    if (j.contains("block") && j["block"].is_boolean()) c.block = j["block"];
    if (j.contains("previewMirror") && j["previewMirror"].is_boolean()) c.previewMirror = j["previewMirror"];
    if (j.contains("blockSource") && j["blockSource"].is_string() && blockSourceValid(j["blockSource"])) c.blockSource = j["blockSource"];
    framingFromJson(j, "block", c.blockFraming.fit, c.blockFraming.zoom, c.blockFraming.panX, c.blockFraming.panY);
    if (j.contains("settings")) settingsFromJson(j["settings"], c.settings);
    if (j.contains("sameForAll") && j["sameForAll"].is_boolean()) c.sameForAll = j["sameForAll"];
    if (j.contains("settingsByCamera") && j["settingsByCamera"].is_object()) {
      for (auto& [bus, sj] : j["settingsByCamera"].items()) { Settings st = c.settings; settingsFromJson(sj, st); c.settingsByCamera[bus] = st; }
    }
  } catch (const std::exception& e) {
    fprintf(stderr, "config: %s\n", e.what());
  }
}

void saveConfig(const Config& c) {
  json byCam = json::object();
  for (auto& [bus, st] : c.settingsByCamera) byCam[bus] = settingsToJson(st);
  json j = { { "output", { { "width", c.outW }, { "height", c.outH }, { "fps", c.fps } } },
             { "capture", { { "width", c.capW }, { "height", c.capH } } },
             { "camera", c.preferredCamera },
             { "block", c.block },
             { "previewMirror", c.previewMirror },
             { "blockSource", c.blockSource },
             { "blockFit", c.blockFraming.fit }, { "blockZoom", c.blockFraming.zoom }, { "blockPanX", c.blockFraming.panX }, { "blockPanY", c.blockFraming.panY },
             { "sameForAll", c.sameForAll },
             { "settings", settingsToJson(c.settings) },
             { "settingsByCamera", byCam } };
  std::string dir = c.configPath.substr(0, c.configPath.rfind('/'));
  mkdir(dir.c_str(), 0755);
  std::string tmp = c.configPath + ".tmp";
  { std::ofstream f(tmp); f << j.dump(2) << "\n"; }
  // The setgid (hide-raw) build would otherwise leave these group camerad.
  (void)!chown(dir.c_str(), (uid_t)-1, getgid());
  (void)!chown(tmp.c_str(), (uid_t)-1, getgid());
  rename(tmp.c_str(), c.configPath.c_str());
}

class Daemon {
public:
  Daemon(Config cfg) : cfg_(std::move(cfg)) {}
  int run();

private:
  Config cfg_;
  std::mutex mu_;
  EffectPipeline fx_;
  LoopbackWriter loop_;
  PipeWireOutput pwOut_;
  std::atomic<bool> pwActive_{false};
  ConsumerWatcher watcher_;
  ControlServer server_;
  CameraEnumerator enumerator_;
  V4L2Capture cap_;
  FileCapture file_;   // dev/test source when the selected "camera" is a file path
  bool useFile_ = false;
  BlockFeed block_;    // placeholder source while cfg_.block is on
  std::atomic<int> consumers_{0};
  std::atomic<bool> stateDirty_{true};
  // State shown to clients. Written by the main thread and read by the
  // server thread (stateJson): every write happens under mu_.
  std::vector<CameraInfo> cameras_;
  CameraInfo current_;
  bool running_ = false;
  double fps_ = 0;
  double procMs_ = 0;   // EMA of the displayed per-frame cost: decode + effects + output conversion/write
  int tier_ = 0;        // adaptive quality tier (0 = full, 2 = cheapest), see updateTier()
  bool reactionsActive_ = false;  // a reaction is playing or queued (mirrors fx_.reactionsActive())
  // Tier controller state (main thread only). ctrlMs_ is the EMA of the
  // controlling metric (max of decode and processing: they run in parallel);
  // slowMs_ a slower EMA of the same for the step-down decision, periodMs_ the
  // measured frame interval the budget is derived from.
  double ctrlMs_ = 0, slowMs_ = 0, periodMs_ = 0, overSince_ = -1, underSince_ = -1, lastFrameT_ = -1;
  int warmup_ = 0;           // frames still to skip after a (re)open before feeding the EMAs
  double holdSec_ = 10;      // headroom needed before stepping down (doubles after a failed probe)
  double probeSince_ = -1;   // >= 0 while a step-down is on probation
  bool probeRes_ = false;    // the current probe is the capture-size step (csLowRes_ off), not a tier step
  bool csLowRes_ = false;    // Center Stage capture dropped to the output size (tier 2) until a probe at tier 0 shows the full size fits
  bool lowCores_ = false;    // <= 2 hardware threads: never capture above the output size
  std::string error_, loopPath_, pwStatus_ = "off", gesture_, profileLine_;
  bool profile_ = false;       // `profile on`: per-stage timings and the framing debug object in the state
  EffectPipeline::FramingInfo framing_;   // last frame's framing (shown while profile_ is on)
  cv::Point2f panRange_;       // pan room of what is shown now (camera framing or placeholder), for the panel's drag
  // Settings changes are saved at most every 0.5 s (a pan drag or a zoom
  // wheel sends a burst of them): the first goes straight to disk, the rest
  // are flushed by the main loop (and on exit). Both under mu_.
  bool cfgDirty_ = false;
  double cfgSavedAt_ = 0;
  // Clients (by connection id) that asked for the preview: while any is
  // connected the pipeline runs even without consumers and previewPath_ is
  // kept fresh (previewOn_ mirrors !previewClients_.empty(), under mu_).
  std::set<int> previewClients_;
  bool previewOn_ = false;
  std::string previewPath_, previewTmp_;   // preview.jpg and the .tmp it is renamed from
  cv::Mat previewSmall_;          // downscaled frame, reused
  std::vector<uchar> previewJpg_; // encode buffer, reused
  clk::time_point previewDue_{};   // next frame at or after this is encoded
  bool previewWritten_ = false;   // a preview.jpg is on disk (to unlink when the preview stops)
  // Snapshot (`{"cmd":"snapshot"}`): the next output frame (or placeholder
  // frame) is saved as a PNG under snapDir_; while one is pending the pipeline
  // runs like for a preview client. Requesters still connected when it is
  // written get the reply; lastSnapshot_ ({path, time[, error]}) is shown in
  // the state. Pending state under mu_; the PNG encode + write run on
  // snapThread_ so the frame loop is not held up.
  bool snapPending_ = false;
  double snapSince_ = 0;          // when the pending request arrived (fails after kSnapTimeout)
  std::vector<int> snapClients_;
  std::string snapDir_;
  json lastSnapshot_;
  std::thread snapThread_;
  static constexpr double kSnapTimeout = 8;   // seconds: enough to open a slow camera
  void takeSnapshot(const cv::Mat& bgr);
  void failStaleSnapshot();
  void snapshotDone(const std::string& path, const std::string& error, const std::vector<int>& clients);
  static std::string saveSnapshotPng(const std::string& dir, const cv::Mat& bgr, std::string* err);
  bool hideRawActive_ = false;
  std::string runtimeDir_;
  int misses_ = 0;      // consecutive grab timeouts
  cv::Size capOpened_;  // size the current capture was asked for (reopen when the wanted size changes)
  cv::Size capActual_;  // what the device actually delivers (shown in the state)
  cv::Mat yuyv_;        // output frame converted once for both writers
  void writePreview(const cv::Mat& bgr, clk::time_point now);
  void removePreview();

  // Capture thread: grab + decode run off the processing thread into a
  // depth-1 "latest frame" slot (drop-oldest), so the decode overlaps with
  // the effects on any >=2-core machine. Latency is unchanged (no queue).
  std::thread capThread_;
  std::atomic<bool> capStop_{false};
  std::mutex slotMu_;
  std::condition_variable slotCv_;
  cv::Mat slotFrame_;
  double slotDecodeMs_ = 0;
  bool slotHas_ = false;
  int slotMisses_ = 0;
  void captureLoop();
  bool nextFrame(cv::Mat& f, double* decodeMs, int ms);
  cv::Size wantedCapture();
  void updateTier(double decodeMs, double loopMs, double now);
  void resetTier(bool keepTier);

  // Set a client-visible field (main thread) and mark the state dirty if it changed.
  template <class T> void setState(T& field, const T& value) {
    std::lock_guard<std::mutex> lk(mu_);
    if (field == value) return;
    field = value;
    stateDirty_ = true;
  }
  void setError(const std::string& e) { setState(error_, e); }
  void saveConfigSoon();   // caller holds mu_
  void flushConfig();      // main loop / exit: write a pending save once it is due
  json stateJson();
  void publishState();
  // Settings that apply to the current camera (global when sameForAll).
  Settings& effectiveSettings();
  std::string selectedBus();
  static bool cameraHidden(const CameraInfo& c) { return !c.key.empty() && fileExists("/etc/udev/rules.d/71-camera-effects-hide-" + c.key + ".rules"); }
  std::string handle(int client, const std::string& req);
  void clientClosed(int client);
  void rescanCameras();
  CameraInfo pickCamera();
  bool openCapture(std::string* err);
  bool captureOpen() const;
  void captureClose();
  bool captureGrab(cv::Mat& f, int ms);
};

// The camera the settings refer to: the one being captured, else the one that
// would be picked next (so the panel edits the right one while idle).
std::string Daemon::selectedBus() {
  if (!current_.bus.empty()) return current_.bus;
  for (auto& c : cameras_) if (!cfg_.preferredCamera.empty() && (c.bus == cfg_.preferredCamera || c.path == cfg_.preferredCamera)) return c.bus;
  return cameras_.empty() ? "" : cameras_[0].bus;
}

Settings& Daemon::effectiveSettings() {
  std::string bus = selectedBus();
  if (cfg_.sameForAll || bus.empty()) return cfg_.settings;
  auto it = cfg_.settingsByCamera.find(bus);
  if (it == cfg_.settingsByCamera.end()) it = cfg_.settingsByCamera.emplace(bus, cfg_.settings).first;
  return it->second;
}

json Daemon::stateJson() {
  json cams = json::array();
  for (auto& c : cameras_) cams.push_back({ { "path", c.path }, { "name", c.name }, { "bus", c.bus }, { "key", c.key }, { "hidden", cameraHidden(c) } });
  CameraInfo shown = current_;
  if (shown.bus.empty()) { std::string b = selectedBus(); for (auto& c : cameras_) if (c.bus == b) shown = c; }
  json j = json{ { "type", "state" },
               { "running", running_ },
               { "consumers", consumers_.load() },
               { "consumerApps", watcher_.apps() },
               { "previewOn", previewOn_ },
               { "camera", { { "path", shown.path }, { "name", shown.name }, { "bus", shown.bus }, { "key", shown.key } } },
               { "cameras", cams },
               { "loopback", loopPath_ },
               { "loopbackLabel", cfg_.label },
               { "pipewire", pwStatus_ },
               { "pipewireActive", pwActive_.load() },
               { "output", { { "width", cfg_.outW }, { "height", cfg_.outH }, { "fps", cfg_.fps } } },
               { "fps", (int)std::lround(fps_) },
               { "procMs", (int)std::lround(procMs_) },
               { "tier", tier_ },
               { "capture", { { "width", capActual_.width }, { "height", capActual_.height } } },
               { "settings", settingsToJson(effectiveSettings()) },
               { "sameForAll", cfg_.sameForAll },
               { "models", fx_.modelStatus() },
               { "gesture", gesture_ },
               { "profile", profileLine_ },
               { "reactions", Reactions::names() },
               { "reactionsActive", reactionsActive_ },
               { "hideRaw", hideRawActive_ },
               { "block", cfg_.block },
               { "previewMirror", cfg_.previewMirror },
               { "blockSource", cfg_.blockSource },
               { "blockFit", cfg_.blockFraming.fit }, { "blockZoom", cfg_.blockFraming.zoom }, { "blockPanX", cfg_.blockFraming.panX }, { "blockPanY", cfg_.blockFraming.panY },
               { "panRange", { panRange_.x, panRange_.y } },
               { "lastSnapshot", lastSnapshot_ },
               { "error", error_ } };
  if (profile_) {
    const auto& f = framing_;
    j["framing"] = { { "faces", f.faces }, { "face", { f.face.x, f.face.y, f.face.width, f.face.height } },
                     { "crop", { f.geom.crop.x, f.geom.crop.y, f.geom.crop.width, f.geom.crop.height } },
                     { "dst", { f.geom.dst.x, f.geom.dst.y, f.geom.dst.width, f.geom.dst.height } }, { "src", { f.src.width, f.src.height } } };
  }
  return j;
}

void Daemon::saveConfigSoon() {
  double now = nowSec();
  if (now - cfgSavedAt_ >= 0.5) { saveConfig(cfg_); cfgSavedAt_ = now; cfgDirty_ = false; }
  else cfgDirty_ = true;
}

void Daemon::flushConfig() {
  std::lock_guard<std::mutex> lk(mu_);
  if (!cfgDirty_) return;
  double now = nowSec();
  if (now - cfgSavedAt_ < 0.5) return;
  saveConfig(cfg_); cfgSavedAt_ = now; cfgDirty_ = false;
}

void Daemon::publishState() {
  json j;
  { std::lock_guard<std::mutex> lk(mu_); j = stateJson(); }
  server_.broadcast(j.dump());
}

void Daemon::rescanCameras() {
  std::vector<CameraInfo> cams = enumerator_.scan();
  std::lock_guard<std::mutex> lk(mu_);
  if (cams.size() != cameras_.size()) stateDirty_ = true;
  else for (size_t i = 0; i < cams.size(); i++) if (cams[i].path != cameras_[i].path) { stateDirty_ = true; break; }
  cameras_ = cams;
}

CameraInfo Daemon::pickCamera() {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& c : cameras_) if (!cfg_.preferredCamera.empty() && (c.bus == cfg_.preferredCamera || c.path == cfg_.preferredCamera)) return c;
  return cameras_.empty() ? CameraInfo{} : cameras_[0];
}

// Center Stage captures at the (larger) capture size so it has room to zoom;
// everything else at the output size. On a machine that cannot keep up (tier
// 2, or two hardware threads) Center Stage works from the output size too:
// the face detector only needs a 480x270 level anyway, only the maximum zoom
// loses sharpness, and a 1080p MJPEG decode is what such a machine cannot afford.
cv::Size Daemon::wantedCapture() {
  std::lock_guard<std::mutex> lk(mu_);
  cv::Size outSz(std::min(cfg_.capW, cfg_.outW), std::min(cfg_.capH, cfg_.outH));
  if (effectiveSettings().centerStage && !lowCores_ && !csLowRes_) return cv::Size(cfg_.capW, cfg_.capH);
  return outSz;
}

bool Daemon::openCapture(std::string* err) {
  std::string pref;
  { std::lock_guard<std::mutex> lk(mu_); pref = cfg_.preferredCamera; }
  cv::Size want = wantedCapture();
  // A file source (dev/testing) must be a readable regular file: a FIFO or a
  // directory would wedge or spin the OpenCV/FFmpeg opener.
  if (!pref.empty() && pref[0] == '/' && pref.rfind("/dev/", 0) != 0 && blockSourceValid(pref)) {
    if (!file_.open(pref, cfg_.fps, err)) return false;
    useFile_ = true;
    std::lock_guard<std::mutex> lk(mu_);
    current_ = CameraInfo{ pref, "File: " + pref.substr(pref.rfind('/') + 1), pref, "" };
    capOpened_ = want; capActual_ = cv::Size(file_.width(), file_.height());
  } else {
    useFile_ = false;
    CameraInfo c = pickCamera();
    if (c.path.empty()) { *err = "no camera found"; return false; }
    if (!cap_.open(c.path, want.width, want.height, cfg_.fps, err)) return false;
    std::lock_guard<std::mutex> lk(mu_);
    current_ = c; capOpened_ = want; capActual_ = cv::Size(cap_.width(), cap_.height());
  }
  { std::lock_guard<std::mutex> lk(slotMu_); slotHas_ = false; slotMisses_ = 0; }
  resetTier(true);  // new source, new timings: restart the controller (tier itself is kept across a reopen)
  capStop_ = false;
  capThread_ = std::thread([this] { captureLoop(); });
  return true;
}
bool Daemon::captureOpen() const { return useFile_ ? file_.isOpen() : cap_.isOpen(); }
void Daemon::captureClose() {
  capStop_ = true;
  if (capThread_.joinable()) capThread_.join();
  cap_.close(); file_.close();
  std::lock_guard<std::mutex> lk(mu_);
  current_ = CameraInfo{};
  capOpened_ = capActual_ = cv::Size();
}
bool Daemon::captureGrab(cv::Mat& f, int ms) { return useFile_ ? file_.grab(f, ms) : cap_.grab(f, ms); }

// Capture thread body. `f` is reused as the decode target; swapping it with
// the slot means the three buffers (capture, slot, processing) rotate without
// per-frame allocation, and an unconsumed frame is simply overwritten.
// A corrupt frame must not take the daemon down: OpenCV throws on e.g. an MJPEG
// header claiming an absurd size or odd YUV dimensions, and a decode that
// "succeeds" at a size far from the negotiated one is treated as a miss too
// (it would only feed garbage sizes downstream).
void Daemon::captureLoop() {
  cv::Mat f;
  const int maxW = 2 * (useFile_ ? file_.width() : cap_.width()), maxH = 2 * (useFile_ ? file_.height() : cap_.height());
  int badLogged = 0;
  while (!capStop_) {
    bool ok = false;
    try {
      ok = captureGrab(f, 200);
      if (ok && (f.empty() || f.cols > maxW || f.rows > maxH)) {
        if (badLogged++ < 3) fprintf(stderr, "camera-effects-server: dropping %dx%d frame (negotiated up to %dx%d)\n", f.cols, f.rows, maxW, maxH);
        ok = false;
      }
    } catch (const std::exception& e) {
      if (badLogged++ < 3) fprintf(stderr, "camera-effects-server: decode failed: %s\n", e.what());
      ok = false;
    }
    std::lock_guard<std::mutex> lk(slotMu_);
    if (ok) {
      std::swap(f, slotFrame_);
      slotDecodeMs_ = useFile_ ? file_.decodeMs() : cap_.decodeMs();
      slotHas_ = true;
      slotMisses_ = 0;
    } else {
      slotMisses_++;
    }
    slotCv_.notify_one();
  }
}

// Processing side: waits up to `ms` for a fresh frame (false on timeout or a
// grab error, which the caller counts as a miss).
bool Daemon::nextFrame(cv::Mat& f, double* decodeMs, int ms) {
  std::unique_lock<std::mutex> lk(slotMu_);
  slotCv_.wait_for(lk, std::chrono::milliseconds(ms), [&] { return slotHas_ || slotMisses_ > 0; });
  if (slotHas_) {
    std::swap(f, slotFrame_);
    slotHas_ = false;
    *decodeMs = slotDecodeMs_;
    return true;
  }
  if (slotMisses_ > 0) slotMisses_--;
  return false;
}

// Adaptive quality. The controlling metric is the larger of decode time and
// processing time (they run on different threads), tracked as an EMA against a
// budget of 80% of the frame period. The period is the measured frame interval
// (a camera delivering 17 fps leaves 59 ms per frame), never shorter than the
// configured one. Sustained overrun (~2 s) raises the tier. Stepping down is a
// probe: after holdSec_ (10 s; twice that for the capture-size step, the only
// one that reopens the camera) with a slow EMA below 70% of the budget the tier
// drops; if the fast EMA then overruns the budget for 0.7 s within the 5 s
// probation the step is reverted and the hold doubles (thrash guard, reset by
// the next successful probe). The under-window only restarts once the slow EMA
// climbs above 85% of the budget, so periodic heavy frames (palm detection with
// a hand in view) do not keep a tier that would fit from ever being probed. The
// first frames after a (re)open are warm-up outliers and are not fed in. Tiers
// only cheapen the effects (see effects.cpp: segmenter every 2nd/3rd frame,
// slower gesture/face cadence, single blur pass, portrait at 1/8); passthrough
// is unaffected. Tier 2 additionally drops the Center Stage capture to the
// output size (csLowRes_, see wantedCapture); going back up to the full capture
// size is the last probe step after tier 0 fits.
void Daemon::updateTier(double decodeMs, double loopMs, double now) {
  double sum = decodeMs + loopMs;
  { std::lock_guard<std::mutex> lk(mu_); procMs_ = procMs_ == 0 ? sum : procMs_ * 0.9 + sum * 0.1; }
  double cfgPeriod = 1000.0 / std::max(1, cfg_.fps);
  double gap = lastFrameT_ >= 0 ? (now - lastFrameT_) * 1000.0 : -1;
  lastFrameT_ = now;
  if (warmup_ > 0) { warmup_--; return; }
  if (gap > 0 && gap < 4 * cfgPeriod) periodMs_ = periodMs_ == 0 ? gap : periodMs_ * 0.9 + gap * 0.1;  // stalls/misses are not the cadence
  double budget = 0.8 * std::max(cfgPeriod, periodMs_);
  double ctrl = std::max(decodeMs, loopMs);
  ctrlMs_ = ctrlMs_ == 0 ? ctrl : ctrlMs_ * 0.9 + ctrl * 0.1;
  slowMs_ = slowMs_ == 0 ? ctrl : slowMs_ * 0.97 + ctrl * 0.03;
  double ema = ctrlMs_;
  int t = tier_;
  bool lowRes = csLowRes_;
  bool probing = probeSince_ >= 0;
  if (probing && now - probeSince_ > 5.0) { probeSince_ = -1; probing = false; holdSec_ = 10; }  // probe passed
  if (ema > budget) {
    if (overSince_ < 0) overSince_ = now;
    else if (probing && now - overSince_ > 0.7) {  // the step-down did not fit: back up, wait longer next time
      if (probeRes_) lowRes = true; else t = std::min(t + 1, 2);
      probeSince_ = -1; overSince_ = -1; holdSec_ = std::min(holdSec_ * 2, 160.0);
    } else if (!probing && now - overSince_ > 2.0 && t < 2) { t++; overSince_ = -1; }
  } else overSince_ = -1;
  if (slowMs_ < 0.7 * budget && underSince_ < 0) underSince_ = now;
  else if (slowMs_ > 0.85 * budget) underSince_ = -1;
  if (underSince_ >= 0 && !probing && (t > 0 || lowRes)) {
    bool resStep = t == 0;  // tier 0 already: the remaining step is the capture size (a reopen: ask for twice the headroom)
    if (now - underSince_ > (resStep ? 2 * holdSec_ : holdSec_)) {
      probeRes_ = resStep;
      if (probeRes_) lowRes = false; else t--;
      underSince_ = -1; probeSince_ = now;
    }
  }
  if (t >= 2) lowRes = true;
  if (t != tier_ || lowRes != csLowRes_) {
    fx_.setTier(t);
    setState(tier_, t);
    setState(csLowRes_, lowRes);
    fprintf(stderr, "camera-effects-server: quality tier %d%s (%.1f ms/frame, budget %.1f, hold %.0f s)\n", t, lowRes ? " (capture at output size)" : "", ema, budget, holdSec_);
  }
}

// Restart the controller's timing after a (re)open, or forget everything when
// the camera goes idle. The tier survives a reopen (a Center Stage or camera
// switch does not change the machine); a probe in flight gets a fresh window.
void Daemon::resetTier(bool keepTier) {
  ctrlMs_ = slowMs_ = periodMs_ = 0; overSince_ = underSince_ = lastFrameT_ = -1; warmup_ = 5;
  { std::lock_guard<std::mutex> lk(mu_); procMs_ = 0; }
  if (keepTier) { if (probeSince_ >= 0) probeSince_ = nowSec(); return; }
  probeSince_ = -1; probeRes_ = false; holdSec_ = 10;
  if (tier_ != 0 || csLowRes_) { fx_.setTier(0); setState(tier_, 0); setState(csLowRes_, false); }
}

void Daemon::clientClosed(int client) {
  std::lock_guard<std::mutex> lk(mu_);
  if (previewClients_.erase(client)) { previewOn_ = !previewClients_.empty(); stateDirty_ = true; }
  snapClients_.erase(std::remove(snapClients_.begin(), snapClients_.end(), client), snapClients_.end());  // the snapshot is still taken
}

// Runs on the server thread. Requests come from same-uid clients but may be
// malformed: every field is type-checked and nothing here may throw.
std::string Daemon::handle(int client, const std::string& req) {
  static const std::string kOk = R"({"type":"ok"})";
  auto error = [](const char* what) { return json{ { "type", "error" }, { "error", what } }.dump(); };
  try {
    json j = json::parse(req, nullptr, false);
    if (j.is_discarded()) return error("bad json");
    if (!j.is_object() || !j.contains("cmd") || !j["cmd"].is_string()) return error("missing cmd");
    std::string cmd = j["cmd"];
    auto boolField = [&](const char* k) { return j.contains(k) && j[k].is_boolean() && j[k].get<bool>(); };
    if (cmd == "get") { std::lock_guard<std::mutex> lk(mu_); return stateJson().dump(); }
    if (cmd == "set") {
      {
        std::lock_guard<std::mutex> lk(mu_);
        if (j.contains("sameForAll") && j["sameForAll"].is_boolean()) {
          bool v = j["sameForAll"];
          // Switching to per-camera: seed the current camera from what is shown now.
          if (!v && cfg_.sameForAll && !selectedBus().empty()) cfg_.settingsByCamera[selectedBus()] = cfg_.settings;
          // Switching to shared: what is shown now becomes the shared set.
          if (v && !cfg_.sameForAll) cfg_.settings = effectiveSettings();
          cfg_.sameForAll = v;
        }
        if (j.contains("camera") && j["camera"].is_string()) cfg_.preferredCamera = j["camera"];
        if (j.contains("settings") && j["settings"].is_object()) {
          const json& sj = j["settings"];
          settingsFromJson(sj, effectiveSettings());
          // Global (not per camera) keys travel in the same object: `camera-effects-server set block=true blockSource=/path`.
          if (sj.contains("block") && sj["block"].is_boolean()) cfg_.block = sj["block"];
          if (sj.contains("previewMirror") && sj["previewMirror"].is_boolean()) cfg_.previewMirror = sj["previewMirror"];
          framingFromJson(sj, "block", cfg_.blockFraming.fit, cfg_.blockFraming.zoom, cfg_.blockFraming.panX, cfg_.blockFraming.panY);
          if (sj.contains("blockSource") && sj["blockSource"].is_string()) {
            std::string p = sj["blockSource"], why;
            if (blockSourceValid(p, &why)) cfg_.blockSource = p;
            else { saveConfigSoon(); stateDirty_ = true; return json{ { "type", "error" }, { "error", "blockSource: " + why } }.dump(); }
          }
        }
        saveConfigSoon();
      }
      stateDirty_ = true;
      return kOk;
    }
    if (cmd == "react") {
      if (!j.contains("name") || !j["name"].is_string()) return error("missing name");
      fx_.triggerReaction(j["name"]);
      return kOk;
    }
    if (cmd == "rescan") { rescanCameras(); stateDirty_ = true; return kOk; }
    if (cmd == "preview") {  // per client: on until it says off or disconnects
      std::lock_guard<std::mutex> lk(mu_);
      if (boolField("on")) previewClients_.insert(client); else previewClients_.erase(client);
      previewOn_ = !previewClients_.empty();
      stateDirty_ = true;
      return kOk;
    }
    if (cmd == "snapshot") {  // replied to when the frame is written (see takeSnapshot)
      std::lock_guard<std::mutex> lk(mu_);
      if (!snapPending_) { snapPending_ = true; snapSince_ = nowSec(); }
      snapClients_.push_back(client);
      return "";
    }
    if (cmd == "profile") {
      bool on = boolField("on");
      fx_.setProfile(on);
      { std::lock_guard<std::mutex> lk(mu_); profile_ = on; if (!on) profileLine_.clear(); stateDirty_ = true; }  // clear at once, also while idle
      return kOk;
    }
    if (cmd == "quit") { g_quit = true; return kOk; }
    return error("unknown cmd");
  } catch (const std::exception& e) {
    fprintf(stderr, "camera-effects-server: request failed: %s\n", e.what());
    return error("request failed");
  }
}

// The runtime dir holds the control socket and the instance lock. Refuse
// anything that is not a private directory of ours (a pre-created dir in a
// shared /tmp could redirect the socket or the lock).
bool ensureRuntimeDir(const std::string& dir, std::string* err) {
  if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) { *err = std::string("mkdir ") + dir + ": " + strerror(errno); return false; }
  int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) { *err = std::string("open ") + dir + ": " + strerror(errno); return false; }
  struct stat st{};
  bool ok = fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid() && (st.st_mode & 0777) == 0700;
  // The setgid (hide-raw) build creates files with egid camerad; keep our own
  // dirs/files in the user's group so nothing looks odd or depends on camerad.
  if (ok && st.st_gid != getgid()) (void)!fchown(fd, (uid_t)-1, getgid());
  close(fd);
  if (!ok) { *err = dir + " must be a directory owned by us with mode 0700"; return false; }
  return true;
}

// Preview for the panel: the output frame downscaled to fit 640x360, JPEG
// (quality 70, ~1 ms), ~15 Hz (a due time advanced by 66 ms per write, so a
// source with jittery frame times still averages that rate instead of losing
// every borderline frame), written to preview.jpg in the runtime dir (a
// private 0700 dir of ours, see ensureRuntimeDir) via a temp file + rename so
// a reader always gets a whole image. Buffers are reused.
void Daemon::writePreview(const cv::Mat& bgr, clk::time_point now) {
  if (bgr.empty() || now < previewDue_) return;
  previewDue_ = std::max(previewDue_ + std::chrono::milliseconds(66), now - std::chrono::milliseconds(33));  // no backlog after a stall
  double sc = std::min(1.0, std::min(640.0 / bgr.cols, 360.0 / bgr.rows));
  cv::Size sz(std::max(2, (int)std::lround(bgr.cols * sc)), std::max(2, (int)std::lround(bgr.rows * sc)));
  const cv::Mat* src = &bgr;
  if (sz != bgr.size()) { cv::resize(bgr, previewSmall_, sz, 0, 0, cv::INTER_AREA); src = &previewSmall_; }
  try {
    if (!cv::imencode(".jpg", *src, previewJpg_, { cv::IMWRITE_JPEG_QUALITY, 70 })) return;
  } catch (const std::exception& e) {
    fprintf(stderr, "camera-effects-server: preview encode failed: %s\n", e.what());
    return;
  }
  int fd = open(previewTmp_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) return;
  (void)!fchown(fd, (uid_t)-1, getgid());  // the setgid (hide-raw) build would leave it group camerad
  size_t off = 0;
  while (off < previewJpg_.size()) {
    ssize_t n = write(fd, previewJpg_.data() + off, previewJpg_.size() - off);
    if (n <= 0) { close(fd); unlink(previewTmp_.c_str()); return; }
    off += (size_t)n;
  }
  close(fd);
  if (rename(previewTmp_.c_str(), previewPath_.c_str()) == 0) previewWritten_ = true;
  else unlink(previewTmp_.c_str());
}

void Daemon::removePreview() {
  if (!previewWritten_) return;
  previewWritten_ = false;
  unlink(previewPath_.c_str());
  unlink(previewTmp_.c_str());
}

// Snapshot: <dir>/YYYY-MM-DD_HH-MM-SS.png (a -2, -3 suffix when that name is
// taken), created exclusively with O_NOFOLLOW so nothing outside the directory
// can be written whatever is placed at the path. New dirs/files get the user's
// gid (the setgid hide-raw build would leave them group camerad). Returns the
// path, or "" with *err set. Runs on snapThread_.
std::string Daemon::saveSnapshotPng(const std::string& dir, const cv::Mat& bgr, std::string* err) {
  std::vector<uchar> png;
  if (bgr.empty() || !cv::imencode(".png", bgr, png, { cv::IMWRITE_PNG_COMPRESSION, 3 })) { *err = "PNG encode failed"; return ""; }
  std::string parent = dir.substr(0, dir.rfind('/'));   // ~/Pictures may not exist yet either
  for (const std::string& d : { parent, dir }) {
    if (mkdir(d.c_str(), 0755) == 0) (void)!chown(d.c_str(), (uid_t)-1, getgid());
    else if (errno != EEXIST) { *err = "mkdir " + d + ": " + strerror(errno); return ""; }
  }
  time_t t = time(nullptr);
  struct tm tm{};
  localtime_r(&t, &tm);
  char stamp[32];
  strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", &tm);
  int fd = -1;
  std::string path;
  for (int i = 1; i <= 100 && fd < 0; i++) {
    path = dir + "/" + stamp + (i == 1 ? std::string() : "-" + std::to_string(i)) + ".png";
    fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0 && errno != EEXIST) { *err = "open " + path + ": " + strerror(errno); return ""; }
  }
  if (fd < 0) { *err = "too many snapshots this second"; return ""; }
  (void)!fchown(fd, (uid_t)-1, getgid());
  size_t off = 0;
  while (off < png.size()) {
    ssize_t n = write(fd, png.data() + off, png.size() - off);
    if (n <= 0) { *err = "write " + path + ": " + strerror(errno); close(fd); unlink(path.c_str()); return ""; }
    off += (size_t)n;
  }
  close(fd);
  return path;
}

// Main thread, with the frame the outputs just got: hand a copy to the worker.
void Daemon::takeSnapshot(const cv::Mat& bgr) {
  std::vector<int> clients;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!snapPending_) return;
    snapPending_ = false;
    clients.swap(snapClients_);
  }
  if (snapThread_.joinable()) snapThread_.join();  // the previous one (~50 ms of PNG work) is normally long done
  cv::Mat copy = bgr.clone();
  snapThread_ = std::thread([this, copy, clients] {
    std::string err, path;
    try { path = saveSnapshotPng(snapDir_, copy, &err); } catch (const std::exception& e) { err = e.what(); }
    snapshotDone(path, err, clients);
  });
}

// No frame came within kSnapTimeout (no camera, capture failing): fail the request.
void Daemon::failStaleSnapshot() {
  std::vector<int> clients;
  std::string why;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!snapPending_ || nowSec() - snapSince_ < kSnapTimeout) return;
    snapPending_ = false;
    clients.swap(snapClients_);
    why = error_.empty() ? "no frame" : error_;
  }
  snapshotDone("", why, clients);
}

// Any thread: reply to the requesters and show the result in the state.
void Daemon::snapshotDone(const std::string& path, const std::string& error, const std::vector<int>& clients) {
  json r = { { "type", "snapshot" }, { "path", path } };
  if (!error.empty()) r["error"] = error;
  std::string line = r.dump();
  for (int c : clients) server_.sendTo(c, line);
  {
    std::lock_guard<std::mutex> lk(mu_);
    lastSnapshot_ = json{ { "path", path }, { "time", std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count() } };
    if (!error.empty()) lastSnapshot_["error"] = error;
    stateDirty_ = true;
  }
  if (error.empty()) fprintf(stderr, "camera-effects-server: snapshot saved to %s\n", path.c_str());
  else fprintf(stderr, "camera-effects-server: snapshot failed: %s\n", error.c_str());
}

int Daemon::run() {
  // The shell owns us: exit with it rather than linger as an orphan the next
  // shell cannot manage (it also asks a stray instance to quit, belt and braces).
  // The kernel clears this flag on every credential change, and the setgid
  // (hide-raw) build switches gid for each consumer scan: ConsumerWatcher
  // re-arms it after every restore (loopback.cpp).
  prctl(PR_SET_PDEATHSIG, SIGTERM);
  // OpenCV's worker threads spin after every parallel region: with one thread
  // per core the passthrough loop burns 3-4x the CPU time for the same wall
  // time. Two threads (one on a dual-core) keeps the CPU for the video apps.
  unsigned hw = std::thread::hardware_concurrency();
  cv::setNumThreads(hw <= 2 ? 1 : 2);
  lowCores_ = hw > 0 && hw <= 2;
  runtimeDir_ = xdgRuntime() + "/camera-effects";
  std::string err;
  if (!ensureRuntimeDir(runtimeDir_, &err)) { fprintf(stderr, "camera-effects-server: %s\n", err.c_str()); return 1; }
  std::string sockPath = runtimeDir_ + "/ctl.sock";
  previewPath_ = runtimeDir_ + "/preview.jpg";
  previewTmp_ = previewPath_ + ".tmp";
  snapDir_ = homeDir() + "/Pictures/Camera Effects";
  // Single instance per session (the shell restarts us; a stray manual copy
  // must not fight over the loopback writer).
  int lockFd = open((runtimeDir_ + "/lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (lockFd < 0 || flock(lockFd, LOCK_EX | LOCK_NB) < 0) { fprintf(stderr, "camera-effects-server: another instance is running\n"); return 3; }
  previewWritten_ = true; removePreview();  // a stale one from a crashed instance

  if (!fx_.init(cfg_.modelsDir, cfg_.assetsDir, &err)) fprintf(stderr, "models: %s\n", err.c_str());
  else if (!err.empty()) fprintf(stderr, "models (degraded): %s\n", err.c_str());
  fx_.setSettings(cfg_.settings);

  if (!server_.start(sockPath, [this](int c, const std::string& r) { return handle(c, r); }, [this](int c) { clientClosed(c); }, &err)) { fprintf(stderr, "control socket: %s\n", err.c_str()); return 1; }

  // Native PipeWire camera node for portal-based apps (best effort).
  if (pwOut_.start(cfg_.outW, cfg_.outH, cfg_.fps, cfg_.label, [this](bool on) { pwActive_ = on; stateDirty_ = true; }, &err)) setState(pwStatus_, std::string("ok"));
  else { setState(pwStatus_, err); fprintf(stderr, "camera-effects-server: pipewire output unavailable: %s\n", err.c_str()); }

  // The loopback device is created by the camera-effects-device systemd unit
  // (v4l2loopback-ctl, installed by `camera-effects-setup install`). Wait for
  // it rather than fail: the plugin's setup step may still be running.
  auto lastLoopCheck = clk::now() - std::chrono::seconds(10);
  auto lastScan = clk::now() - std::chrono::seconds(10);
  auto lastPublish = clk::now();
  auto lastPwCheck = clk::now();
  bool wasRunning = false;
  int frames = 0;
  auto fpsT0 = clk::now();
  cv::Mat frame;  // processing-side frame buffer (rotates with the capture slot)
  fprintf(stderr, "camera-effects-server: ready (socket %s)\n", sockPath.c_str());

  while (!g_quit) {
    auto now = clk::now();
    if (!loop_.isOpen() && now - lastLoopCheck > std::chrono::seconds(2)) {
      lastLoopCheck = now;
      std::string p = findLoopbackByLabel(cfg_.label);
      if (p.empty()) setError("virtual camera device missing");
      else if (loop_.open(p, cfg_.outW, cfg_.outH, &err)) {
        setState(loopPath_, p);
        setError("");
        fprintf(stderr, "camera-effects-server: virtual camera %s (%dx%d)\n", p.c_str(), cfg_.outW, cfg_.outH);
        watcher_.start(p, [this](int n) { consumers_ = n; stateDirty_ = true; });
      } else setError(err);
    }
    if (now - lastScan > std::chrono::seconds(running_ ? 10 : 4)) { lastScan = now; rescanCameras(); }
    // PipeWire restarted / stream error: reconnect with backoff, show it meanwhile.
    if (now - lastPwCheck > std::chrono::seconds(1)) { lastPwCheck = now; setState(pwStatus_, pwOut_.maintain(nowSec())); }
    setState(hideRawActive_, fileExists("/etc/udev/rules.d/71-camera-effects-hide-raw.rules"));

    flushConfig();
    failStaleSnapshot();
    bool preview, snap, block;
    std::string blockSrc;
    Framing blockFraming;
    { std::lock_guard<std::mutex> lk(mu_); preview = previewOn_; snap = snapPending_; block = cfg_.block; blockSrc = cfg_.blockSource; blockFraming = cfg_.blockFraming; }
    bool want = (loop_.isOpen() && consumers_ > 0) || pwActive_ || preview || snap;  // a pending snapshot runs the pipeline like a preview client
    if (!preview) removePreview();
    // Blocked: the physical camera stays closed (light off) whatever the
    // consumers do; they get the placeholder from block_ instead, on the same
    // on-demand terms.
    bool stopped = false;
    if (block && captureOpen()) { captureClose(); resetTier(false); fprintf(stderr, "camera-effects-server: blocked: camera released\n"); }
    if (block_.active() && (!block || !want)) { block_.close(); setError(""); stopped = !want; }
    if (want && !block && !captureOpen()) {
      if (openCapture(&err)) {
        setError("");
        misses_ = 0;
        fprintf(stderr, "camera-effects-server: capturing %s %dx%d %s\n", current_.path.c_str(), useFile_ ? file_.width() : cap_.width(), useFile_ ? file_.height() : cap_.height(), useFile_ ? "file" : cap_.format().c_str());
      } else {
        if (error_ != err) fprintf(stderr, "camera-effects-server: capture: %s\n", err.c_str());
        setError(err);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }
    }
    if (want && block) {
      bool was = block_.active();
      block_.open(blockSrc, cv::Size(cfg_.outW, cfg_.outH), cfg_.fps, blockFraming);
      if (!was) fprintf(stderr, "camera-effects-server: blocked: showing %s\n", blockSrc.empty() ? "the built-in card" : blockSrc.c_str());
      setError(block_.error());
      setState(panRange_, block_.panRange());
    }
    if (!want && captureOpen()) { captureClose(); resetTier(false); stopped = true; }
    if (stopped) {
      // Leave black behind so the next opener doesn't see a stale frame.
      cv::Mat black(cfg_.outH, cfg_.outW, CV_8UC3, cv::Scalar(0, 0, 0));
      if (loop_.isOpen()) loop_.write(black);
      pwOut_.clear();
      fprintf(stderr, "camera-effects-server: idle\n");
    }
    setState(running_, captureOpen() || block_.active());
    if (running_ != wasRunning) { wasRunning = running_; frames = 0; fpsT0 = now; setState(fps_, 0.0); }
    auto countFrame = [&]() {
      frames++;
      auto dt = std::chrono::duration<double>(now - fpsT0).count();
      if (dt >= 1.0) { setState(fps_, frames / dt); frames = 0; fpsT0 = now; }
    };

    if (block_.active()) {
      // Placeholder: no effects, no tier control; block_.next() paces the loop.
      try {
        const cv::Mat& card = block_.next();
        cv::cvtColor(card, yuyv_, cv::COLOR_BGR2YUV_YUYV);
        if (loop_.isOpen()) loop_.writeYuyv(yuyv_);
        pwOut_.pushYuyv(yuyv_);
        if (preview) writePreview(card, clk::now());
        if (snap) takeSnapshot(card);
        countFrame();
      } catch (const std::exception& e) {
        fprintf(stderr, "camera-effects-server: placeholder frame dropped: %s\n", e.what());
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
      }
      setState(reactionsActive_, fx_.reactionsActive());
    } else if (running_) {
      cv::Mat out;
      double decodeMs = 0;
      if (nextFrame(frame, &decodeMs, 200)) {
        misses_ = 0;
        Settings s;
        std::string preferred;
        { std::lock_guard<std::mutex> lk(mu_); s = effectiveSettings(); preferred = cfg_.preferredCamera; }
        if (!(fx_.settings() == s)) fx_.setSettings(s);
        // Camera switch requested?
        if (!preferred.empty() && preferred != current_.bus && preferred != current_.path) {
          CameraInfo c = pickCamera();
          if (useFile_ || fileExists(preferred) || (!c.path.empty() && c.path != current_.path)) { captureClose(); continue; }
        }
        // Center Stage toggled: the camera is reopened at the size it needs.
        if (!useFile_ && wantedCapture() != capOpened_) { captureClose(); continue; }
        auto p0 = clk::now();
        // A bad frame or setting must not take the daemon down mid-call: log and skip the frame.
        try {
          fx_.process(frame, out, cv::Size(cfg_.outW, cfg_.outH), nowSec());
          // One BGR->YUYV conversion feeds both outputs.
          cv::cvtColor(out, yuyv_, cv::COLOR_BGR2YUV_YUYV);
          if (loop_.isOpen()) loop_.writeYuyv(yuyv_);
          pwOut_.pushYuyv(yuyv_);
          if (preview) writePreview(out, clk::now());  // after the outputs: never delays them
          if (snap) takeSnapshot(out);
        } catch (const std::exception& e) {
          fprintf(stderr, "camera-effects-server: frame dropped: %s\n", e.what());
          continue;
        }
        double loopMs = std::chrono::duration<double, std::milli>(clk::now() - p0).count();
        updateTier(decodeMs, loopMs, nowSec());  // also updates procMs_ (shown with the next publish)
        setState(gesture_, fx_.lastGesture());
        setState(reactionsActive_, fx_.reactionsActive());
        setState(profileLine_, fx_.profileLine());
        {
          EffectPipeline::FramingInfo fi = fx_.framing();
          std::lock_guard<std::mutex> lk(mu_);
          if (panRange_ != fi.geom.range) { panRange_ = fi.geom.range; stateDirty_ = true; }
          if (profile_ && framing_ != fi) { framing_ = fi; stateDirty_ = true; }
        }
        countFrame();
      } else {
        // Camera unplugged or stalled: reopen.
        if (++misses_ > 15) { misses_ = 0; captureClose(); rescanCameras(); }
      }
    } else {
      setState(reactionsActive_, fx_.reactionsActive());  // queued while idle: shown as pending
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }

    if (stateDirty_ && now - lastPublish > std::chrono::milliseconds(150)) {
      stateDirty_ = false;
      lastPublish = now;
      publishState();
    }
  }
  captureClose();
  block_.close();
  pwOut_.stop();
  watcher_.stop();
  if (snapThread_.joinable()) snapThread_.join();
  server_.stop();
  removePreview();
  { std::lock_guard<std::mutex> lk(mu_); if (cfgDirty_) saveConfig(cfg_); }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string cmd = argc > 1 ? argv[1] : "run";
  // Only the daemon needs the setgid (camerad) copy's privileges: every other
  // mode drops to the caller's own gid before doing anything else.
  if (cmd != "run" && getegid() != getgid()) {
    if (setresgid(getgid(), getgid(), getgid()) != 0) { perror("setresgid"); return 1; }
  }
  if (cmd == "hands" && argc > 2) {
    // Dev tool: run hand detection on an image, print per-hand debug, write annotated copy.
    std::string exe(4096, '\0');
    ssize_t n = readlink("/proc/self/exe", exe.data(), exe.size() - 1);
    exe.resize(n > 0 ? n : 0);
    std::string dir = exe.substr(0, exe.rfind('/'));
    std::string models = fileExists(dir + "/../../models/pphumanseg.onnx") ? dir + "/../../models" : xdgData() + "/camera-effects/models";
    GestureDetector g;
    std::string err;
    if (!g.load(models, &err)) { fprintf(stderr, "load: %s\n", err.c_str()); return 1; }
    cv::Mat img = cv::imread(argv[2], cv::IMREAD_COLOR);
    if (img.empty()) { fprintf(stderr, "cannot read image\n"); return 1; }
    if (img.cols > 1280) { double s = 1280.0 / img.cols; cv::resize(img, img, cv::Size(), s, s, cv::INTER_AREA); }
    auto hands = g.detect(img);
    printf("hands: %zu\n", hands.size());
    for (auto& h : hands) {
      std::string dbg;
      std::string gs = GestureDetector::singleHandGesture(h, &dbg);
      printf("  score=%.2f box=%.0f,%.0f %.0fx%.0f gesture='%s' %s\n", h.score, h.box.x, h.box.y, h.box.width, h.box.height, gs.c_str(), dbg.c_str());
      for (int i = 0; i < 21; i++) cv::circle(img, h.lm[i], 4, i == 4 ? cv::Scalar(0, 0, 255) : (i == 8 ? cv::Scalar(0, 255, 0) : cv::Scalar(255, 200, 0)), -1);
      cv::rectangle(img, h.box, cv::Scalar(0, 255, 255), 2);
    }
    printf("combined: '%s'\n", g.classify(hands, {}).c_str());
    cv::imwrite(argc > 3 ? argv[3] : "/tmp/hands.jpg", img);
    return 0;
  }

  Config cfg;
  cfg.configPath = xdgConfig() + "/camera-effects/config.json";
  std::string exe(4096, '\0');
  ssize_t n = readlink("/proc/self/exe", exe.data(), exe.size() - 1);
  exe.resize(n > 0 ? n : 0);
  std::string exeDir = exe.substr(0, exe.rfind('/'));
  // Data dirs: installed layout first, then the source tree (daemon/build/../..).
  std::string dataDir = xdgData() + "/camera-effects";
  if (fileExists(exeDir + "/../models/pphumanseg.onnx")) dataDir = exeDir + "/..";
  else if (fileExists(exeDir + "/../../models/pphumanseg.onnx")) dataDir = exeDir + "/../..";
  cfg.modelsDir = dataDir + "/models";
  cfg.assetsDir = dataDir + "/assets";
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--config" && i + 1 < argc) cfg.configPath = argv[++i];
    else if (a == "--data" && i + 1 < argc) { cfg.modelsDir = std::string(argv[i + 1]) + "/models"; cfg.assetsDir = std::string(argv[i + 1]) + "/assets"; i++; }
  }
  loadConfig(cfg);

  std::string sock = xdgRuntime() + "/camera-effects/ctl.sock";
  if (cmd == "status") return cmdClient(sock, R"({"cmd":"get"})", false);
  if (cmd == "react" && argc > 2) return cmdClient(sock, json{ { "cmd", "react" }, { "name", argv[2] } }.dump(), true);
  if (cmd == "set") {
    json s = json::object();
    for (int i = 2; i < argc; i++) {
      std::string kv = argv[i];
      auto eq = kv.find('=');
      if (eq == std::string::npos) continue;
      std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
      if (v == "true" || v == "false") s[k] = (v == "true");
      else if (!v.empty() && (isdigit(v[0]) || v[0] == '.' || (v[0] == '-' && v.size() > 1))) {  // numbers (panX=-0.5 too)
        try { size_t used = 0; double d = std::stod(v, &used); if (used == v.size() && std::isfinite(d)) s[k] = d; else s[k] = v; }
        catch (const std::exception&) { s[k] = v; }  // "." / "1e999": not a number, send as string
      } else s[k] = v;
    }
    return cmdClient(sock, json{ { "cmd", "set" }, { "settings", s } }.dump(), true);
  }
  if (cmd == "camera" && argc > 2) return cmdClient(sock, json{ { "cmd", "set" }, { "camera", argv[2] } }.dump(), true);
  if (cmd == "preview") {
    // The preview is per connection: `on` holds the connection (and so the
    // pipeline and preview.jpg) until this process exits.
    bool on = argc > 2 && std::string(argv[2]) == "on";
    return cmdClient(sock, json{ { "cmd", "preview" }, { "on", on } }.dump(), true, on);
  }
  if (cmd == "quit") return cmdClient(sock, R"({"cmd":"quit"})", true);
  if (cmd == "snap") return cmdClient(sock, R"({"cmd":"snapshot"})", true, false, "snapshot");  // prints the PNG path
  if (cmd == "profile") return cmdClient(sock, json{ { "cmd", "profile" }, { "on", argc > 2 && std::string(argv[2]) == "on" } }.dump(), true);
  if (cmd != "run") {
    fprintf(stderr, "usage: camera-effects-server [run|status|set k=v...|react NAME|camera BUS|snap|preview on (holds while running)|off|profile on|off|quit]\n");
    return 2;
  }
  signal(SIGINT, onSignal);
  signal(SIGTERM, onSignal);
  signal(SIGPIPE, SIG_IGN);
  Daemon d(cfg);
  return d.run();
}
