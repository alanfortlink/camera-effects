#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <opencv2/core.hpp>
#include <string>
#include <vector>

#include "gestures.hpp"
#include "models.hpp"

// User-facing effect settings. Mirrors what the shell panel shows.
struct Settings {
  bool centerStage = false;
  bool portrait = false;            // background blur
  float portraitIntensity = 0.6f;   // 0..1
  bool studioLight = false;
  float studioLightIntensity = 0.6f;
  std::string background = "none";  // none | image | color
  std::string backgroundImage;      // path
  std::string backgroundColor = "#1e1e2e";
  bool reactions = true;            // gesture-triggered reactions
  bool mirror = false;
  int rotate = 0;                   // 0 | 90 | 180 | 270 (clockwise), applied before everything else
  std::string filter = "none";      // colour filter, one of filterNames()
  std::string fun = "none";         // face accessory, one of funNames()
  // Manual framing (see Framing): how the source is fitted, zoom, pan. With
  // Center Stage on, zoom is the minimum zoom and pan is automatic.
  std::string fit = "cover";
  float zoom = 1;                   // 1..4
  float panX = 0, panY = 0;         // -1..1
  bool operator==(const Settings& o) const {
    return centerStage == o.centerStage && portrait == o.portrait && portraitIntensity == o.portraitIntensity &&
           studioLight == o.studioLight && studioLightIntensity == o.studioLightIntensity && background == o.background &&
           backgroundImage == o.backgroundImage && backgroundColor == o.backgroundColor && reactions == o.reactions && mirror == o.mirror &&
           rotate == o.rotate && filter == o.filter && fun == o.fun && fit == o.fit && zoom == o.zoom && panX == o.panX && panY == o.panY;
  }
  // Valid values for `filter` / `fun` ("none" first).
  static const std::vector<std::string>& filterNames();
  static const std::vector<std::string>& funNames();
};

// How a source is placed on the output: `fit` cover (largest output-aspect
// part of the source fills the output), contain (whole source, letterboxed on
// black) or stretch (whole source, aspect ignored); then `zoom` (1..4) into
// it and `pan` (-1..1 per axis) the visible part across the room the zoom
// leaves. Shared by the camera pipeline and the block placeholder.
struct Framing {
  std::string fit = "cover";
  float zoom = 1, panX = 0, panY = 0;
  bool operator==(const Framing& o) const { return fit == o.fit && zoom == o.zoom && panX == o.panX && panY == o.panY; }
  bool operator!=(const Framing& o) const { return !(*this == o); }
  static const std::vector<std::string>& fitNames();  // "cover" first
};

// Resolved framing: the part of the source (`crop`) shown in the part of the
// output (`dst`: the whole output unless contain leaves bars) and the pan
// room per axis as a fraction of the output size (0 = nothing to pan).
struct FrameGeom {
  cv::Rect crop, dst;
  cv::Point2f range;
  bool passthrough(const cv::Size& src, const cv::Size& out) const {
    return src == out && crop == cv::Rect(0, 0, src.width, src.height) && dst == cv::Rect(0, 0, out.width, out.height);
  }
};
FrameGeom frameGeometry(const cv::Size& src, const cv::Size& out, const Framing& f);

// Renders `img` into `dst` (an out-sized image, black where the source does
// not reach) per `f`; returns the geometry used. `dst` is reused when it
// already has the size.
FrameGeom fitFrame(const cv::Mat& img, cv::Mat& dst, const cv::Size& out, const Framing& f);

// Parses "#RRGGBB" (exactly 6 hex digits). Returns false on anything else.
bool parseHexColor(const std::string& s, unsigned& rgb);

// Scales `img` to fill `sz` (keeping its aspect) and crops the centre; `dst`
// is a fresh sz-sized image. Used for the background image.
void coverFit(const cv::Mat& img, cv::Mat& dst, const cv::Size& sz);

// Alpha-blits a BGRA sprite scaled by `scale` and rotated by `rot` (radians,
// counter-clockwise on screen) around its centre onto `frame` (BGR) at (cx, cy),
// with an extra global `alpha`. The rotated sprite is not clipped to its own
// box. Shared by the reactions and the fun overlays.
void alphaBlit(cv::Mat& frame, const cv::Mat& sprite, double cx, double cy, double scale, double alpha, double rot);

