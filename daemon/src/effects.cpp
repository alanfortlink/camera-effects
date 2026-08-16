#include "effects.hpp"

#include <algorithm>
#include <cmath>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/geometry/2d.hpp>
#include <random>
#include <chrono>
#include <cstdio>
#include <cstdlib>

bool parseHexColor(const std::string& s, unsigned& rgb) {
  if (s.size() != 7 || s[0] != '#') return false;
  unsigned v = 0;
  for (size_t i = 1; i < 7; i++) {
    char c = s[i];
    unsigned d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return false;
    v = v * 16 + d;
  }
  rgb = v;
  return true;
}

// ---------------------------------------------------------------------------
// Framer (Center Stage)

void Framer::reset() { init_ = false; lastFaceTime_ = -1e9; }

cv::Rect Framer::fullCrop(const cv::Size& src, double outAspect) const {
  double w = src.width, h = src.height;
  if (w / h > outAspect) w = h * outAspect; else h = w / outAspect;
  return cv::Rect((int)((src.width - w) / 2), (int)((src.height - h) / 2), (int)w, (int)h);
}

cv::Rect Framer::update(const cv::Size& src, const std::vector<cv::Rect2f>& faces, bool facesFresh, double outAspect, double dt) {
  now_ += dt;
  cv::Rect full = fullCrop(src, outAspect);
  if (!init_) {
    cx_ = tcx_ = full.x + full.width / 2.0; cy_ = tcy_ = full.y + full.height / 2.0; h_ = th_ = full.height;
    init_ = true;
  }
  if (facesFresh) {
    if (!faces.empty()) {
      lastFaceTime_ = now_;
      // Union of faces, expanded to head + shoulders.
      float x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9, maxFaceH = 0;
      for (auto& f : faces) {
        x0 = std::min(x0, f.x); y0 = std::min(y0, f.y); x1 = std::max(x1, f.x + f.width); y1 = std::max(y1, f.y + f.height);
        maxFaceH = std::max(maxFaceH, f.height);
      }
      double ncx = (x0 + x1) / 2.0;
      double ncy = (y0 + y1) / 2.0 + 0.55 * maxFaceH;                    // headroom: face sits in the upper part
      double nh = std::max((double)(y1 - y0) + 3.4 * maxFaceH, ((x1 - x0) + 3.0 * maxFaceH) / outAspect);
      nh = std::clamp(nh, full.height / maxZoom_, (double)full.height);
      // Deadband so tiny detector jitter doesn't move the frame.
      if (std::abs(ncx - tcx_) > 0.035 * h_ || std::abs(ncy - tcy_) > 0.035 * h_ || std::abs(nh - th_) > 0.06 * h_) {
        tcx_ = ncx; tcy_ = ncy; th_ = nh;
      }
    } else if (now_ - lastFaceTime_ > 1.5) {
      tcx_ = full.x + full.width / 2.0; tcy_ = full.y + full.height / 2.0; th_ = full.height;
    }
  }
  // Ease: pan fairly quick, zoom-in slower than zoom-out.
  double aPan = 1 - std::exp(-dt / 0.35);
  double aZoom = 1 - std::exp(-dt / (th_ > h_ ? 0.5 : 1.0));
  cx_ += (tcx_ - cx_) * aPan; cy_ += (tcy_ - cy_) * aPan; h_ += (th_ - h_) * aZoom;
  double w = h_ * outAspect;
  if (w > src.width) { w = src.width; h_ = w / outAspect; }
  double x = std::clamp(cx_ - w / 2, 0.0, src.width - w);
  double y = std::clamp(cy_ - h_ / 2, 0.0, src.height - h_);
  return cv::Rect((int)x, (int)y, std::max(2, (int)w), std::max(2, (int)h_));
}

// ---------------------------------------------------------------------------
// Reactions

const std::vector<std::string>& Reactions::names() {
  static const std::vector<std::string> n = { "hearts", "thumbsup", "thumbsdown", "balloons", "confetti", "fireworks", "rain", "lasers" };
  return n;
}

