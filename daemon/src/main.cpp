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
}

// ---------------------------------------------------------------------------
// v4l2loopback control (root): create/remove the virtual camera device.

struct v4l2_loopback_config {
  int32_t output_nr;
  int32_t unused;
  char card_label[32];
  uint32_t min_width, max_width, min_height, max_height;
  int32_t max_buffers;
  int32_t max_openers;
  int32_t debug;
  int32_t announce_all_caps;
};
#define V4L2LOOPBACK_CTL_ADD _IOW('~', 1, struct v4l2_loopback_config)
#define V4L2LOOPBACK_CTL_REMOVE _IOW('~', 2, uint32_t)

int cmdDevice(int argc, char** argv) {
  std::string op = argc > 0 ? argv[0] : "";
  int nr = 20;
  std::string label = "Omarchy Camera";
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--nr" && i + 1 < argc) nr = atoi(argv[++i]);
    else if (a == "--label" && i + 1 < argc) label = argv[++i];
  }
  if (op == "add") {
    std::string existing = findLoopbackByLabel(label);
    if (!existing.empty()) { printf("%s\n", existing.c_str()); return 0; }
    if (!fileExists("/dev/v4l2loopback")) {
      if (system("modprobe v4l2loopback") != 0 || !fileExists("/dev/v4l2loopback")) { fprintf(stderr, "v4l2loopback module not available\n"); return 1; }
    }
    int fd = open("/dev/v4l2loopback", O_RDWR);
    if (fd < 0) { perror("open /dev/v4l2loopback"); return 1; }
    v4l2_loopback_config cfg{};
    cfg.output_nr = nr;
    cfg.unused = -1;
    strncpy(cfg.card_label, label.c_str(), sizeof(cfg.card_label) - 1);
    cfg.max_openers = 16;
    cfg.max_buffers = 4;
    cfg.announce_all_caps = 0;  // == exclusive_caps=1: looks like a capture device only while we feed it
    int r = ioctl(fd, V4L2LOOPBACK_CTL_ADD, &cfg);
    close(fd);
    if (r < 0) { perror("V4L2LOOPBACK_CTL_ADD"); return 1; }
    printf("/dev/video%d\n", r);
    return 0;
  }
  if (op == "remove") {
    std::string path = findLoopbackByLabel(label);
    if (path.empty()) return 0;
    int n = atoi(path.c_str() + strlen("/dev/video"));
    int fd = open("/dev/v4l2loopback", O_RDWR);
    if (fd < 0) { perror("open /dev/v4l2loopback"); return 1; }
    uint32_t dev = n;
    int r = ioctl(fd, V4L2LOOPBACK_CTL_REMOVE, &dev);
    close(fd);
    if (r < 0) { perror("V4L2LOOPBACK_CTL_REMOVE"); return 1; }
    return 0;
  }
  if (op == "find") {
    std::string path = findLoopbackByLabel(label);
    if (path.empty()) return 1;
    printf("%s\n", path.c_str());
    return 0;
  }
  fprintf(stderr, "usage: camfxd device add|remove|find [--nr N] [--label NAME]\n");
  return 2;
}

// ---------------------------------------------------------------------------
// Client mode: talk to a running daemon.

int cmdClient(const std::string& sockPath, const std::string& request, bool wait) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un addr{};
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
    if (j.contains("loopback") && j["loopback"].contains("label")) c.label = j["loopback"]["label"];
    if (j.contains("output")) {
      auto& o = j["output"];
      if (o.contains("width")) c.outW = o["width"]; if (o.contains("height")) c.outH = o["height"]; if (o.contains("fps")) c.fps = o["fps"];
    }
    if (j.contains("capture")) {
      auto& o = j["capture"];
      if (o.contains("width")) c.capW = o["width"]; if (o.contains("height")) c.capH = o["height"];
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
  std::string pwStatus_ = "off";
  ConsumerWatcher watcher_;
  ControlServer server_;
  V4L2Capture cap_;
  FileCapture file_;   // dev/test source when the selected "camera" is a file path
  bool useFile_ = false;
  std::vector<CameraInfo> cameras_;
  CameraInfo current_;
  std::atomic<int> consumers_{0};
  std::atomic<bool> stateDirty_{true};
  bool running_ = false;
  double fps_ = 0;
  double procMs_ = 0;   // EMA of per-frame processing time
  std::string error_, loopPath_, statePath_, runtimeDir_;
  bool forcePreview_ = false;  // keep running even without consumers (debug)
  bool hideRawActive_ = false;

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
               { "gesture", fx_.lastGesture() },
               { "profile", fx_.profileLine() },
               { "reactions", Reactions::names() },
               { "hideRaw", hideRawActive_ },
               { "error", error_ } };
}

