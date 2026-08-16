// camfxd — system-wide camera effects daemon for Omarchy.
//
// Reads a physical webcam, applies effects (Center Stage, Portrait, Studio
// Light, background replacement, Reactions) and publishes the result as a
// virtual V4L2 camera ("Omarchy Camera") that every app can use. Runs the
// camera only while some app has the virtual camera open.
#include <fcntl.h>
#include <linux/videodev2.h>
#include <pwd.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/file.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <map>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <thread>

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
  return json{ { "centerStage", s.centerStage }, { "portrait", s.portrait }, { "portraitIntensity", s.portraitIntensity },
               { "studioLight", s.studioLight }, { "studioLightIntensity", s.studioLightIntensity }, { "background", s.background },
               { "backgroundImage", s.backgroundImage }, { "backgroundColor", s.backgroundColor }, { "reactions", s.reactions }, { "mirror", s.mirror } };
}

void settingsFromJson(const json& j, Settings& s) {
  if (!j.is_object()) return;
  auto getb = [&](const char* k, bool& v) { if (j.contains(k) && j[k].is_boolean()) v = j[k]; };
  auto getf = [&](const char* k, float& v) { if (j.contains(k) && j[k].is_number()) v = std::clamp(j[k].get<float>(), 0.f, 1.f); };
  auto gets = [&](const char* k, std::string& v) { if (j.contains(k) && j[k].is_string()) v = j[k]; };
  getb("centerStage", s.centerStage); getb("portrait", s.portrait); getf("portraitIntensity", s.portraitIntensity);
  getb("studioLight", s.studioLight); getf("studioLightIntensity", s.studioLightIntensity);
  gets("background", s.background); gets("backgroundImage", s.backgroundImage); gets("backgroundColor", s.backgroundColor);
  getb("reactions", s.reactions); getb("mirror", s.mirror);
  if (s.background != "none" && s.background != "image" && s.background != "color") s.background = "none";
  unsigned rgb;
  if (!parseHexColor(s.backgroundColor, rgb)) s.backgroundColor = Settings().backgroundColor;
}

// ---------------------------------------------------------------------------
// Client mode: talk to a running daemon.

int cmdClient(const std::string& sockPath, const std::string& request, bool wait) {
  sockaddr_un addr{};
  if (sockPath.size() >= sizeof(addr.sun_path)) { fprintf(stderr, "socket path too long: %s\n", sockPath.c_str()); return 1; }
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);
  if (connect(fd, (sockaddr*)&addr, sizeof addr) < 0) { fprintf(stderr, "daemon not running (%s)\n", sockPath.c_str()); return 1; }
  std::string msg = request + "\n";
  send(fd, msg.data(), msg.size(), 0);
  // Read the greeting state + reply.
  std::string acc;
  char buf[8192];
  int lines = 0;
  while (lines < (wait ? 2 : 1)) {
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n <= 0) break;
    acc.append(buf, n);
    size_t pos;
    while ((pos = acc.find('\n')) != std::string::npos) {
      std::string line = acc.substr(0, pos);
      acc.erase(0, pos + 1);
      lines++;
      if (lines == (wait ? 2 : 1)) { printf("%s\n", line.c_str()); }
    }
  }
  close(fd);
  return 0;
}

// ---------------------------------------------------------------------------
// Daemon

struct Config {
  std::string label = "Omarchy Camera";
  int outW = 1280, outH = 720, fps = 30;
  int capW = 1920, capH = 1080;
  std::string preferredCamera;  // bus info or path
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
    if (j.contains("camera") && j["camera"].is_string()) c.preferredCamera = j["camera"];
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
  json j = { { "loopback", { { "label", c.label } } },
             { "output", { { "width", c.outW }, { "height", c.outH }, { "fps", c.fps } } },
             { "capture", { { "width", c.capW }, { "height", c.capH } } },
             { "camera", c.preferredCamera },
             { "sameForAll", c.sameForAll },
             { "settings", settingsToJson(c.settings) },
             { "settingsByCamera", byCam } };
  std::string dir = c.configPath.substr(0, c.configPath.rfind('/'));
  mkdir(dir.c_str(), 0755);
  std::string tmp = c.configPath + ".tmp";
  { std::ofstream f(tmp); f << j.dump(2) << "\n"; }
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
  std::atomic<int> consumers_{0};
  std::atomic<bool> stateDirty_{true};
  // State shown to clients. Written by the main thread and read by the
  // server thread (stateJson): every write happens under mu_.
  std::vector<CameraInfo> cameras_;
  CameraInfo current_;
  bool running_ = false;
  double fps_ = 0;
  double procMs_ = 0;   // EMA of per-frame processing time
  std::string error_, loopPath_, pwStatus_ = "off", gesture_, profileLine_;
  bool forcePreview_ = false;  // keep running even without consumers (debug)
  bool hideRawActive_ = false;
  std::string runtimeDir_;
  int misses_ = 0;      // consecutive grab timeouts