bool Reactions::loadAssets(const std::string& dir, std::string* err) {
  const char* files[] = { "heart", "thumbsup", "thumbsdown", "balloon", "confetti", "party" };
  for (auto f : files) {
    cv::Mat m = cv::imread(dir + "/" + f + ".png", cv::IMREAD_UNCHANGED);
    if (m.empty() || m.channels() != 4) { if (err) *err = std::string("missing sprite ") + f; return false; }
    sprites_[f] = m;
  }
  return true;
}

void Reactions::trigger(const std::string& name) {
  if (std::find(names().begin(), names().end(), name) == names().end()) return;
  std::lock_guard<std::mutex> lk(pendingMu_);
  if (std::find(pending_.begin(), pending_.end(), name) == pending_.end()) pending_.push_back(name);
}

bool Reactions::active() {
  if (!anims_.empty()) return true;
  std::lock_guard<std::mutex> lk(pendingMu_);
  return !pending_.empty();
}

void Reactions::drainPending() {
  std::vector<std::string> names;
  { std::lock_guard<std::mutex> lk(pendingMu_); names.swap(pending_); }
  for (auto& name : names) {
    bool playing = false;
    for (auto& a : anims_) if (a.name == name) playing = true;
    if (playing) continue;
    Anim a; a.name = name; a.start = -1;  // spawned lazily on first render (needs frame size)
    anims_.push_back(a);
  }
}

namespace {
double easeOutBack(double k) { const double c1 = 1.70158, c3 = c1 + 1; return 1 + c3 * std::pow(k - 1, 3) + c1 * std::pow(k - 1, 2); }
double smooth(double k) { k = std::clamp(k, 0.0, 1.0); return k * k * (3 - 2 * k); }
// Per-particle envelope: fade in over 0.25 s, out over the last 0.5 s of its life.
double envelope(double t, double life) {
  if (t < 0 || t > life) return 0;
  return std::min(smooth(t / 0.25), smooth((life - t) / 0.5));
}
}  // namespace

void Reactions::spawn(Anim& a, const cv::Size& sz) {
  static std::mt19937 rng(42);
  std::uniform_real_distribution<double> U(0, 1);
  const double W = sz.width, H = sz.height;
  auto add = [&](int n, auto fn) { for (int i = 0; i < n; i++) { Particle p{}; p.life = 3.0; fn(p, i); a.parts.push_back(p); } };
  const cv::Scalar palette[] = { {90, 90, 255}, {60, 200, 255}, {110, 230, 90}, {255, 170, 60}, {255, 90, 220}, {60, 240, 240}, {255, 255, 255} };
  if (a.name == "hearts") add(34, [&](Particle& p, int i) {
    p.x = W * (0.08 + 0.84 * U(rng)); p.y = H * (1.02 + 0.15 * U(rng)); p.vy = -H * (0.28 + 0.22 * U(rng)); p.size = H * (0.05 + 0.09 * U(rng));
    p.phase = U(rng) * 6.28; p.delay = i * 0.06 + U(rng) * 0.1; p.life = 2.6 + U(rng) * 0.6; p.kind = 0; });
  else if (a.name == "balloons") add(16, [&](Particle& p, int i) {
    p.x = W * (0.05 + 0.9 * U(rng)); p.y = H * (1.05 + 0.2 * U(rng)); p.vy = -H * (0.22 + 0.16 * U(rng)); p.size = H * (0.13 + 0.09 * U(rng));
    p.phase = U(rng) * 6.28; p.delay = i * 0.12 + U(rng) * 0.1; p.life = 3.2; p.kind = 0; });
  else if (a.name == "thumbsup" || a.name == "thumbsdown") {
    add(1, [&](Particle& p, int) { p.x = W * 0.5; p.y = H * 0.6; p.size = H * 0.46; p.kind = 1; p.life = 3.0; });
    add(14, [&](Particle& p, int i) { p.x = W * (0.05 + 0.9 * U(rng)); p.y = H * (1.03 + 0.1 * U(rng)); p.vy = -H * (0.3 + 0.2 * U(rng)); p.size = H * (0.06 + 0.05 * U(rng));
      p.phase = U(rng) * 6.28; p.delay = 0.3 + i * 0.1 + U(rng) * 0.1; p.life = 2.4; p.kind = 0; });
  } else if (a.name == "confetti") add(220, [&](Particle& p, int i) {
    p.x = W * U(rng); p.y = -H * 0.05; p.vy = H * (0.30 + 0.30 * U(rng)); p.vx = W * (U(rng) - 0.5) * 0.12; p.size = H * (0.011 + 0.012 * U(rng));
    p.phase = U(rng) * 6.28; p.rot = U(rng) * 6.28; p.delay = U(rng) * 1.6; p.life = 3.0; p.kind = 2; p.color = palette[i % 6]; });
  else if (a.name == "fireworks") add(7, [&](Particle& p, int i) {
    p.x = W * (0.15 + 0.7 * U(rng)); p.y = H * (0.18 + 0.35 * U(rng)); p.delay = i * 0.38 + U(rng) * 0.15; p.size = H * (0.16 + 0.14 * U(rng));
    p.phase = U(rng) * 6.28; p.life = 1.6; p.kind = 3; p.color = palette[i % 6]; });
  else if (a.name == "rain") add(260, [&](Particle& p, int) {
    p.x = W * U(rng); p.y = -H * U(rng); p.vy = H * (1.3 + 0.9 * U(rng)); p.size = H * (0.03 + 0.05 * U(rng)); p.delay = U(rng) * 0.4; p.life = 3.4; p.kind = 4; });
  else if (a.name == "lasers") add(10, [&](Particle& p, int i) {
    p.x = i < 5 ? 0 : W; p.y = H; p.phase = U(rng) * 6.28; p.vx = 0.7 + U(rng) * 1.0; p.life = 3.4; p.kind = 5; p.color = palette[i % 5]; });
}