void Daemon::publishState() {
  json j;
  { std::lock_guard<std::mutex> lk(mu_); j = stateJson(); }
  std::string s = j.dump();
  server_.broadcast(s);
  std::string tmp = statePath_ + ".tmp";
  { std::ofstream f(tmp); f << s << "\n"; }
  rename(tmp.c_str(), statePath_.c_str());
}

void Daemon::rescanCameras() {
  std::vector<CameraInfo> cams;
  { std::lock_guard<std::mutex> credLk(g_credMutex); cams = enumerateCameras(loopPath_); }
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
    current_ = CameraInfo{ pref, "File: " + pref.substr(pref.rfind('/') + 1), pref };
    return true;
  }
  useFile_ = false;
  CameraInfo c = pickCamera();
  if (c.path.empty()) { *err = "no camera found"; return false; }
  std::lock_guard<std::mutex> credLk(g_credMutex);
  if (!cap_.open(c.path, cfg_.capW, cfg_.capH, cfg_.fps, err)) return false;
  std::lock_guard<std::mutex> lk(mu_);
  current_ = c;
  return true;
}
bool Daemon::captureOpen() const { return useFile_ ? file_.isOpen() : cap_.isOpen(); }
void Daemon::captureClose() { cap_.close(); file_.close(); std::lock_guard<std::mutex> lk(mu_); current_ = CameraInfo{}; }
bool Daemon::captureGrab(cv::Mat& f, int ms) { return useFile_ ? file_.grab(f, ms) : cap_.grab(f, ms); }

std::string Daemon::handle(const std::string& req) {
  json j;
  try { j = json::parse(req); } catch (...) { return R"({"type":"error","error":"bad json"})"; }
  std::string cmd = j.value("cmd", "");
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
      if (j.contains("settings")) {
        Settings& target = effectiveSettings();
        settingsFromJson(j["settings"], target);
      }
      saveConfig(cfg_);
    }
    stateDirty_ = true;
    return R"({"type":"ok"})";
  }
  if (cmd == "react") { fx_.triggerReaction(j.value("name", "")); return R"({"type":"ok"})"; }
  if (cmd == "rescan") { rescanCameras(); stateDirty_ = true; return R"({"type":"ok"})"; }
  if (cmd == "preview") { std::lock_guard<std::mutex> lk(mu_); forcePreview_ = j.value("on", false); stateDirty_ = true; return R"({"type":"ok"})"; }
  if (cmd == "profile") { fx_.setProfile(j.value("on", false)); return R"({"type":"ok"})"; }
  if (cmd == "quit") { g_quit = true; return R"({"type":"ok"})"; }
  return R"({"type":"error","error":"unknown cmd"})";
}

