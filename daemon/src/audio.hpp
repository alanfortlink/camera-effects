#pragma once
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "mic.hpp"

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_registry;
struct pw_stream;
struct spa_hook;

// A microphone PipeWire tells us about.
struct MicSourceInfo {
  std::string name;         // node.name: what we target, and what the hide rule matches
  std::string description;  // node.description / nick, for the panel
  unsigned id = 0;          // registry id (logging only)
  bool bluetooth = false;
};

// The microphone half of the daemon: reads one real microphone, runs it
// through MicProcessor and publishes the result as an "Microphone Effects"
// PipeWire source that every app can pick — the audio counterpart of the
// virtual camera.
//
// Two PipeWire streams in one node.link-group, like module-filter-chain's
// noise-cancelling source: a capture stream linked to the chosen mic, and an
// Audio/Source stream apps link to. The link group keeps both in the same
// graph cycle (so there is no clock drift between them) and stops PipeWire
// from ever linking our own source back into our own capture.
//
// The real mic is only opened while some app uses the virtual one, while the
// panel asks for the level meter (setMonitor), or while the user is listening
// to themselves (setListen) — the same on-demand rule the camera follows.
class AudioEngine {
public:
  ~AudioEngine();
  bool start(const std::string& label, std::string* err);
  void stop();
  bool started() const { return source_ != nullptr; }
  // Call periodically from the owning thread: (re)connects after an error,
  // opens/closes the capture stream as the demand changes, and switches mic
  // when setSource asked for another one. Returns the status to show.
  std::string maintain(double now);

  void setSource(const std::string& nodeName);   // "" = PipeWire's default source
  std::vector<MicSourceInfo> sources() const;
  MicSourceInfo currentSource() const;           // the mic we are reading now

  void setSettings(const MicSettings& s);
  void setMuted(bool m) { muted_ = m; }   // apps get silence and the mic is released
  void setMonitor(bool on) { monitor_ = on; }    // hold the mic open for the panel's meter
  // Play the processed microphone back through the speakers, so the user can
  // hear exactly what apps get. The daemon owns whether this is remembered;
  // here it is just on or off.
  void setListen(bool on) { listenWanted_ = on; }
  bool listening() const { return listening_.load(); }   // the playback stream is running

  bool active() const { return active_.load(); }        // an app is linked to the virtual mic
  int consumers() const { return consumers_.load(); }   // how many are
  bool capturing() const { return capturing_.load(); }  // the real mic is open
  float inLevel() const { return capturing_.load() ? proc_.inLevel() : 0.f; }
  float outLevel() const { return capturing_.load() ? proc_.outLevel() : 0.f; }
  int rate() const { return rate_.load(); }
  static const char* kNodeName;      // node.name of the virtual microphone

private:
  friend struct AudioCallbacks;
  bool startImpl(const std::string& label, std::string* err);
  bool openCapture(std::string* err);   // loop must be locked
  void closeCapture();                  // loop must be locked
  bool openListen(std::string* err);    // loop must be locked
  void closeListen();                   // loop must be locked
  bool openNudge(const std::string& target, std::string* err);   // loop must be locked
  void closeNudge();                    // loop must be locked
  void onNudgeProcess();
  void onCaptureProcess();
  void onPlaybackProcess();
  void onListenProcess();
  void onListenState(int old, int state, const char* error);
  // One reader's share of the ring: fills out[0..n), zero-padding an underrun.
  int readRing(std::atomic<unsigned>& rp, float* out, int n);
  void onCaptureParam(unsigned id, const void* param);
  void onPlaybackParam(unsigned id, const void* param);
  void onCaptureState(int old, int state, const char* error);
  void onPlaybackState(int old, int state, const char* error);
  void onCoreError(unsigned id, int seq, int res, const char* message);
  void onGlobal(unsigned id, const char* type, const struct spa_dict* props);
  void onGlobalRemove(unsigned id);
  void updateLinks();   // caller holds mu_: who is linked to us, and to what

  pw_thread_loop* loop_ = nullptr;
  pw_context* context_ = nullptr;
  pw_core* core_ = nullptr;
  pw_registry* registry_ = nullptr;
  pw_stream* source_ = nullptr;    // Audio/Source node apps see
  pw_stream* capture_ = nullptr;   // linked to the real mic (only while wanted)
  pw_stream* listen_ = nullptr;    // playback of the result (only while listening)
  pw_stream* nudge_ = nullptr;     // headset-profile helper (only for Bluetooth mics)
  spa_hook *sourceHook_ = nullptr, *captureHook_ = nullptr, *listenHook_ = nullptr, *nudgeHook_ = nullptr,
           *coreHook_ = nullptr, *registryHook_ = nullptr;

  std::string label_ = "Microphone Effects";
  std::atomic<bool> active_{false}, capturing_{false}, muted_{false}, monitor_{false};
  std::atomic<bool> listenWanted_{false}, listening_{false}, listenLost_{false}, captureLost_{false};
  std::atomic<int> consumers_{0};
  std::atomic<bool> failed_{false}, sourceDirty_{false};
  std::atomic<int> rate_{48000};
  std::atomic<int> procRate_{48000};   // format the data thread should size the DSP for
  std::string status_ = "off";
  bool everConnected_ = false;
  double retryAt_ = 0, backoff_ = 2, okSince_ = -1, listenRetryAt_ = 0, listenOpenedAt_ = 0;
  double captureRetryAt_ = 0, nudgeRetryAt_ = 0;   // an open that failed is not retried every tick
  int listenFails_ = 0;       // consecutive listen streams that died soon after opening
  std::string nudgeTarget_;   // Bluetooth node the profile helper is attached to

  mutable std::mutex mu_;             // sources_, links_, want-/currentSource_
  std::map<unsigned, MicSourceInfo> sources_;
  // Links PipeWire knows about, id -> (output node, input node). A pw_stream
  // that publishes a source node sits in STREAMING whether or not anything
  // listens, so "is an app using the virtual mic" is answered from here.
  std::map<unsigned, std::pair<unsigned, unsigned>> links_;
  unsigned sourceNode_ = 0xffffffffu, captureNode_ = 0xffffffffu;
  std::string wantedSource_, currentSource_;

  MicProcessor proc_;                 // touched by the capture callback only
  bool wasMuted_ = false;             // data thread: reset the chain once when mute starts
  std::mutex setMu_;
  MicSettings pending_;
  std::atomic<bool> pendingDirty_{false};

  // Capture -> playback handoff. The capture and source callbacks run in the
  // PipeWire data thread of the same graph cycle, so a small ring is all it
  // takes. Each reader (the source apps see, and the listen stream, which
  // follows the sink's clock instead) keeps its own read index; the writer
  // never moves them, it only appends.
  static constexpr int kRing = 1 << 15;
  std::vector<float> ring_;
  std::atomic<unsigned> rw_{0}, rr_{0}, rl_{0};
  std::vector<float> work_;           // de-interleaved / scratch block
};