void Reactions::blit(cv::Mat& frame, const cv::Mat& sprite, double cx, double cy, double size, double alpha, double rot) {
  if (sprite.empty() || size < 2 || alpha <= 0.01) return;  // empty: asset missing
  double s = size / std::max(sprite.cols, sprite.rows);
  cv::Mat sp;
  cv::Size dst((int)std::max(1.0, sprite.cols * s), (int)std::max(1.0, sprite.rows * s));
  cv::resize(sprite, sp, dst, 0, 0, s < 1 ? cv::INTER_AREA : cv::INTER_LINEAR);
  if (std::abs(rot) > 1e-3) {
    cv::Mat R = cv::getRotationMatrix2D(cv::Point2f(sp.cols / 2.f, sp.rows / 2.f), rot * 180 / CV_PI, 1.0);
    cv::warpAffine(sp, sp, R, sp.size(), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0, 0));
  }
  int x0 = (int)(cx - sp.cols / 2.0), y0 = (int)(cy - sp.rows / 2.0);
  cv::Rect r(x0, y0, sp.cols, sp.rows);
  cv::Rect fr = r & cv::Rect(0, 0, frame.cols, frame.rows);
  if (fr.empty()) return;
  cv::Mat roi = frame(fr);
  cv::Mat sroi = sp(cv::Rect(fr.x - r.x, fr.y - r.y, fr.width, fr.height));
  for (int y = 0; y < fr.height; y++) {
    uchar* d = roi.ptr<uchar>(y);
    const uchar* sr = sroi.ptr<uchar>(y);
    for (int x = 0; x < fr.width; x++) {
      float a = sr[x * 4 + 3] / 255.f * (float)alpha;
      if (a <= 0.003f) continue;
      for (int c = 0; c < 3; c++) d[x * 3 + c] = (uchar)(d[x * 3 + c] * (1 - a) + sr[x * 4 + c] * a);
    }
  }
}