int Daemon::run() {
  runtimeDir_ = xdgRuntime() + "/omarchy-camera";
  mkdir(runtimeDir_.c_str(), 0700);
  statePath_ = runtimeDir_ + "/state.json";
  std::string sockPath = runtimeDir_ + "/ctl.sock";
  // Single instance per session (the shell restarts us; a stray manual copy
  // must not fight over the loopback writer).
  int lockFd = open((runtimeDir_ + "/lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lockFd < 0 || flock(lockFd, LOCK_EX | LOCK_NB) < 0) { fprintf(stderr, "camfxd: another instance is running\n"); return 3; }

  std::string err;
  if (!fx_.init(cfg_.modelsDir, cfg_.assetsDir, &err)) fprintf(stderr, "models: %s\n", err.c_str());
  else if (!err.empty()) fprintf(stderr, "models (degraded): %s\n", err.c_str());
  fx_.setSettings(cfg_.settings);

  if (!server_.start(sockPath, [this](const std::string& r) { return handle(r); }, &err)) { fprintf(stderr, "control socket: %s\n", err.c_str()); return 1; }

  // Native PipeWire camera node for portal-based apps (best effort).
  if (pwOut_.start(cfg_.outW, cfg_.outH, cfg_.fps, cfg_.label, [this](bool on) { pwActive_ = on; stateDirty_ = true; }, &err)) pwStatus_ = "ok";
  else { pwStatus_ = err; fprintf(stderr, "camfxd: pipewire output unavailable: %s\n", err.c_str()); }

  // The loopback device is created by `camfxd device add` (root, at boot).
  // Wait for it rather than fail: the plugin's setup step may still be running.
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
      if (p.empty()) { if (error_ != "virtual camera device missing") { error_ = "virtual camera device missing"; stateDirty_ = true; } }
      else if (loop_.open(p, cfg_.outW, cfg_.outH, &err)) {
        loopPath_ = p; error_.clear(); stateDirty_ = true;
        fprintf(stderr, "camfxd: virtual camera %s (%dx%d)\n", p.c_str(), cfg_.outW, cfg_.outH);
        watcher_.start(p, [this](int n) { consumers_ = n; stateDirty_ = true; });
      } else if (error_ != err) { error_ = err; stateDirty_ = true; }
    }
    if (now - lastScan > std::chrono::seconds(running_ ? 10 : 4)) { lastScan = now; rescanCameras(); }
    hideRawActive_ = fileExists("/etc/udev/rules.d/71-omarchy-camera-hide-raw.rules");

    bool want = (loop_.isOpen() && consumers_ > 0) || pwActive_ || forcePreview_;
    if (want && !captureOpen()) {
      if (openCapture(&err)) { error_.clear(); stateDirty_ = true; fprintf(stderr, "camfxd: capturing %s %dx%d %s\n", current_.path.c_str(), useFile_ ? file_.width() : cap_.width(), useFile_ ? file_.height() : cap_.height(), useFile_ ? "file" : cap_.format().c_str()); }
      else { if (error_ != err) { error_ = err; stateDirty_ = true; fprintf(stderr, "camfxd: capture: %s\n", err.c_str()); } std::this_thread::sleep_for(std::chrono::milliseconds(500)); }
    }
    if (!want && captureOpen()) {
      captureClose();
      // Leave black behind so the next opener doesn't see a stale frame.
      cv::Mat black(cfg_.outH, cfg_.outW, CV_8UC3, cv::Scalar(0, 0, 0));
      if (loop_.isOpen()) loop_.write(black);
      pwOut_.clear();
      fprintf(stderr, "camfxd: idle\n");
    }
    running_ = captureOpen();
    if (running_ != wasRunning) { wasRunning = running_; stateDirty_ = true; frames = 0; fpsT0 = now; fps_ = 0; }

    if (running_) {
      cv::Mat frame, out;
      if (captureGrab(frame, 200)) {
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
        fx_.process(frame, out, cv::Size(cfg_.outW, cfg_.outH), nowSec());
        if (loop_.isOpen()) loop_.write(out);
        pwOut_.push(out);
        double ms = std::chrono::duration<double, std::milli>(clk::now() - p0).count();
        procMs_ = procMs_ == 0 ? ms : procMs_ * 0.9 + ms * 0.1;
        frames++;
        auto dt = std::chrono::duration<double>(now - fpsT0).count();
        if (dt >= 1.0) { fps_ = frames / dt; frames = 0; fpsT0 = now; stateDirty_ = true; }
      } else {
        // Camera unplugged or stalled: reopen.
        static int misses = 0;
        if (++misses > 15) { misses = 0; captureClose(); rescanCameras(); }
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
  unlink(statePath_.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string cmd = argc > 1 ? argv[1] : "run";
  if (cmd == "device") return cmdDevice(argc - 2, argv + 2);
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
    fprintf(stderr, "usage: camfxd [run|status|set k=v...|react NAME|camera BUS|preview on|off|quit|device ...]\n");
    return 2;
  }
  signal(SIGINT, onSignal);
  signal(SIGTERM, onSignal);
  signal(SIGPIPE, SIG_IGN);
  Daemon d(cfg);
  return d.run();
}