  // Set a client-visible field (main thread) and mark the state dirty if it changed.
  template <class T> void setState(T& field, const T& value) {
    std::lock_guard<std::mutex> lk(mu_);
    if (field == value) return;
    field = value;
    stateDirty_ = true;
  }
  void setError(const std::string& e) { setState(error_, e); }
  json stateJson();
  void publishState();
  // Settings that apply to the current camera (global when sameForAll).
  Settings& effectiveSettings();
  std::string selectedBus();
  static bool cameraHidden(const CameraInfo& c) { return !c.key.empty() && fileExists("/etc/udev/rules.d/71-omarchy-camera-hide-" + c.key + ".rules"); }
  std::string handle(const std::string& req);
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
  return json{ { "type", "state" },
               { "running", running_ },
               { "consumers", consumers_.load() },
               { "camera", { { "path", shown.path }, { "name", shown.name }, { "bus", shown.bus }, { "key", shown.key } } },
               { "cameras", cams },
               { "loopback", loopPath_ },
               { "loopbackLabel", cfg_.label },
               { "pipewire", pwStatus_ },
               { "pipewireActive", pwActive_.load() },
               { "output", { { "width", cfg_.outW }, { "height", cfg_.outH }, { "fps", cfg_.fps } } },
               { "fps", (int)std::lround(fps_) },
               { "procMs", (int)std::lround(procMs_) },
               { "settings", settingsToJson(effectiveSettings()) },
               { "sameForAll", cfg_.sameForAll },
               { "models", fx_.modelStatus() },
               { "gesture", gesture_ },
               { "profile", profileLine_ },
               { "reactions", Reactions::names() },
               { "hideRaw", hideRawActive_ },
               { "error", error_ } };
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

bool Daemon::openCapture(std::string* err) {
  std::string pref;
  { std::lock_guard<std::mutex> lk(mu_); pref = cfg_.preferredCamera; }
  if (!pref.empty() && pref[0] == '/' && pref.rfind("/dev/", 0) != 0 && fileExists(pref)) {
    if (!file_.open(pref, cfg_.fps, err)) return false;
    useFile_ = true;
    std::lock_guard<std::mutex> lk(mu_);
    current_ = CameraInfo{ pref, "File: " + pref.substr(pref.rfind('/') + 1), pref, "" };
    return true;
  }
  useFile_ = false;
  CameraInfo c = pickCamera();
  if (c.path.empty()) { *err = "no camera found"; return false; }
  if (!cap_.open(c.path, cfg_.capW, cfg_.capH, cfg_.fps, err)) return false;
  std::lock_guard<std::mutex> lk(mu_);
  current_ = c;
  return true;
}
bool Daemon::captureOpen() const { return useFile_ ? file_.isOpen() : cap_.isOpen(); }
void Daemon::captureClose() { cap_.close(); file_.close(); std::lock_guard<std::mutex> lk(mu_); current_ = CameraInfo{}; }
bool Daemon::captureGrab(cv::Mat& f, int ms) { return useFile_ ? file_.grab(f, ms) : cap_.grab(f, ms); }

// Runs on the server thread. Requests come from same-uid clients but may be
// malformed: every field is type-checked and nothing here may throw.
std::string Daemon::handle(const std::string& req) {
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
        if (j.contains("settings") && j["settings"].is_object()) settingsFromJson(j["settings"], effectiveSettings());
        saveConfig(cfg_);
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
    if (cmd == "preview") { std::lock_guard<std::mutex> lk(mu_); forcePreview_ = boolField("on"); stateDirty_ = true; return kOk; }
    if (cmd == "profile") { fx_.setProfile(boolField("on")); return kOk; }
    if (cmd == "quit") { g_quit = true; return kOk; }
    return error("unknown cmd");
  } catch (const std::exception& e) {
    fprintf(stderr, "camfxd: request failed: %s\n", e.what());
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
  close(fd);
  if (!ok) { *err = dir + " must be a directory owned by us with mode 0700"; return false; }
  return true;
}

int Daemon::run() {
  // The shell owns us: exit with it rather than linger as an orphan the next
  // shell cannot manage (it also asks a stray instance to quit, belt and braces).
  prctl(PR_SET_PDEATHSIG, SIGTERM);
  runtimeDir_ = xdgRuntime() + "/omarchy-camera";
  std::string err;
  if (!ensureRuntimeDir(runtimeDir_, &err)) { fprintf(stderr, "camfxd: %s\n", err.c_str()); return 1; }
  std::string sockPath = runtimeDir_ + "/ctl.sock";
  // Single instance per session (the shell restarts us; a stray manual copy
  // must not fight over the loopback writer).
  int lockFd = open((runtimeDir_ + "/lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (lockFd < 0 || flock(lockFd, LOCK_EX | LOCK_NB) < 0) { fprintf(stderr, "camfxd: another instance is running\n"); return 3; }

  if (!fx_.init(cfg_.modelsDir, cfg_.assetsDir, &err)) fprintf(stderr, "models: %s\n", err.c_str());
  else if (!err.empty()) fprintf(stderr, "models (degraded): %s\n", err.c_str());
  fx_.setSettings(cfg_.settings);

  if (!server_.start(sockPath, [this](const std::string& r) { return handle(r); }, &err)) { fprintf(stderr, "control socket: %s\n", err.c_str()); return 1; }

  // Native PipeWire camera node for portal-based apps (best effort).
  if (pwOut_.start(cfg_.outW, cfg_.outH, cfg_.fps, cfg_.label, [this](bool on) { pwActive_ = on; stateDirty_ = true; }, &err)) setState(pwStatus_, std::string("ok"));
  else { setState(pwStatus_, err); fprintf(stderr, "camfxd: pipewire output unavailable: %s\n", err.c_str()); }

  // The loopback device is created by the omarchy-camera-device systemd unit
  // (v4l2loopback-ctl, installed by `omarchy-camera-setup install`). Wait for
  // it rather than fail: the plugin's setup step may still be running.
  auto lastLoopCheck = clk::now() - std::chrono::seconds(10);
  auto lastScan = clk::now() - std::chrono::seconds(10);
  auto lastPublish = clk::now();
  bool wasRunning = false;
  int frames = 0;
  auto fpsT0 = clk::now();
  fprintf(stderr, "camfxd: ready (socket %s)\n", sockPath.c_str());

  while (!g_quit) {
    auto now = clk::now();
    if (!loop_.isOpen() && now - lastLoopCheck > std::chrono::seconds(2)) {
      lastLoopCheck = now;
      std::string p = findLoopbackByLabel(cfg_.label);
      if (p.empty()) setError("virtual camera device missing");
      else if (loop_.open(p, cfg_.outW, cfg_.outH, &err)) {
        setState(loopPath_, p);
        setError("");
        fprintf(stderr, "camfxd: virtual camera %s (%dx%d)\n", p.c_str(), cfg_.outW, cfg_.outH);
        watcher_.start(p, [this](int n) { consumers_ = n; stateDirty_ = true; });
      } else setError(err);
    }
    if (now - lastScan > std::chrono::seconds(running_ ? 10 : 4)) { lastScan = now; rescanCameras(); }
    setState(hideRawActive_, fileExists("/etc/udev/rules.d/71-omarchy-camera-hide-raw.rules"));

    bool preview;
    { std::lock_guard<std::mutex> lk(mu_); preview = forcePreview_; }
    bool want = (loop_.isOpen() && consumers_ > 0) || pwActive_ || preview;
    if (want && !captureOpen()) {
      if (openCapture(&err)) {
        setError("");
        misses_ = 0;
        fprintf(stderr, "camfxd: capturing %s %dx%d %s\n", current_.path.c_str(), useFile_ ? file_.width() : cap_.width(), useFile_ ? file_.height() : cap_.height(), useFile_ ? "file" : cap_.format().c_str());
      } else {
        if (error_ != err) fprintf(stderr, "camfxd: capture: %s\n", err.c_str());
        setError(err);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }
    }
    if (!want && captureOpen()) {
      captureClose();
      // Leave black behind so the next opener doesn't see a stale frame.
      cv::Mat black(cfg_.outH, cfg_.outW, CV_8UC3, cv::Scalar(0, 0, 0));
      if (loop_.isOpen()) loop_.write(black);
      pwOut_.clear();
      fprintf(stderr, "camfxd: idle\n");
    }
    setState(running_, captureOpen());
    if (running_ != wasRunning) { wasRunning = running_; frames = 0; fpsT0 = now; setState(fps_, 0.0); }

    if (running_) {
      cv::Mat frame, out;
      if (captureGrab(frame, 200)) {
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
        auto p0 = clk::now();
        // A bad frame or setting must not take the daemon down mid-call: log and skip the frame.
        try {
          fx_.process(frame, out, cv::Size(cfg_.outW, cfg_.outH), nowSec());
          if (loop_.isOpen()) loop_.write(out);
          pwOut_.push(out);
        } catch (const std::exception& e) {
          fprintf(stderr, "camfxd: frame dropped: %s\n", e.what());
          continue;
        }
        double ms = std::chrono::duration<double, std::milli>(clk::now() - p0).count();
        { std::lock_guard<std::mutex> lk(mu_); procMs_ = procMs_ == 0 ? ms : procMs_ * 0.9 + ms * 0.1; }  // shown with the next publish
        setState(gesture_, fx_.lastGesture());
        setState(profileLine_, fx_.profileLine());
        frames++;
        auto dt = std::chrono::duration<double>(now - fpsT0).count();
        if (dt >= 1.0) { setState(fps_, frames / dt); frames = 0; fpsT0 = now; }
      } else {
        // Camera unplugged or stalled: reopen.
        if (++misses_ > 15) { misses_ = 0; captureClose(); rescanCameras(); }
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }

    if (stateDirty_ && now - lastPublish > std::chrono::milliseconds(150)) {
      stateDirty_ = false;
      lastPublish = now;
      publishState();
    }
  }
  captureClose();
  pwOut_.stop();
  watcher_.stop();
  server_.stop();
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
    std::string models = fileExists(dir + "/../../models/pphumanseg.onnx") ? dir + "/../../models" : xdgData() + "/omarchy-camera/models";
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
  cfg.configPath = xdgConfig() + "/omarchy/camera.json";
  std::string exe(4096, '\0');
  ssize_t n = readlink("/proc/self/exe", exe.data(), exe.size() - 1);
  exe.resize(n > 0 ? n : 0);
  std::string exeDir = exe.substr(0, exe.rfind('/'));
  // Data dirs: installed layout first, then the source tree (daemon/build/../..).
  std::string dataDir = xdgData() + "/omarchy-camera";
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

  std::string sock = xdgRuntime() + "/omarchy-camera/ctl.sock";
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
      else if (!v.empty() && (isdigit(v[0]) || v[0] == '.')) s[k] = std::stod(v);
      else s[k] = v;
    }
    return cmdClient(sock, json{ { "cmd", "set" }, { "settings", s } }.dump(), true);
  }
  if (cmd == "camera" && argc > 2) return cmdClient(sock, json{ { "cmd", "set" }, { "camera", argv[2] } }.dump(), true);
  if (cmd == "preview") return cmdClient(sock, json{ { "cmd", "preview" }, { "on", argc > 2 && std::string(argv[2]) == "on" } }.dump(), true);
  if (cmd == "quit") return cmdClient(sock, R"({"cmd":"quit"})", true);
  if (cmd == "profile") return cmdClient(sock, json{ { "cmd", "profile" }, { "on", argc > 2 && std::string(argv[2]) == "on" } }.dump(), true);
  if (cmd != "run") {
    fprintf(stderr, "usage: camfxd [run|status|set k=v...|react NAME|camera BUS|preview on|off|profile on|off|quit]\n");
    return 2;
  }
  signal(SIGINT, onSignal);
  signal(SIGTERM, onSignal);
  signal(SIGPIPE, SIG_IGN);
  Daemon d(cfg);
  return d.run();
}