void Reactions::render(cv::Mat& frame, double now) {
  const double DUR = 3.6;
  const double W = frame.cols, H = frame.rows;
  // Procedural effects draw into an overlay + alpha layer, composited once so
  // shapes get real transparency (and a cheap glow where it helps).
  bool usedOverlay = false;
  bool glow = false;
  auto ensureOverlay = [&]() {
    if (usedOverlay) return;
    overlay_.create(frame.size(), CV_8UC3); overlay_.setTo(cv::Scalar(0, 0, 0));
    overlayA_.create(frame.size(), CV_32F); overlayA_.setTo(0.0f);
    usedOverlay = true;
  };
  auto poly = [&](const cv::Point* pts, int n, cv::Scalar col, double a) {
    ensureOverlay();
    cv::fillConvexPoly(overlay_, pts, n, col, cv::LINE_AA);
    cv::fillConvexPoly(overlayA_, pts, n, cv::Scalar(a), cv::LINE_AA);
  };
  auto line = [&](cv::Point2f p0, cv::Point2f p1, cv::Scalar col, double a, int th) {
    ensureOverlay();
    cv::line(overlay_, p0, p1, col, th, cv::LINE_AA);
    cv::line(overlayA_, p0, p1, cv::Scalar(a), th, cv::LINE_AA);
  };
  auto dot = [&](cv::Point2f c, int r, cv::Scalar col, double a) {
    ensureOverlay();
    cv::circle(overlay_, c, r, col, -1, cv::LINE_AA);
    cv::circle(overlayA_, c, r, cv::Scalar(a), -1, cv::LINE_AA);
  };

  drainPending();
  for (auto& a : anims_) {
    if (a.start < 0) { a.start = now; spawn(a, frame.size()); }
    double t = now - a.start;
    double fadeAll = t > DUR - 0.5 ? std::max(0.0, (DUR - t) / 0.5) : 1.0;
    for (auto& p : a.parts) {
      double lt = t - p.delay;  // particle-local time
      if (lt < 0) continue;
      double env = envelope(lt, p.life) * fadeAll;
      if (env <= 0) continue;
      switch (p.kind) {
        case 0: {  // rising sprite with sway and a gentle pulse
          const char* key = a.name == "balloons" ? "balloon" : (a.name == "thumbsup" ? "thumbsup" : (a.name == "thumbsdown" ? "thumbsdown" : "heart"));
          double x = p.x + std::sin(lt * 2.2 + p.phase) * W * 0.025;
          double y = p.y + p.vy * lt;
          double pulse = a.name == "hearts" ? 1 + 0.12 * std::sin(lt * 7 + p.phase) : 1;
          blit(frame, sprites_[key], x, y, p.size * pulse * (0.7 + 0.3 * smooth(lt / 0.3)), env, std::sin(lt * 1.5 + p.phase) * 0.12);
          break;
        }
        case 1: {  // hero sprite: overshoot pop, hold with a bob, drift up and away
          double k = std::min(1.0, lt / 0.45);
          double pop = easeOutBack(k);
          double y = p.y - std::max(0.0, lt - 1.8) * H * 0.12 + std::sin(lt * 3.2) * H * 0.008;
          blit(frame, sprites_[a.name], p.x, y, p.size * (0.3 + 0.7 * pop), env, std::sin(lt * 2.5) * 0.05);
          break;
        }
        case 2: {  // confetti: tumbling paper
          double x = p.x + p.vx * lt + std::sin(lt * 3.5 + p.phase) * W * 0.012;
          double y = p.y + p.vy * lt;
          if (y > H + p.size) break;
          double ang = p.rot + lt * 5;
          double flip = std::cos(lt * 9 + p.phase);  // 3D tumble: squash one axis
          cv::Point2f c((float)x, (float)y);
          cv::Point2f dx((float)(std::cos(ang) * p.size), (float)(std::sin(ang) * p.size));
          cv::Point2f dy((float)(-std::sin(ang) * p.size * 0.55 * flip), (float)(std::cos(ang) * p.size * 0.55 * flip));
          cv::Point pts[4] = { c - dx - dy, c + dx - dy, c + dx + dy, c - dx + dy };
          poly(pts, 4, p.color, 0.95 * env);
          break;
        }
        case 3: {  // firework: rising trail then a burst with gravity
          glow = true;
          const double up = 0.35;
          if (lt < up) {  // trail
            double k = lt / up;
            cv::Point2f from((float)p.x, (float)H), to((float)p.x, (float)(H - (H - p.y) * smooth(k)));
            line(cv::Point2f(to.x, to.y + (float)(H * 0.06)), to, cv::Scalar(255, 255, 255), 0.8 * env, std::max(1, (int)(H * 0.003)));
            dot(to, std::max(2, (int)(H * 0.005)), cv::Scalar(255, 255, 255), env);
            break;
          }
          double bt = lt - up;
          double r = p.size * (1 - std::pow(1 - std::min(1.0, bt / 1.0), 2.2));
          double g = bt * bt * H * 0.18;  // gravity sag
          for (int i = 0; i < 24; i++) {
            double ang = i * 2 * CV_PI / 24 + p.phase;
            double rr = r * (0.85 + 0.15 * std::sin(i * 2.3 + p.phase));
            cv::Point2f end((float)(p.x + std::cos(ang) * rr), (float)(p.y + std::sin(ang) * rr + g));
            cv::Point2f mid((float)(p.x + std::cos(ang) * rr * 0.6), (float)(p.y + std::sin(ang) * rr * 0.6 + g * 0.6));
            line(mid, end, p.color, 0.9 * env, std::max(1, (int)(H * 0.004)));
            dot(end, std::max(2, (int)(H * 0.006)), cv::Scalar(255, 255, 255), env);
          }
          break;
        }
        case 4: {  // rain streaks
          double y = std::fmod(p.y + p.vy * lt, H + p.size * 2) - p.size;
          line(cv::Point2f((float)p.x, (float)y), cv::Point2f((float)(p.x - W * 0.004), (float)(y + p.size)),
               cv::Scalar(255, 235, 200), 0.55 * env, std::max(1, (int)(H * 0.0025)));
          break;
        }
        case 5: {  // lasers sweeping from the bottom corners
          glow = true;
          double base = p.x < W / 2 ? -CV_PI / 2 + 0.35 : -CV_PI / 2 - 0.35;
          double ang = base + std::sin(lt * p.vx * 1.8 + p.phase) * 0.6;
          cv::Point2f end((float)(p.x + std::cos(ang) * W * 1.6), (float)(p.y + std::sin(ang) * W * 1.6));
          line(cv::Point2f((float)p.x, (float)p.y), end, p.color, 0.75 * env, std::max(2, (int)(H * 0.005)));
          break;
        }
      }
    }
    if (a.name == "rain") {  // cool, darker cast while it rains
      double k = 0.35 * fadeAll * smooth(t / 0.4);
      cv::Mat tint(frame.size(), CV_8UC3, cv::Scalar(90, 60, 30));
      cv::addWeighted(frame, 1 - k, tint, k, 0, frame);
    }
  }
  if (usedOverlay) {
    if (glow) {  // soft halo: blurred copy added under the crisp layer
      cv::Mat gA, gC;
      int k = std::max(3, (int)(H * 0.02) | 1);
      cv::GaussianBlur(overlayA_, gA, cv::Size(k, k), 0);
      cv::GaussianBlur(overlay_, gC, cv::Size(k, k), 0);
      cv::Mat inv = 1.0f - gA * 0.7f;
      cv::Mat tmp;
      cv::blendLinear(frame, gC, inv, gA * 0.7f, tmp);
      frame = tmp;
    }
    cv::Mat inv = 1.0f - overlayA_;
    cv::Mat out;
    cv::blendLinear(frame, overlay_, inv, overlayA_, out);
    frame = out;
  }
  anims_.erase(std::remove_if(anims_.begin(), anims_.end(), [&](const Anim& a) { return a.start >= 0 && now - a.start > DUR; }), anims_.end());
}