// Procedural drawing layer + 8-bit alpha, composited once per frame over the
// union bounding box of what was drawn (a cheap glow halo on request), so
// shapes get real transparency without a per-primitive blend. The buffers are
// kept between frames; only the previous frame's box is cleared.
class OverlayLayer {
public:
  void begin(const cv::Size& sz);  // per frame, before drawing (lazy: nothing is touched until a draw)
  void poly(const cv::Point* pts, int n, cv::Scalar col, double a);
  void line(cv::Point2f p0, cv::Point2f p1, cv::Scalar col, double a, int th);
  void dot(cv::Point2f c, int r, cv::Scalar col, double a);
  void ellipse(cv::Point2f c, cv::Size2f axes, double angleDeg, cv::Scalar col, double a, int th);
  bool used() const { return used_; }
  void composite(cv::Mat& frame, bool glow);  // blends what was drawn since begin() over `frame`

private:
  void ensure();
  void grow(cv::Rect r);
  cv::Size size_;
  cv::Mat overlay_, overlayA_, glowC_, glowA_;
  cv::Rect dirty_, bbox_;
  bool used_ = false;
};

// Center Stage: keeps the people in frame by choosing a crop of the (larger)
// source frame and easing towards it.
class Framer {
public:
  void reset();
  // Returns the crop rect (in src coordinates) for this frame. `minZoom` is
  // the user's zoom: the framer never shows more than the full crop / minZoom
  // (and no less than full / maxZoom, see effects.cpp).
  cv::Rect update(const cv::Size& src, const std::vector<cv::Rect2f>& faces, bool facesFresh, const cv::Size& out, double dt, double minZoom);
  cv::Rect fullCrop(const cv::Size& src, double outAspect) const;

private:
  bool init_ = false;
  double cx_ = 0, cy_ = 0, h_ = 0;          // current (eased) crop centre + height
  double tcx_ = 0, tcy_ = 0, th_ = 0;       // target
  double lastFaceTime_ = -1e9, now_ = 0;
};

// Emoji particle overlays. trigger() may be called from any thread; it only
// queues. Everything else runs on the frame (main) thread.
class Reactions {
public:
  bool loadAssets(const std::string& dir, std::string* err);
  void trigger(const std::string& name);  // hearts|thumbsup|thumbsdown|balloons|confetti|fireworks|rain|lasers
  bool active() const;                    // something playing or queued (any thread)
  void render(cv::Mat& frame, double now);
  static const std::vector<std::string>& names();

private:
  struct Particle { double x, y, vx, vy, size, phase, rot, delay, life; int kind; cv::Scalar color; };
  struct Anim { std::string name; double start; std::vector<Particle> parts; };
  std::map<std::string, cv::Mat> sprites_;  // BGRA
  std::vector<Anim> anims_;
  std::atomic<bool> playing_{false};        // anims_ non-empty, published by render for active()
  mutable std::mutex pendingMu_;
  std::vector<std::string> pending_;        // triggered, not yet started (drained by render)
  void drainPending();
  void spawn(Anim& a, const cv::Size& sz);
  // Sprite scaled so its larger side is `size` px.
  void blit(cv::Mat& frame, const cv::Mat& sprite, double cx, double cy, double size, double alpha, double rot);
  OverlayLayer layer_;
};

// Fun face filters: accessories (sprites or procedural shapes) anchored on
// YuNet's landmarks. Faces are fed at the detector's cadence (~10 Hz) and
// tracked with an EMA plus a short hold, so the accessories neither jitter
// nor vanish between detections. Main thread only.
class FunOverlay {
public:
  bool loadAssets(const std::string& dir, std::string* err);
  // New detection result (in the caller's coordinates; `now` in seconds).
  void update(const std::vector<Face>& faces, double now);
  void reset() { tracks_.clear(); }
  // Draw accessory `kind` (one of Settings::funNames()) for every live track,
  // mapping track coordinates into `frame` with (p - origin) * scale.
  void render(cv::Mat& frame, const std::string& kind, cv::Point2f origin, cv::Point2f scale, double now);

private:
  struct Track { cv::Point2f lm[5]; float size = 0; double lastSeen = 0; };
  std::vector<Track> tracks_;      // at most kMaxFaces
  std::map<std::string, cv::Mat> sprites_;  // BGRA, trimmed to their alpha box
  OverlayLayer layer_;
  static constexpr int kMaxFaces = 4;
  static constexpr double kHold = 0.5;   // seconds a face stays after its last detection
};

