#pragma once
#include <chrono>
#include <map>
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
  bool operator==(const Settings& o) const {
    return centerStage == o.centerStage && portrait == o.portrait && portraitIntensity == o.portraitIntensity &&
           studioLight == o.studioLight && studioLightIntensity == o.studioLightIntensity && background == o.background &&
           backgroundImage == o.backgroundImage && backgroundColor == o.backgroundColor && reactions == o.reactions && mirror == o.mirror;
  }
};

// Center Stage: keeps the people in frame by choosing a crop of the (larger)
// source frame and easing towards it.
class Framer {
public:
  void reset();
  // Returns the crop rect (in src coordinates) for this frame.
  cv::Rect update(const cv::Size& src, const std::vector<cv::Rect2f>& faces, bool facesFresh, double outAspect, double dt);
  cv::Rect fullCrop(const cv::Size& src, double outAspect) const;

private:
  bool init_ = false;
  double cx_ = 0, cy_ = 0, h_ = 0;          // current (eased) crop centre + height
  double tcx_ = 0, tcy_ = 0, th_ = 0;       // target
  double lastFaceTime_ = -1e9, now_ = 0;
  double maxZoom_ = 2.2;
};

// Emoji particle overlays.
class Reactions {
public:
  bool loadAssets(const std::string& dir, std::string* err);
  void trigger(const std::string& name);  // hearts|thumbsup|thumbsdown|balloons|confetti|fireworks|rain|lasers
  bool active() const { return !anims_.empty(); }
  void render(cv::Mat& frame, double now);
  static const std::vector<std::string>& names();

private:
  struct Particle { double x, y, vx, vy, size, phase, rot, delay, life; int kind; cv::Scalar color; };
  struct Anim { std::string name; double start; std::vector<Particle> parts; };
  std::map<std::string, cv::Mat> sprites_;  // BGRA
  std::vector<Anim> anims_;
  void spawn(Anim& a, const cv::Size& sz);
  void blit(cv::Mat& frame, const cv::Mat& sprite, double cx, double cy, double size, double alpha, double rot);
  cv::Mat overlay_, overlayA_;  // procedural drawing layer + alpha, composited once per frame
};

class EffectPipeline {
public:
  bool init(const std::string& modelsDir, const std::string& assetsDir, std::string* err);
  void setSettings(const Settings& s);
  Settings settings() const { return settings_; }
  // Processes a source frame into an output of the given size.
  void process(const cv::Mat& src, cv::Mat& out, const cv::Size& outSize, double now);
  void triggerReaction(const std::string& name) { reactions_.trigger(name); }
  bool reactionsActive() const { return reactions_.active(); }
  std::string lastGesture() const { return gestures_.lastGesture(); }
  std::string modelStatus() const { return modelStatus_; }
  void setProfile(bool on) { profile_ = on; }
  std::string profileLine() const { return profileLine_; }

private:
  Settings settings_;
  PersonSegmenter segmenter_;
  FaceDetector faces_;
  GestureDetector gestures_;
  Framer framer_;
  Reactions reactions_;
  std::string modelStatus_;
  cv::Mat maskSmall_;      // temporal EMA of the 192x192 probability
  cv::Mat bgImage_;        // cached background image (scaled to out)
  std::string bgImagePath_;
  cv::Size bgImageSize_;
  std::vector<cv::Rect2f> lastFaces_;
  int frameIdx_ = 0;
  bool profile_ = false;
  std::string profileLine_;
  double profAcc_[8] = {0};
  int profN_ = 0;
  double lastTime_ = 0;
  double lastGestureTime_ = 0;

  void computeMask(const cv::Mat& work, cv::Mat& maskF);
  void applyBackground(cv::Mat& work, const cv::Mat& maskF);
  void applyStudioLight(cv::Mat& work, const cv::Mat& maskF);
  void ensureBackground(const cv::Size& sz);
};