// ---------------------------------------------------------------------------
// EffectPipeline

bool EffectPipeline::init(const std::string& modelsDir, const std::string& assetsDir, std::string* err) {
  std::string e;
  std::string status;
  if (!segmenter_.load(modelsDir + "/pphumanseg.onnx", &e)) status += "segmenter: " + e + "; ";
  if (!faces_.load(modelsDir + "/yunet.onnx", &e)) status += "faces: " + e + "; ";
  if (!gestures_.load(modelsDir, &e)) status += "gestures: " + e + "; ";
  if (!reactions_.loadAssets(assetsDir, &e)) status += "assets: " + e + "; ";
  modelStatus_ = status.empty() ? "ok" : status;
  if (err) *err = status;
  return segmenter_.loaded();  // the rest degrade gracefully
}

void EffectPipeline::setSettings(const Settings& s) {
  if (s.centerStage != settings_.centerStage) framer_.reset();
  settings_ = s;
}

void EffectPipeline::ensureBackground(const cv::Size& sz) {
  if (settings_.background == "image") {
    if (bgImagePath_ != settings_.backgroundImage || bgImageSize_ != sz || bgImage_.empty()) {
      cv::Mat img = cv::imread(settings_.backgroundImage, cv::IMREAD_COLOR);
      if (img.empty()) { img = cv::Mat(sz, CV_8UC3, cv::Scalar(30, 30, 30)); }
      // cover-fit
      double s = std::max((double)sz.width / img.cols, (double)sz.height / img.rows);
      cv::Mat scaled;
      cv::resize(img, scaled, cv::Size(std::max(sz.width, (int)(img.cols * s)), std::max(sz.height, (int)(img.rows * s))), 0, 0, cv::INTER_AREA);
      cv::Rect r((scaled.cols - sz.width) / 2, (scaled.rows - sz.height) / 2, sz.width, sz.height);
      bgImage_ = scaled(r).clone();
      bgImagePath_ = settings_.backgroundImage;
      bgImageSize_ = sz;
    }
  } else if (settings_.background == "color") {
    unsigned rgb = 0x1e1e2e;
    const std::string& c = settings_.backgroundColor;
    parseHexColor(c, rgb);  // validated when the setting is read; default if not
    cv::Scalar col((rgb) & 0xff, (rgb >> 8) & 0xff, (rgb >> 16) & 0xff);
    if (bgImage_.empty() || bgImageSize_ != sz || bgImagePath_ != c) {
      bgImage_ = cv::Mat(sz, CV_8UC3, col);
      bgImagePath_ = c;
      bgImageSize_ = sz;
    }
  }
}