// Downscale pyramid of one frame (pyrDown chain: ~0.3 ms for 720p and
// alias-free), built lazily and shared by the segmenter, palm detection, YuNet
// and the portrait blur so nobody runs its own slow INTER_AREA resize.
class Pyramid {
public:
  void reset(const cv::Mat& base) { base_ = base; built_ = 0; }
  const cv::Mat& base() const { return base_; }
  // Level i is base / 2^i (level 0 = base itself).
  const cv::Mat& level(int i);
  // Smallest level that is still at least `minW` wide.
  const cv::Mat& atLeastWide(int minW, int* levelOut = nullptr);
  static double scaleOf(int level) { return 1.0 / (1 << level); }

private:
  cv::Mat base_, lv_[4];
  int built_ = 0;
};

class EffectPipeline {
public:
  bool init(const std::string& modelsDir, const std::string& assetsDir, std::string* err);
  void setSettings(const Settings& s);
  Settings settings() const { return settings_; }
  // Processes a source frame into an output of the given size. `out` may alias
  // `src` when nothing needs to be done (passthrough).
  void process(const cv::Mat& src, cv::Mat& out, const cv::Size& outSize, double now);
  // Thread-safe (queued); everything else is main-thread only.
  void triggerReaction(const std::string& name) { reactions_.trigger(name); }
  void setProfile(bool on) { profile_ = on; }
  // Adaptive quality tier (0 = full quality, 2 = cheapest); see main.cpp.
  void setTier(int t) { tier_ = std::clamp(t, 0, 2); }
  int tier() const { return tier_; }
  bool reactionsActive() const { return reactions_.active(); }  // any thread
  // Main thread only.
  std::string lastGesture() const { return gestures_.lastGesture(); }
  std::string modelStatus() const { return modelStatus_; }
  std::string profileLine() const { return profileLine_; }
  // Framing of the last processed frame (crop/dst/pan room, source size after
  // rotation, faces the framer saw): shown by the daemon's state.
  struct FramingInfo {
    FrameGeom geom;
    cv::Size src;
    int faces = 0;
    cv::Rect face;   // the largest face (source coordinates), empty when none
    bool operator==(const FramingInfo& o) const { return geom.crop == o.geom.crop && geom.dst == o.geom.dst && geom.range == o.geom.range && src == o.src && faces == o.faces && face == o.face; }
    bool operator!=(const FramingInfo& o) const { return !(*this == o); }
  };
  FramingInfo framing() const { return framing_; }

private:
  Settings settings_;
  PersonSegmenter segmenter_;
  FaceDetector faces_;
  GestureDetector gestures_;
  Framer framer_;
  Reactions reactions_;
  FunOverlay fun_;
  std::string modelStatus_;
  Pyramid pyr_, srcPyr_;
  cv::Mat work_;           // crop/resize target (reused)
  cv::Mat rot_;            // rotated source (Settings::rotate)
  FramingInfo framing_;
  cv::Mat maskSmall_;      // temporal EMA of the 192x192 probability (CV_32F)
  cv::Mat mask8s_, mask8_, mask3_;  // 8-bit person mask: model res, work res, and a 3-channel copy for the blends
  cv::Mat bg_, bgSmall_, pre_, blurF_;     // portrait/background buffers
  cv::Mat lit_, dark_, darkFactor_, lut_;  // studio light buffers
  float darkI_ = -1, lutI_ = -1;
  cv::Mat filt_, filtTmp_, filtSmall_, filtLut_, vig_;  // colour filter buffers (filtLut_/vig_ cached per filter/size)
  std::string filtLutFor_;
  cv::Mat bgImage_;        // cached background image (scaled to out)
  std::string bgImagePath_;
  cv::Size bgImageSize_;
  std::vector<Face> lastFaces_;         // source coordinates
  std::vector<cv::Rect2f> faceRects_;   // lastFaces_ boxes (for the framer)
  int frameIdx_ = 0;
  int tier_ = 0;
  std::atomic<bool> profile_{false};
  std::string profileLine_;
  double profAcc_[10] = {0};
  int profN_ = 0;
  double lastTime_ = 0;
  double lastGestureTime_ = 0;
  // Time-based cadences (capture fps varies, so nothing counts frames).
  double lastPalmTime_ = -1e9, lastFaceTime_ = -1e9, lastHandSeen_ = -1e9;
  std::string pendingGesture_;
  double pendingSince_ = 0;
  bool debugGestures_ = false;

  void computeMask(const cv::Mat& work);
  void applyBackground(cv::Mat& work);
  void applyStudioLight(cv::Mat& work);
  void applyFilter(cv::Mat& work);
  void ensureBackground(const cv::Size& sz);
};