void EffectPipeline::computeMask(const cv::Mat& work, cv::Mat& maskF) {
  cv::Mat prob;
  if (!segmenter_.infer(work, prob)) { maskF = cv::Mat(work.size(), CV_32F, cv::Scalar(1)); return; }
  // Temporal smoothing at model resolution.
  if (maskSmall_.empty() || maskSmall_.size() != prob.size()) maskSmall_ = prob.clone();
  else cv::addWeighted(prob, 0.55, maskSmall_, 0.45, 0, maskSmall_);
  // Push uncertain probabilities towards 0/1 (smoothstep between 0.25 and
  // 0.75) so clothing with mid confidence doesn't turn milky, then upsample
  // and feather the edge.
  cv::Mat m = (maskSmall_ - 0.25f) * (1.0f / 0.5f);
  cv::min(cv::max(m, 0.0f), 1.0f, m);
  m = m.mul(m).mul(3.0f - 2.0f * m);
  cv::resize(m, maskF, work.size(), 0, 0, cv::INTER_LINEAR);
  int k = std::max(3, (work.rows / 90) | 1);
  cv::GaussianBlur(maskF, maskF, cv::Size(k, k), 0);
}

void EffectPipeline::applyBackground(cv::Mat& work, const cv::Mat& maskF) {
  cv::Mat bg;
  if (settings_.background == "image" || settings_.background == "color") {
    ensureBackground(work.size());
    bg = bgImage_;
  } else if (settings_.portrait) {
    // Masked blur at quarter resolution: the foreground is excluded from the
    // blur so its colours don't halo into the background.
    cv::Mat small, msmall, inv;
    cv::resize(work, small, cv::Size(), 0.25, 0.25, cv::INTER_AREA);
    cv::resize(maskF, msmall, small.size(), 0, 0, cv::INTER_AREA);
    inv = 1.0f - msmall;
    cv::Mat smallF;
    small.convertTo(smallF, CV_32FC3);
    cv::Mat inv3;
    cv::merge(std::vector<cv::Mat>{ inv, inv, inv }, inv3);
    cv::Mat num = smallF.mul(inv3), den;
    int k = std::max(3, (int)(small.rows * (0.04 + 0.12 * settings_.portraitIntensity)) | 1);
    cv::GaussianBlur(num, num, cv::Size(k, k), 0);
    cv::GaussianBlur(inv, den, cv::Size(k, k), 0);
    den = cv::max(den, 0.02f);
    cv::Mat den3;
    cv::merge(std::vector<cv::Mat>{ den, den, den }, den3);
    cv::Mat blurred = num / den3;
    // second pass for a creamier bokeh at high intensity
    if (settings_.portraitIntensity > 0.5f) cv::GaussianBlur(blurred, blurred, cv::Size(k, k), 0);
    blurred.convertTo(blurred, CV_8UC3);
    cv::resize(blurred, bg, work.size(), 0, 0, cv::INTER_LINEAR);
  } else {
    return;
  }
  cv::Mat inv = 1.0f - maskF;
  cv::Mat out;
  cv::blendLinear(work, bg, maskF, inv, out);
  work = out;
}

void EffectPipeline::applyStudioLight(cv::Mat& work, const cv::Mat& maskF) {
  float I = settings_.studioLightIntensity;
  // Foreground: lift the midtones with a soft gamma and a touch of warmth
  // (key light); background: darken with a vignette so the subject pops.
  static cv::Mat lutB, lutG, lutR;
  static float lutI = -1;
  if (lutI != I) {
    lutB.create(1, 256, CV_8UC1); lutG.create(1, 256, CV_8UC1); lutR.create(1, 256, CV_8UC1);
    float g = 1.0f - 0.32f * I;
    for (int i = 0; i < 256; i++) {
      double v = 255.0 * std::pow(i / 255.0, g);
      lutB.at<uchar>(i) = (uchar)std::min(255.0, v * (1.0 - 0.04 * I));
      lutG.at<uchar>(i) = (uchar)std::min(255.0, v);
      lutR.at<uchar>(i) = (uchar)std::min(255.0, v * (1.0 + 0.05 * I));
    }
    lutI = I;
  }
  static cv::Mat vignette;  // CV_32F, 1 in the middle, ~0.45 in the corners
  if (vignette.empty() || vignette.size() != work.size()) {
    vignette.create(work.size(), CV_32F);
    float cx = work.cols / 2.f, cy = work.rows / 2.f, rmax = std::sqrt(cx * cx + cy * cy);
    for (int y = 0; y < work.rows; y++) {
      float* row = vignette.ptr<float>(y);
      for (int x = 0; x < work.cols; x++) {
        float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) / rmax;
        row[x] = 1.0f - 0.55f * d * d;
      }
    }
  }
  std::vector<cv::Mat> ch(3);
  cv::split(work, ch);
  cv::LUT(ch[0], lutB, ch[0]); cv::LUT(ch[1], lutG, ch[1]); cv::LUT(ch[2], lutR, ch[2]);
  cv::Mat lit;
  cv::merge(ch, lit);
  // Background darkening = flat dim blended with the vignette by intensity.
  cv::Mat dimF;
  work.convertTo(dimF, CV_32FC3, 1.0 - 0.5 * I);
  cv::Mat vig3;
  cv::Mat v = 1.0f - (1.0f - vignette) * I;
  cv::merge(std::vector<cv::Mat>{ v, v, v }, vig3);
  dimF = dimF.mul(vig3);
  cv::Mat dark;
  dimF.convertTo(dark, CV_8UC3);
  cv::Mat inv = 1.0f - maskF;
  cv::Mat out;
  cv::blendLinear(lit, dark, maskF, inv, out);
  work = out;
}

void EffectPipeline::process(const cv::Mat& src, cv::Mat& out, const cv::Size& outSize, double now) {
  auto tick = [&]() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
  double tl = profile_ ? tick() : 0;
  auto stage = [&](int i) { if (!profile_) return; double t = tick(); profAcc_[i] += t - tl; tl = t; };
  double dt = lastTime_ > 0 ? std::clamp(now - lastTime_, 0.001, 0.2) : 1.0 / 30;
  lastTime_ = now;
  frameIdx_++;
  double outAspect = (double)outSize.width / outSize.height;

  bool needFaces = settings_.centerStage || settings_.reactions;
  bool facesFresh = false;
  if (needFaces && faces_.loaded() && frameIdx_ % 3 == 0) {
    lastFaces_ = faces_.detect(src);
    facesFresh = true;
  }

  stage(0);  // faces
  cv::Rect crop = settings_.centerStage ? framer_.update(src.size(), lastFaces_, facesFresh, outAspect, dt)
                                        : framer_.fullCrop(src.size(), outAspect);
  cv::Mat work;
  cv::resize(src(crop), work, outSize, 0, 0, crop.width > outSize.width ? cv::INTER_AREA : cv::INTER_LINEAR);
  stage(1);  // crop/resize

  bool needMask = settings_.portrait || settings_.studioLight || settings_.background != "none";
  cv::Mat maskF;
  if (needMask) computeMask(work, maskF);
  stage(2);  // mask
  if (settings_.portrait || settings_.background != "none") applyBackground(work, maskF);
  stage(3);  // background
  if (settings_.studioLight) applyStudioLight(work, maskF);
  stage(4);  // studio light

  // Gesture-triggered reactions (checked at ~10 Hz on the output frame).
  if (settings_.reactions && gestures_.loaded() && frameIdx_ % 3 == 1) {
    auto hands = gestures_.detect(work);
    // Faces are in src coordinates; map into work coordinates for the on-face check.
    std::vector<cv::Rect2f> facesWork;
    double sx = (double)outSize.width / crop.width, sy = (double)outSize.height / crop.height;
    for (auto& f : lastFaces_) facesWork.emplace_back((f.x - crop.x) * sx, (f.y - crop.y) * sy, f.width * sx, f.height * sy);
    std::string g = gestures_.classify(hands, facesWork);
    static const bool dbg = getenv("CAMFX_DEBUG") != nullptr;
    if (dbg) fprintf(stderr, "gesture: hands=%zu faces=%zu -> '%s'\n", hands.size(), facesWork.size(), g.c_str());
    static std::string pending; static int stable = 0;
    if (!g.empty() && g == pending) stable++; else { pending = g; stable = g.empty() ? 0 : 1; }
    gestures_.noteGesture(g);
    // ~0.7 s of a steady gesture, then a cooldown so it does not re-fire while held.
    if (!g.empty() && stable >= 7 && now - lastGestureTime_ > 4.0) {
      reactions_.trigger(g);
      lastGestureTime_ = now;
      stable = 0;
    }
  }
  stage(5);  // gestures
  if (reactions_.active()) reactions_.render(work, now);
  stage(6);  // reactions
  if (settings_.mirror) cv::flip(work, work, 1);
  out = work;
  if (profile_ && ++profN_ == 60) {
    char b[256];
    snprintf(b, sizeof b, "ms/frame: faces %.1f resize %.1f mask %.1f bg %.1f light %.1f gestures %.1f react %.1f",
             profAcc_[0] / profN_, profAcc_[1] / profN_, profAcc_[2] / profN_, profAcc_[3] / profN_, profAcc_[4] / profN_, profAcc_[5] / profN_, profAcc_[6] / profN_);
    profileLine_ = b;
    for (auto& a : profAcc_) a = 0;
    profN_ = 0;
  }
}
