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

const std::vector<std::string>& Settings::filterNames() {
  static const std::vector<std::string> n = { "none", "mono", "sepia", "warm", "cool", "vivid", "soft", "sharpen", "vintage" };
  return n;
}
const std::vector<std::string>& Settings::funNames() {
  static const std::vector<std::string> n = { "none", "sunglasses", "glasses", "tophat", "crown", "cat", "halo", "headphones", "flowers" };
  return n;
}

const std::vector<std::string>& Framing::fitNames() {
  static const std::vector<std::string> n = { "cover", "contain", "stretch" };
  return n;
}

// The source is scaled by the fit's factor(s) times the zoom and placed on the
// output plane, centred, then shifted by pan * (the overhang on that axis);
// what falls inside the output is shown. Cover and stretch always fill the
// output; contain fills it once the zoom eats the bars.
FrameGeom frameGeometry(const cv::Size& src, const cv::Size& out, const Framing& f) {
  FrameGeom g;
  const double sw = std::max(1, src.width), sh = std::max(1, src.height), ow = std::max(1, out.width), oh = std::max(1, out.height);
  const double z = std::clamp((double)f.zoom, 1.0, 4.0);
  double kx, ky;
  if (f.fit == "stretch") { kx = ow / sw; ky = oh / sh; }
  else kx = ky = f.fit == "contain" ? std::min(ow / sw, oh / sh) : std::max(ow / sw, oh / sh);
  kx *= z; ky *= z;
  const double W = sw * kx, H = sh * ky;                                     // the placed source
  const double fx = std::max(0.0, (W - ow) / 2), fy = std::max(0.0, (H - oh) / 2);  // room to pan
  const double x0 = (ow - W) / 2 - std::clamp((double)f.panX, -1.0, 1.0) * fx;
  const double y0 = (oh - H) / 2 - std::clamp((double)f.panY, -1.0, 1.0) * fy;
  // Visible part of the placement, and the source behind it (each rounded on
  // its own from the exact bounds, so a whole source stays whole).
  const double dx0 = std::max(0.0, x0), dy0 = std::max(0.0, y0), dx1 = std::min(ow, x0 + W), dy1 = std::min(oh, y0 + H);
  auto rect = [](double a0, double b0, double a1, double b1, const cv::Size& bound) {
    int ia0 = (int)std::lround(a0), ib0 = (int)std::lround(b0);
    cv::Rect r(ia0, ib0, std::max(1, (int)std::lround(a1) - ia0), std::max(1, (int)std::lround(b1) - ib0));
    r &= cv::Rect(0, 0, bound.width, bound.height);
    return r.empty() ? cv::Rect(0, 0, bound.width, bound.height) : r;
  };
  g.dst = rect(dx0, dy0, dx1, dy1, out);
  g.crop = rect((dx0 - x0) / kx, (dy0 - y0) / ky, (dx1 - x0) / kx, (dy1 - y0) / ky, src);
  g.range = cv::Point2f((float)(fx / ow), (float)(fy / oh));
  return g;
}

FrameGeom fitFrame(const cv::Mat& img, cv::Mat& dst, const cv::Size& out, const Framing& f) {
  FrameGeom g = frameGeometry(img.size(), out, f);
  if (g.passthrough(img.size(), out)) { img.copyTo(dst); return g; }
  dst.create(out, img.type());  // reused when it already has the size
  if (g.dst != cv::Rect(0, 0, out.width, out.height)) dst.setTo(cv::Scalar::all(0));
  cv::Mat roi = dst(g.dst);
  double s = (double)g.dst.width / g.crop.width;
  cv::resize(img(g.crop), roi, g.dst.size(), 0, 0, s < 1 ? cv::INTER_AREA : cv::INTER_LINEAR);
  return g;
}

void coverFit(const cv::Mat& img, cv::Mat& dst, const cv::Size& sz) { fitFrame(img, dst, sz, Framing()); }

void alphaBlit(cv::Mat& frame, const cv::Mat& sprite, double cx, double cy, double scale, double alpha, double rot) {
  if (sprite.empty() || sprite.type() != CV_8UC4 || frame.type() != CV_8UC3 || !(scale > 0) || alpha <= 0.01) return;  // empty: asset missing
  cv::Size dst((int)std::max(1.0, sprite.cols * scale), (int)std::max(1.0, sprite.rows * scale));
  if (dst.width < 2 && dst.height < 2) return;
  cv::Mat sp;
  cv::resize(sprite, sp, dst, 0, 0, scale < 1 ? cv::INTER_AREA : cv::INTER_LINEAR);
  if (std::abs(rot) > 1e-3) {
    // Rotate into a box large enough for the whole rotated sprite (a wide,
    // flat sprite would otherwise lose its corners).
    double c = std::abs(std::cos(rot)), sn = std::abs(std::sin(rot));
    cv::Size rs((int)std::ceil(sp.cols * c + sp.rows * sn), (int)std::ceil(sp.cols * sn + sp.rows * c));
    cv::Mat R = cv::getRotationMatrix2D(cv::Point2f(sp.cols / 2.f, sp.rows / 2.f), rot * 180 / CV_PI, 1.0);
    R.at<double>(0, 2) += (rs.width - sp.cols) / 2.0;
    R.at<double>(1, 2) += (rs.height - sp.rows) / 2.0;
    cv::Mat rotated;
    cv::warpAffine(sp, rotated, R, rs, cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0, 0));
    sp = rotated;
  }
  int x0 = (int)std::lround(cx - sp.cols / 2.0), y0 = (int)std::lround(cy - sp.rows / 2.0);
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

// ---------------------------------------------------------------------------
// Framer (Center Stage)

void Framer::reset() { init_ = false; lastFaceTime_ = -1e9; }

cv::Rect Framer::fullCrop(const cv::Size& src, double outAspect) const {
  double w = src.width, h = src.height;
  if (w / h > outAspect) w = h * outAspect; else h = w / outAspect;
  return cv::Rect((int)((src.width - w) / 2), (int)((src.height - h) / 2), (int)w, (int)h);
}

cv::Rect Framer::update(const cv::Size& src, const std::vector<cv::Rect2f>& faces, bool facesFresh, const cv::Size& out, double dt, double minZoom, double intensity) {
  const double I = std::clamp(intensity, 0.0, 1.0);
  now_ += dt;
  const double outAspect = (double)out.width / std::max(1, out.height);
  cv::Rect full = fullCrop(src, outAspect);
  // How far in we may go: 3x with a source at least 1.5x the output (the crop
  // still has output resolution at 2x), 1.8x with less headroom, and 1.6x when
  // the source is no bigger than the output (an upscale, but that is what a
  // 720p Center Stage is: framing beats sharpness at desk distance).
  const double ratio = (double)full.height / std::max(1, out.height);
  const double hwZoom = ratio >= 1.5 ? 3.0 : (ratio > 1.0 ? 1.8 : 1.6);
  const double maxZoom = 1.0 + (hwZoom - 1.0) * (0.4 + 0.6 * I);   // intensity scales how far in we go
  minZoom = std::clamp(minZoom, 1.0, 4.0);
  const double hRest = full.height / minZoom;                        // "nobody in view": the user's own zoom, centred
  const double hMin = std::min(hRest, full.height / maxZoom);
  if (!init_) {
    cx_ = tcx_ = full.x + full.width / 2.0; cy_ = tcy_ = full.y + full.height / 2.0; h_ = th_ = hRest;
    init_ = true;
  }
  if (facesFresh) {
    if (!faces.empty()) {
      lastFaceTime_ = now_;
      // Union of faces, framed head + shoulders: 2.4 face heights tall for one
      // face (YuNet's box runs forehead to chin), with 0.35 of a face height of
      // headroom above the top face.
      float x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9, maxFaceH = 0;
      for (auto& f : faces) {
        x0 = std::min(x0, f.x); y0 = std::min(y0, f.y); x1 = std::max(x1, f.x + f.width); y1 = std::max(y1, f.y + f.height);
        maxFaceH = std::max(maxFaceH, f.height);
      }
      // Tightness: 3.6 face heights (loose) at intensity 0 down to 1.8 (tight) at 1.
      const double heights = 3.6 - 1.8 * I;
      double nh = std::max((double)(y1 - y0) + (heights - 1.0) * maxFaceH, ((x1 - x0) + (heights - 0.4) * maxFaceH) / outAspect);
      double ncx = (x0 + x1) / 2.0;
      double ncy = y0 - 0.35 * maxFaceH + nh / 2;  // centre of the ideal frame: kept when the zoom limits make the crop larger
      nh = std::clamp(nh, hMin, hRest);
      // Deadband so tiny detector jitter doesn't move the frame.
      const double dead = 0.055 - 0.03 * I;
      if (std::abs(ncx - tcx_) > dead * h_ || std::abs(ncy - tcy_) > dead * h_ || std::abs(nh - th_) > 1.7 * dead * h_) {
        tcx_ = ncx; tcy_ = ncy; th_ = nh;
      }
    } else if (now_ - lastFaceTime_ > 1.5) {
      tcx_ = full.x + full.width / 2.0; tcy_ = full.y + full.height / 2.0; th_ = hRest;
    }
  }
  th_ = std::clamp(th_, hMin, hRest);  // the user's zoom changed under us
  // Ease: pan fairly quick, zoom-in slower than zoom-out.
  double aPan = 1 - std::exp(-dt / (0.65 - 0.4 * I));                          // 0.65 s (calm) .. 0.25 s (snappy)
  double aZoom = 1 - std::exp(-dt / (th_ > h_ ? (0.8 - 0.4 * I) : (1.5 - 0.9 * I)));
  cx_ += (tcx_ - cx_) * aPan; cy_ += (tcy_ - cy_) * aPan; h_ += (th_ - h_) * aZoom;
  h_ = std::min(h_, (double)full.height);
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

bool Reactions::active() const {
  if (playing_.load()) return true;
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
  playing_ = !anims_.empty();
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
  if (sprite.empty() || size < 2) return;
  alphaBlit(frame, sprite, cx, cy, size / std::max(sprite.cols, sprite.rows), alpha, rot);
}

namespace {
// 8-bit alpha blend of two 3-channel images with a 3-channel mask (all the
// same size and continuous): out = (a*m + b*(255-m)) / 255. Plain elementwise
// loop so the compiler vectorises it; parallel_for_ splits it over OpenCV's
// (two) worker threads. This replaces cv::blendLinear + the F32 mask copies.
void blend8(const cv::Mat& a, const cv::Mat& b, const cv::Mat& m3, cv::Mat& out) {
  CV_Assert(a.size() == b.size() && a.size() == m3.size() && a.type() == CV_8UC3 && b.type() == CV_8UC3 && m3.type() == CV_8UC3);
  out.create(a.size(), CV_8UC3);
  const int n = a.cols * 3;
  cv::parallel_for_(cv::Range(0, a.rows), [&](const cv::Range& r) {
    for (int y = r.start; y < r.end; y++) {
      const uchar* pa = a.ptr<uchar>(y);
      const uchar* pb = b.ptr<uchar>(y);
      const uchar* pm = m3.ptr<uchar>(y);
      uchar* pd = out.ptr<uchar>(y);
      for (int i = 0; i < n; i++) {
        unsigned v = pa[i] * pm[i] + pb[i] * (255 - pm[i]) + 128;
        pd[i] = (uchar)((v + (v >> 8)) >> 8);  // exact v/255 for v < 65536
      }
    }
  });
}
// Same, but with a single-channel mask, over a ROI (used by the reactions overlay).
void blend8Roi(cv::Mat& frame, const cv::Mat& layer, const cv::Mat& alpha, const cv::Rect& roi) {
  cv::parallel_for_(cv::Range(roi.y, roi.y + roi.height), [&](const cv::Range& r) {
    for (int y = r.start; y < r.end; y++) {
      uchar* pd = frame.ptr<uchar>(y) + roi.x * 3;
      const uchar* pl = layer.ptr<uchar>(y) + roi.x * 3;
      const uchar* pm = alpha.ptr<uchar>(y) + roi.x;
      for (int x = 0; x < roi.width; x++) {
        unsigned m = pm[x];
        if (!m) continue;
        unsigned im = 255 - m;
        for (int c = 0; c < 3; c++) {
          unsigned v = pl[x * 3 + c] * m + pd[x * 3 + c] * im + 128;
          pd[x * 3 + c] = (uchar)((v + (v >> 8)) >> 8);
        }
      }
    }
  });
}
}  // namespace

// ---------------------------------------------------------------------------
// OverlayLayer

void OverlayLayer::begin(const cv::Size& sz) { size_ = sz; used_ = false; bbox_ = cv::Rect(); }

void OverlayLayer::ensure() {
  if (used_) return;
  if (overlay_.size() != size_) {
    overlay_.create(size_, CV_8UC3); overlay_.setTo(cv::Scalar(0, 0, 0));
    overlayA_.create(size_, CV_8UC1); overlayA_.setTo(0);
  } else if (!dirty_.empty()) {  // clear only what the previous frame drew
    overlay_(dirty_).setTo(cv::Scalar(0, 0, 0));
    overlayA_(dirty_).setTo(0);
  }
  dirty_ = cv::Rect();
  used_ = true;
}

void OverlayLayer::grow(cv::Rect r) { r &= cv::Rect(0, 0, size_.width, size_.height); bbox_ = bbox_.empty() ? r : (bbox_ | r); }

void OverlayLayer::poly(const cv::Point* pts, int n, cv::Scalar col, double a) {
  ensure();
  cv::fillConvexPoly(overlay_, pts, n, col, cv::LINE_AA);
  cv::fillConvexPoly(overlayA_, pts, n, cv::Scalar(a * 255), cv::LINE_AA);
  cv::Rect r = cv::boundingRect(std::vector<cv::Point>(pts, pts + n));
  grow(cv::Rect(r.x - 2, r.y - 2, r.width + 4, r.height + 4));
}

void OverlayLayer::line(cv::Point2f p0, cv::Point2f p1, cv::Scalar col, double a, int th) {
  ensure();
  cv::line(overlay_, p0, p1, col, th, cv::LINE_AA);
  cv::line(overlayA_, p0, p1, cv::Scalar(a * 255), th, cv::LINE_AA);
  int x0 = (int)std::floor(std::min(p0.x, p1.x)) - th - 2, y0 = (int)std::floor(std::min(p0.y, p1.y)) - th - 2;
  int x1 = (int)std::ceil(std::max(p0.x, p1.x)) + th + 2, y1 = (int)std::ceil(std::max(p0.y, p1.y)) + th + 2;
  grow(cv::Rect(x0, y0, x1 - x0, y1 - y0));
}

void OverlayLayer::dot(cv::Point2f c, int r, cv::Scalar col, double a) {
  ensure();
  cv::circle(overlay_, c, r, col, -1, cv::LINE_AA);
  cv::circle(overlayA_, c, r, cv::Scalar(a * 255), -1, cv::LINE_AA);
  grow(cv::Rect((int)c.x - r - 2, (int)c.y - r - 2, 2 * r + 5, 2 * r + 5));
}

void OverlayLayer::ellipse(cv::Point2f c, cv::Size2f axes, double angleDeg, cv::Scalar col, double a, int th) {
  ensure();
  cv::ellipse(overlay_, c, cv::Size((int)axes.width, (int)axes.height), angleDeg, 0, 360, col, th, cv::LINE_AA);
  cv::ellipse(overlayA_, c, cv::Size((int)axes.width, (int)axes.height), angleDeg, 0, 360, cv::Scalar(a * 255), th, cv::LINE_AA);
  int r = (int)std::ceil(std::max(axes.width, axes.height)) + std::max(th, 0) + 2;
  grow(cv::Rect((int)c.x - r, (int)c.y - r, 2 * r + 1, 2 * r + 1));
}

void OverlayLayer::composite(cv::Mat& frame, bool glow) {
  if (!used_ || bbox_.empty() || frame.size() != size_) return;
  const cv::Rect frameRect(0, 0, size_.width, size_.height);
  cv::Rect bbox = bbox_;
  if (glow) {  // soft halo: blurred copy (at half resolution) added under the crisp layer
    int k = std::max(3, (int)(size_.height * 0.02) | 1);
    cv::Rect gr(bbox.x - k, bbox.y - k, bbox.width + 2 * k, bbox.height + 2 * k);
    gr &= frameRect;
    cv::Size half((gr.width + 1) / 2, (gr.height + 1) / 2);
    cv::resize(overlay_(gr), glowC_, half, 0, 0, cv::INTER_LINEAR);
    cv::resize(overlayA_(gr), glowA_, half, 0, 0, cv::INTER_LINEAR);
    int kh = std::max(3, (k / 2) | 1);
    cv::GaussianBlur(glowC_, glowC_, cv::Size(kh, kh), 0);
    cv::GaussianBlur(glowA_, glowA_, cv::Size(kh, kh), 0);
    glowA_.convertTo(glowA_, -1, 0.7);
    cv::Mat gc, ga;  // upsampled halo, blended over the ROI only
    cv::resize(glowC_, gc, gr.size(), 0, 0, cv::INTER_LINEAR);
    cv::resize(glowA_, ga, gr.size(), 0, 0, cv::INTER_LINEAR);
    cv::Mat roi = frame(gr);
    blend8Roi(roi, gc, ga, cv::Rect(0, 0, gr.width, gr.height));
    bbox = gr;
  }
  blend8Roi(frame, overlay_, overlayA_, bbox);
  dirty_ = bbox;
}

void Reactions::render(cv::Mat& frame, double now) {
  const double DUR = 3.6;
  const double W = frame.cols, H = frame.rows;
  // Procedural effects draw into layer_ (overlay + alpha), composited once so
  // shapes get real transparency (and a cheap glow where it helps).
  bool glow = false;
  layer_.begin(frame.size());
  auto poly = [&](const cv::Point* pts, int n, cv::Scalar col, double a) { layer_.poly(pts, n, col, a); };
  auto line = [&](cv::Point2f p0, cv::Point2f p1, cv::Scalar col, double a, int th) { layer_.line(p0, p1, col, a, th); };
  auto dot = [&](cv::Point2f c, int r, cv::Scalar col, double a) { layer_.dot(c, r, col, a); };

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
    if (a.name == "rain") {  // cool, darker cast while it rains: in place, no tint frame
      double k = 0.35 * fadeAll * smooth(t / 0.4);
      frame.convertTo(frame, -1, 1 - k, 0);
      cv::add(frame, cv::Scalar(90, 60, 30) * k, frame);
    }
  }
  layer_.composite(frame, glow);
  anims_.erase(std::remove_if(anims_.begin(), anims_.end(), [&](const Anim& a) { return a.start >= 0 && now - a.start > DUR; }), anims_.end());
  playing_ = !anims_.empty();
}

// ---------------------------------------------------------------------------
// FunOverlay

bool FunOverlay::loadAssets(const std::string& dir, std::string* err) {
  const char* files[] = { "sunglasses", "glasses", "tophat", "crown", "headphones", "flowers" };
  bool ok = true;
  for (auto f : files) {
    cv::Mat m = cv::imread(dir + "/" + f + ".png", cv::IMREAD_UNCHANGED);
    if (m.empty() || m.channels() != 4) { if (err) *err = std::string("missing sprite ") + f; ok = false; continue; }
    // Trim to the glyph's alpha box so the placement sizes refer to the visible shape.
    cv::Mat a;
    cv::extractChannel(m, a, 3);
    cv::Rect r = cv::boundingRect(a > 8);
    sprites_[f] = r.empty() ? m : m(r).clone();
  }
  return ok;
}

namespace {
inline cv::Point2f eyeMid(const cv::Point2f* lm) { return (lm[0] + lm[1]) * 0.5f; }
inline float eyeDist(const cv::Point2f* lm) { return (float)cv::norm(lm[1] - lm[0]); }
}  // namespace

void FunOverlay::update(const std::vector<Face>& faces, double now) {
  bool matched[kMaxFaces] = { false };
  for (size_t i = 0; i < faces.size(); i++) {
    const Face& f = faces[i];
    float d = eyeDist(f.lm);
    if (!(d > 1)) continue;
    cv::Point2f c = eyeMid(f.lm);
    // Nearest live track within a face width; each track takes one face.
    int best = -1;
    double bestDist = 1e9;
    for (size_t t = 0; t < tracks_.size(); t++) {
      if (matched[t]) continue;
      double dist = cv::norm(eyeMid(tracks_[t].lm) - c);
      if (dist < 0.9 * std::max(d, tracks_[t].size) && dist < bestDist) { bestDist = dist; best = (int)t; }
    }
    if (best < 0) {
      if ((int)tracks_.size() < kMaxFaces) { tracks_.push_back(Track{}); best = (int)tracks_.size() - 1; }
      else {  // full: recycle the track unseen for longest (a face that left the frame)
        best = 0;
        for (size_t t = 1; t < tracks_.size(); t++) if (!matched[t] && tracks_[t].lastSeen < tracks_[best].lastSeen) best = (int)t;
        if (matched[best]) continue;
      }
      Track& t = tracks_[best];
      for (int j = 0; j < 5; j++) t.lm[j] = f.lm[j];
      t.size = d;
    } else {  // EMA at the detector's cadence: fast enough to follow, slow enough to hide jitter
      const float a = 0.5f;
      Track& t = tracks_[best];
      for (int j = 0; j < 5; j++) t.lm[j] = t.lm[j] * (1 - a) + f.lm[j] * a;
      t.size = t.size * (1 - a) + d * a;
    }
    tracks_[best].lastSeen = now;
    matched[best] = true;
  }
}

void FunOverlay::render(cv::Mat& frame, const std::string& kind, cv::Point2f origin, cv::Point2f scale, double now) {
  tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(), [&](const Track& t) { return now - t.lastSeen > kHold; }), tracks_.end());
  if (tracks_.empty() || kind == "none") return;
  layer_.begin(frame.size());
  bool glow = false;
  for (const Track& t : tracks_) {
    cv::Point2f lm[5];
    for (int j = 0; j < 5; j++) lm[j] = cv::Point2f((t.lm[j].x - origin.x) * scale.x, (t.lm[j].y - origin.y) * scale.y);
    // Eye line from the image-left eye to the image-right eye: roll in
    // (-90, 90) degrees whichever way the detector labels them.
    cv::Point2f a = lm[0], b = lm[1];
    if (a.x > b.x) std::swap(a, b);
    cv::Point2f ex = b - a;
    float d = (float)cv::norm(ex);
    if (d < 4) continue;
    double roll = std::atan2(ex.y, ex.x);
    cv::Point2f fx = ex * (1.0f / d);                                   // along the eye line (screen right)
    cv::Point2f up(fx.y, -fx.x);                                        // perpendicular, towards the top of the head
    cv::Point2f mid = (a + b) * 0.5f;
    cv::Point2f nose = lm[2];
    // Sprite `name` scaled to `width` px, centred at `c`, rotated with the head.
    auto sprite = [&](const char* name, double width, cv::Point2f c) {
      auto it = sprites_.find(name);
      if (it == sprites_.end()) return;
      alphaBlit(frame, it->second, c.x, c.y, width / it->second.cols, 1.0, -roll);
    };
    // Sprite whose bottom edge sits `above` px above the eye line.
    auto hat = [&](const char* name, double width, double above) {
      auto it = sprites_.find(name);
      if (it == sprites_.end()) return;
      double h = width * it->second.rows / it->second.cols;
      sprite(name, width, mid + up * (float)(above + h / 2));
    };
    if (kind == "sunglasses") sprite("sunglasses", 2.3 * d, mid);
    else if (kind == "glasses") sprite("glasses", 2.3 * d, mid);
    else if (kind == "tophat") hat("tophat", 2.6 * d, 0.9 * d);
    else if (kind == "crown") hat("crown", 2.6 * d, 0.6 * d);
    else if (kind == "flowers") hat("flowers", 2.6 * d, 0.6 * d);
    else if (kind == "headphones") sprite("headphones", 3.4 * d, mid + up * (0.1f * d));  // cups at ear level, band over the head
    else if (kind == "halo") {
      glow = true;
      int th = std::max(2, (int)std::lround(0.06 * d));
      layer_.ellipse(mid + up * (1.6f * d), cv::Size2f(0.9f * d, 0.22f * d), roll * 180 / CV_PI, cv::Scalar(150, 235, 255), 0.92, th);
    } else if (kind == "cat") {
      const cv::Scalar fur(45, 45, 50), pink(175, 130, 255), white(245, 245, 245);
      for (int side = -1; side <= 1; side += 2) {
        cv::Point2f base = mid + up * (1.35f * d) + fx * (side * 0.9f * d);
        cv::Point2f apex = base + up * (0.8f * d) + fx * (side * 0.2f * d);
        cv::Point pts[3] = { base - fx * (0.38f * d), base + fx * (0.38f * d), apex };
        layer_.poly(pts, 3, fur, 0.95);
        cv::Point2f base2 = base + up * (0.08f * d);
        cv::Point2f apex2 = base2 + up * (0.5f * d) + fx * (side * 0.13f * d);
        cv::Point in[3] = { base2 - fx * (0.2f * d), base2 + fx * (0.2f * d), apex2 };
        layer_.poly(in, 3, pink, 0.95);
        // Three whiskers per side, fanning out from beside the nose.
        int th = std::max(1, (int)std::lround(0.03 * d));
        for (int j = -1; j <= 1; j++) {
          cv::Point2f from = nose + fx * (side * 0.28f * d) + up * (j * 0.09f * d - 0.02f * d);
          cv::Point2f to = from + fx * (side * 1.2f * d) + up * (j * 0.24f * d);
          layer_.line(from, to, white, 0.85, th);
        }
      }
      cv::Point np[3] = { nose + up * (0.08f * d) - fx * (0.16f * d), nose + up * (0.08f * d) + fx * (0.16f * d), nose - up * (0.14f * d) };
      layer_.poly(np, 3, pink, 0.95);
    }
  }
  layer_.composite(frame, glow);
}

// ---------------------------------------------------------------------------
// Pyramid

const cv::Mat& Pyramid::level(int i) {
  if (i <= 0) return base_;
  i = std::min(i, 4);
  for (; built_ < i; built_++) cv::pyrDown(built_ == 0 ? base_ : lv_[built_ - 1], lv_[built_]);
  return lv_[i - 1];
}

const cv::Mat& Pyramid::atLeastWide(int minW, int* levelOut) {
  int i = 0;
  while (i < 4 && (base_.cols >> (i + 1)) >= minW) i++;
  if (levelOut) *levelOut = i;
  return level(i);
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
  if (!fun_.loadAssets(assetsDir, &e)) status += "fun assets: " + e + "; ";
  modelStatus_ = status.empty() ? "ok" : status;
  debugGestures_ = getenv("CAMERA_EFFECTS_DEBUG") != nullptr;
  if (err) *err = status;
  return segmenter_.loaded();  // the rest degrade gracefully
}

void EffectPipeline::setSettings(const Settings& s) {
  if (s.centerStage != settings_.centerStage || s.rotate != settings_.rotate) framer_.reset();
  if (s.fun != settings_.fun || s.rotate != settings_.rotate) fun_.reset();  // face tracks are in (rotated) source coordinates
  settings_ = s;
}

void EffectPipeline::ensureBackground(const cv::Size& sz) {
  if (settings_.background == "image") {
    if (bgImagePath_ != settings_.backgroundImage || bgImageSize_ != sz || bgImage_.empty()) {
      cv::Mat img = cv::imread(settings_.backgroundImage, cv::IMREAD_COLOR);
      if (img.empty()) { img = cv::Mat(sz, CV_8UC3, cv::Scalar(30, 30, 30)); }
      coverFit(img, bgImage_, sz);
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

// Person mask for this frame. Everything after the model runs on 8-bit data:
// the mask is smoothed and feathered at model resolution (192x192), upsampled
// once with INTER_LINEAR, and kept as a 3-channel weight (mask3_) so the blends
// below are single elementwise passes.
void EffectPipeline::computeMask(const cv::Mat& work) {
  // Segmenter cadence by tier: every frame / every 2nd / every 3rd; the
  // temporal EMA (maskSmall_) is reused in between (the mask barely moves).
  int every = tier_ == 0 ? 1 : (tier_ == 1 ? 2 : 3);
  bool haveOld = !maskSmall_.empty() && !mask8_.empty() && mask8_.size() == work.size();
  if (haveOld && frameIdx_ % every != 0) return;
  cv::Mat prob;
  if (!segmenter_.infer(pyr_.atLeastWide(192), prob)) {
    mask3_ = cv::Mat(work.size(), CV_8UC3, cv::Scalar::all(255));
    mask8_ = cv::Mat(work.size(), CV_8UC1, cv::Scalar::all(255));
    return;
  }
  // Temporal smoothing at model resolution.
  if (maskSmall_.empty() || maskSmall_.size() != prob.size()) maskSmall_ = prob.clone();
  else cv::addWeighted(prob, 0.55, maskSmall_, 0.45, 0, maskSmall_);
  // Push uncertain probabilities towards 0/1 (smoothstep between 0.25 and
  // 0.75) so clothing with mid confidence doesn't turn milky, then feather
  // (blur at 192x192 instead of at 720p: 10x cheaper, same look after the
  // bilinear upsample) and go 8-bit.
  cv::Mat m = (maskSmall_ - 0.25f) * (1.0f / 0.5f);
  cv::min(cv::max(m, 0.0f), 1.0f, m);
  m = m.mul(m).mul(3.0f - 2.0f * m);
  m.convertTo(mask8s_, CV_8UC1, 255.0);
  cv::GaussianBlur(mask8s_, mask8s_, cv::Size(3, 3), 0);
  cv::resize(mask8s_, mask8_, work.size(), 0, 0, cv::INTER_LINEAR);
  cv::cvtColor(mask8_, mask3_, cv::COLOR_GRAY2BGR);  // 3-channel so the blends are plain elementwise loops
}

void EffectPipeline::applyBackground(cv::Mat& work) {
  cv::Mat bg;
  if (settings_.background == "image" || settings_.background == "color") {
    ensureBackground(work.size());
    bg = bgImage_;
  } else if (settings_.portrait) {
    // Portrait blur: a fixed small kernel at a working scale that shrinks with
    // intensity (1/4 -> 1/8 of the frame), so the cost is constant and the
    // blur radius grows with the setting. The foreground is excluded via a
    // premultiplied 4-channel blur (BGR*inv, inv), which keeps subject colours
    // from haloing into the background; one small F32 buffer, no merge/split.
    float I = settings_.portraitIntensity;
    double scale = 0.25 / (1.0 + I);
    if (tier_ >= 2) scale = 0.125;  // cheapest tier: always 1/8
    cv::Size ss(std::max(16, (int)std::lround(work.cols * scale)), std::max(9, (int)std::lround(work.rows * scale)));
    const cv::Mat& lvl = pyr_.atLeastWide(ss.width);
    cv::resize(lvl, bgSmall_, ss, 0, 0, cv::INTER_LINEAR);
    cv::Mat msmall;
    cv::resize(mask8_, msmall, ss, 0, 0, cv::INTER_AREA);
    pre_.create(ss, CV_32FC4);
    for (int y = 0; y < ss.height; y++) {
      const uchar* p = bgSmall_.ptr<uchar>(y);
      const uchar* pm = msmall.ptr<uchar>(y);
      float* d = pre_.ptr<float>(y);
      for (int x = 0; x < ss.width; x++) {
        float inv = (255 - pm[x]) * (1.0f / 255);
        d[x * 4 + 0] = p[x * 3 + 0] * inv; d[x * 4 + 1] = p[x * 3 + 1] * inv; d[x * 4 + 2] = p[x * 3 + 2] * inv; d[x * 4 + 3] = inv;
      }
    }
    const int k = 9;
    cv::GaussianBlur(pre_, blurF_, cv::Size(k, k), 0);
    // second pass for a creamier bokeh at high intensity (skipped on the low tiers)
    if (I > 0.5f && tier_ == 0) cv::GaussianBlur(blurF_, blurF_, cv::Size(k, k), 0);
    for (int y = 0; y < ss.height; y++) {
      const float* s = blurF_.ptr<float>(y);
      uchar* d = bgSmall_.ptr<uchar>(y);
      for (int x = 0; x < ss.width; x++) {
        float den = std::max(s[x * 4 + 3], 0.02f), r = 1.0f / den;
        d[x * 3 + 0] = cv::saturate_cast<uchar>(s[x * 4 + 0] * r);
        d[x * 3 + 1] = cv::saturate_cast<uchar>(s[x * 4 + 1] * r);
        d[x * 3 + 2] = cv::saturate_cast<uchar>(s[x * 4 + 2] * r);
      }
    }
    cv::resize(bgSmall_, bg_, work.size(), 0, 0, cv::INTER_LINEAR);
    bg = bg_;
  } else {
    return;
  }
  blend8(work, bg, mask3_, work_);
  work = work_;
}

namespace {
// 8-bit factor plane (3 identical channels): dim * (1 - (1 - vig) * strength)
// with vig = 1 in the middle falling to 0.45 in the corners. Cached by the
// callers (studio light's background, the vintage filter); a few ms to build.
void buildVignette(cv::Mat& plane, const cv::Size& sz, float strength, float dim) {
  plane.create(sz, CV_8UC3);
  float cx = sz.width / 2.f, cy = sz.height / 2.f, rmax = std::sqrt(cx * cx + cy * cy);
  for (int y = 0; y < sz.height; y++) {
    uchar* row = plane.ptr<uchar>(y);
    for (int x = 0; x < sz.width; x++) {
      float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) / rmax;
      float vig = 1.0f - 0.55f * d * d;
      uchar f = cv::saturate_cast<uchar>(255.0f * dim * (1.0f - (1.0f - vig) * strength));
      row[x * 3] = row[x * 3 + 1] = row[x * 3 + 2] = f;
    }
  }
}
// out = a * f / 255, elementwise 8-bit (may alias).
void mul8(const cv::Mat& a, const cv::Mat& f, cv::Mat& out) {
  CV_Assert(a.size() == f.size() && a.type() == CV_8UC3 && f.type() == CV_8UC3);
  out.create(a.size(), CV_8UC3);
  const int n = a.cols * 3;
  cv::parallel_for_(cv::Range(0, a.rows), [&](const cv::Range& r) {
    for (int y = r.start; y < r.end; y++) {
      const uchar* pa = a.ptr<uchar>(y);
      const uchar* pf = f.ptr<uchar>(y);
      uchar* pd = out.ptr<uchar>(y);
      for (int i = 0; i < n; i++) { unsigned v = pa[i] * pf[i] + 128; pd[i] = (uchar)((v + (v >> 8)) >> 8); }
    }
  });
}
}  // namespace

// Colour filters, all 8-bit single passes (~1 ms at 720p): a colour matrix
// (cv::transform), a per-channel LUT, or a small blur/kernel. The result goes
// to filt_ (never in place: `work` may still be the capture buffer).
void EffectPipeline::applyFilter(cv::Mat& work) {
  const std::string& f = settings_.filter;
  // Colour matrices act on [B G R 1]. Standard sepia (in BGR order) and the
  // luma weights used for (de)saturation.
  static const cv::Matx33f sepia(0.131f, 0.534f, 0.272f, 0.168f, 0.686f, 0.349f, 0.189f, 0.769f, 0.393f);
  static const cv::Matx33f luma(0.114f, 0.587f, 0.299f, 0.114f, 0.587f, 0.299f, 0.114f, 0.587f, 0.299f);
  auto sat = [](float s) { return luma * (1 - s) + cv::Matx33f::eye() * s; };
  if (f == "mono") {
    cv::cvtColor(work, filtTmp_, cv::COLOR_BGR2GRAY);
    cv::cvtColor(filtTmp_, filt_, cv::COLOR_GRAY2BGR);
  } else if (f == "sepia") {
    cv::transform(work, filt_, sepia);
  } else if (f == "vivid") {  // more saturation, a touch more contrast (about the mid grey)
    const float c = 1.08f;
    cv::Matx33f m = sat(1.45f) * c;
    cv::Matx34f M(m(0, 0), m(0, 1), m(0, 2), 128 * (1 - c), m(1, 0), m(1, 1), m(1, 2), 128 * (1 - c), m(2, 0), m(2, 1), m(2, 2), 128 * (1 - c));
    cv::transform(work, filt_, M);
  } else if (f == "vintage") {  // faded mild sepia, lifted blacks, vignette
    const float c = 0.92f, lift = 10;
    cv::Matx33f m = (sepia * 0.5f + sat(0.7f) * 0.5f) * c;
    cv::Matx34f M(m(0, 0), m(0, 1), m(0, 2), 128 * (1 - c) + lift, m(1, 0), m(1, 1), m(1, 2), 128 * (1 - c) + lift, m(2, 0), m(2, 1), m(2, 2), 128 * (1 - c) + lift);
    cv::transform(work, filt_, M);
    if (vig_.size() != work.size()) buildVignette(vig_, work.size(), 0.75f, 1.0f);
    mul8(filt_, vig_, filt_);
  } else if (f == "warm" || f == "cool") {  // per-channel gains via one 3-channel LUT
    if (filtLutFor_ != f || filtLut_.empty()) {
      filtLut_.create(1, 256, CV_8UC3);
      float gb = f == "warm" ? 0.90f : 1.10f, gr = f == "warm" ? 1.10f : 0.90f;
      for (int i = 0; i < 256; i++) {
        uchar* e = filtLut_.ptr<uchar>(0) + i * 3;
        e[0] = cv::saturate_cast<uchar>(i * gb); e[1] = (uchar)i; e[2] = cv::saturate_cast<uchar>(i * gr);
      }
      filtLutFor_ = f;
    }
    cv::LUT(work, filtLut_, filt_);
  } else if (f == "soft") {  // blend with a quarter-resolution blur, slight lift
    cv::pyrDown(work, filtSmall_);
    cv::pyrDown(filtSmall_, filtTmp_);
    cv::resize(filtTmp_, filtSmall_, work.size(), 0, 0, cv::INTER_LINEAR);
    cv::addWeighted(work, 0.6, filtSmall_, 0.4, 6, filt_);
  } else if (f == "sharpen") {  // unsharp mask as one 3x3 kernel
    static const cv::Matx33f k(0, -0.55f, 0, -0.55f, 1 + 4 * 0.55f, -0.55f, 0, -0.55f, 0);
    cv::filter2D(work, filt_, -1, k);
  } else {
    return;
  }
  work = filt_;
}

// Studio Light. Foreground: lift the midtones with a soft gamma and a touch of
// warmth (key light) via one 3-channel LUT; background: darken with a vignette
// (a cached 8-bit factor plane, rebuilt only when the size or intensity
// changes). Composited with the 8-bit mask in one elementwise pass.
void EffectPipeline::applyStudioLight(cv::Mat& work) {
  float I = settings_.studioLightIntensity;
  if (lutI_ != I) {
    lut_.create(1, 256, CV_8UC3);
    float g = 1.0f - 0.32f * I;
    for (int i = 0; i < 256; i++) {
      double v = 255.0 * std::pow(i / 255.0, g);
      uchar* e = lut_.ptr<uchar>(0) + i * 3;
      e[0] = (uchar)std::min(255.0, v * (1.0 - 0.04 * I));
      e[1] = (uchar)std::min(255.0, v);
      e[2] = (uchar)std::min(255.0, v * (1.0 + 0.05 * I));
    }
    lutI_ = I;
  }
  if (darkFactor_.size() != work.size() || darkI_ != I) {
    // dark = flat dim blended with a vignette (1 in the middle, ~0.45 in the corners) by intensity
    buildVignette(darkFactor_, work.size(), I, 1.0f - 0.5f * I);
    darkI_ = I;
  }
  cv::LUT(work, lut_, lit_);
  // out = lit*m + (work*dark/255)*(255-m), all 8-bit, one pass.
  work_.create(work.size(), CV_8UC3);
  const int n = work.cols * 3;
  cv::parallel_for_(cv::Range(0, work.rows), [&](const cv::Range& r) {
    for (int y = r.start; y < r.end; y++) {
      const uchar* pw = work.ptr<uchar>(y);
      const uchar* pl = lit_.ptr<uchar>(y);
      const uchar* pf = darkFactor_.ptr<uchar>(y);
      const uchar* pm = mask3_.ptr<uchar>(y);
      uchar* pd = work_.ptr<uchar>(y);
      for (int i = 0; i < n; i++) {
        unsigned dk = pw[i] * pf[i] + 128; dk = (dk + (dk >> 8)) >> 8;
        unsigned v = pl[i] * pm[i] + dk * (255 - pm[i]) + 128;
        pd[i] = (uchar)((v + (v >> 8)) >> 8);
      }
    }
  });
  work = work_;
}

void EffectPipeline::process(const cv::Mat& src0, cv::Mat& out, const cv::Size& outSize, double now) {
  auto tick = [&]() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
  const bool prof = profile_;  // sampled once: a flip mid-frame must not pair a stage with tl = 0
  double tl = prof ? tick() : 0;
  auto stage = [&](int i) { if (!prof) return; double t = tick(); profAcc_[i] += t - tl; tl = t; };
  double dt = lastTime_ > 0 ? std::clamp(now - lastTime_, 0.001, 0.2) : 1.0 / 30;
  lastTime_ = now;
  frameIdx_++;
  double outAspect = (double)outSize.width / outSize.height;
  const bool handRecent = now - lastHandSeen_ < 2.0;

  // Rotation first: the rotated frame is the source for everything after
  // (faces, framing, mirror), so a 90/270 turn changes the source aspect.
  // Everything below reads `src`, which aliases the capture buffer only when
  // there is no rotation (the copy-on-write checks compare against it).
  const cv::Mat* sp = &src0;
  if (settings_.rotate == 90 || settings_.rotate == 180 || settings_.rotate == 270) {
    cv::rotate(src0, rot_, settings_.rotate == 90 ? cv::ROTATE_90_CLOCKWISE : settings_.rotate == 180 ? cv::ROTATE_180 : cv::ROTATE_90_COUNTERCLOCKWISE);
    sp = &rot_;
  }
  const cv::Mat& src = *sp;
  const cv::Rect wholeSrc(0, 0, src.cols, src.rows), wholeOut(0, 0, outSize.width, outSize.height);

  // Cadences (Hz, by tier). Palm detection idles at ~5 Hz until a hand shows
  // up, then 10 Hz while one is around; YuNet runs only for Center Stage and
  // the fun face filters, or for the hand-on-face check when a palm was found.
  const double palmHz = tier_ == 0 ? (handRecent ? 10 : 5) : (tier_ == 1 ? (handRecent ? 6 : 3) : (handRecent ? 4 : 2));
  const double faceHz = tier_ == 0 ? 10 : (tier_ == 1 ? 6 : 3);

  // Framing: the user's fit/zoom/pan, or with Center Stage the framer's crop
  // (cover geometry; the user's zoom is its minimum, the pan is automatic).
  FrameGeom geom;
  if (settings_.centerStage) { geom.crop = framer_.fullCrop(src.size(), outAspect); geom.dst = wholeOut; }
  else geom = frameGeometry(src.size(), outSize, Framing{ settings_.fit, settings_.zoom, settings_.panX, settings_.panY });

  // Faces for Center Stage are found in source coordinates (the crop is chosen
  // from them). srcPyr_ is only built when the source differs from the output.
  bool facesFresh = false;
  const bool funOn = settings_.fun != "none" && faces_.loaded();
  bool srcIsWork = geom.passthrough(src.size(), outSize);
  bool pyrReady = false;  // pyr_ already reset to this frame
  auto detectFaces = [&]() {
    int lvl = 0;
    Pyramid* p = &srcPyr_;
    // Reuse the work pyramid when it is (or will be) built from src itself.
    if (srcIsWork && (!pyrReady || pyr_.base().data == src.data)) {
      if (!pyrReady) { pyr_.reset(src); pyrReady = true; }
      p = &pyr_;
    } else {
      srcPyr_.reset(src);
    }
    const cv::Mat& s = p->atLeastWide(320, &lvl);
    lastFaces_ = faces_.detect(s, 1.0 / Pyramid::scaleOf(lvl));
    faceRects_.clear();
    for (auto& f : lastFaces_) faceRects_.push_back(f.box);
    if (funOn) fun_.update(lastFaces_, now);
    lastFaceTime_ = now; facesFresh = true;
  };
  if ((settings_.centerStage || funOn) && faces_.loaded() && now - lastFaceTime_ >= 1.0 / faceHz) detectFaces();
  stage(0);  // faces

  if (settings_.centerStage) geom.crop = framer_.update(src.size(), faceRects_, facesFresh, outSize, dt, settings_.zoom, settings_.centerStageIntensity);
  const cv::Rect crop = geom.crop, dst = geom.dst;
  framing_.geom = geom; framing_.src = src.size(); framing_.faces = (int)faceRects_.size();
  framing_.face = cv::Rect();
  for (auto& f : faceRects_) if (f.area() > framing_.face.area()) framing_.face = cv::Rect((int)f.x, (int)f.y, (int)f.width, (int)f.height);
  cv::Mat work;
  if (crop == wholeSrc && dst == wholeOut && src.size() == outSize) work = src;  // passthrough: no copy
  else if (dst == wholeOut) { cv::resize(src(crop), work_, outSize, 0, 0, cv::INTER_LINEAR); work = work_; }  // LINEAR: 5x cheaper than AREA, invisible after MJPEG
  else {
    // Contain leaves bars: black canvas, the crop scaled into its part of it.
    work_.create(outSize, CV_8UC3);
    work_.setTo(cv::Scalar::all(0));
    cv::Mat roi = work_(dst);
    cv::resize(src(crop), roi, dst.size(), 0, 0, cv::INTER_LINEAR);
    work = work_;
  }
  if (!(pyrReady && work.data == src.data)) pyr_.reset(work);  // detectFaces may have built it from src already
  // Palm detection wants the pre-effect pixels: build its level now, before
  // the effects overwrite `work` in place.
  const bool palmDue = settings_.reactions && gestures_.loaded() && now - lastPalmTime_ >= 1.0 / palmHz;
  if (palmDue) pyr_.atLeastWide(192);
  stage(1);  // crop/resize
  // Source -> output mapping for anything found in source coordinates.
  const double sx = (double)dst.width / crop.width, sy = (double)dst.height / crop.height;
  auto toWork = [&](const cv::Rect2f& f) { return cv::Rect2f((float)((f.x - crop.x) * sx + dst.x), (float)((f.y - crop.y) * sy + dst.y), (float)(f.width * sx), (float)(f.height * sy)); };

  bool needMask = settings_.portrait || settings_.studioLight || settings_.background != "none";
  if (needMask) computeMask(work);
  stage(2);  // mask
  if (settings_.portrait || settings_.background != "none") applyBackground(work);
  stage(3);  // background
  if (settings_.studioLight) applyStudioLight(work);
  stage(4);  // studio light
  // Face accessories: tracks are in source coordinates, mapped through the crop.
  if (funOn) {
    if (work.data == src.data) { src.copyTo(work_); work = work_; }  // don't draw into the capture buffer
    fun_.render(work, settings_.fun, cv::Point2f((float)(crop.x - dst.x / sx), (float)(crop.y - dst.y / sy)), cv::Point2f((float)sx, (float)sy), now);
  }
  stage(5);  // fun

  // Gesture-triggered reactions, checked on the output frame at palmHz.
  if (palmDue) {
    lastPalmTime_ = now;
    auto hands = gestures_.detect(work, pyr_.atLeastWide(192));
    if (!hands.empty()) {
      lastHandSeen_ = now;
      // A hand is in view: (re)detect faces for the on-face check if not fresh.
      if (faces_.loaded() && now - lastFaceTime_ > 1.0 / faceHz) detectFaces();
    }
    // Faces are in src coordinates; map into work coordinates for the on-face check.
    std::vector<cv::Rect2f> facesWork;
    if (now - lastFaceTime_ < 1.0) for (auto& f : faceRects_) facesWork.push_back(toWork(f));
    std::string g = gestures_.classify(hands, facesWork);
    if (debugGestures_) fprintf(stderr, "gesture: hands=%zu faces=%zu -> '%s'\n", hands.size(), facesWork.size(), g.c_str());
    if (g.empty() || g != pendingGesture_) { pendingGesture_ = g; pendingSince_ = now; }
    gestures_.noteGesture(g);
    // ~0.7 s of a steady gesture, then a cooldown so it does not re-fire while held.
    if (!g.empty() && now - pendingSince_ >= 0.6 && now - lastGestureTime_ > 4.0) {
      reactions_.trigger(g);
      lastGestureTime_ = now;
      pendingGesture_.clear();
    }
  }
  stage(6);  // gestures
  if (settings_.filter != "none") applyFilter(work);
  stage(7);  // colour filter
  if (reactions_.active()) {
    if (work.data == src.data) { src.copyTo(work_); work = work_; }  // don't draw into the capture buffer
    reactions_.render(work, now);
  }
  stage(8);  // reactions
  if (settings_.mirror) { cv::flip(work, work.data == src.data ? work_ : work, 1); if (work.data == src.data) work = work_; }
  out = work;
  if (!prof) {
    // profile off: drop the stale line and start the next window fresh (this
    // thread owns profileLine_/profAcc_; setProfile only flips the flag)
    if (!profileLine_.empty()) profileLine_.clear();
    if (profN_) { for (auto& a : profAcc_) a = 0; profN_ = 0; }
  } else if (++profN_ == 60) {
    char b[256];
    snprintf(b, sizeof b, "ms/frame: faces %.1f resize %.1f mask %.1f bg %.1f light %.1f fun %.1f gestures %.1f filter %.1f react %.1f",
             profAcc_[0] / profN_, profAcc_[1] / profN_, profAcc_[2] / profN_, profAcc_[3] / profN_, profAcc_[4] / profN_, profAcc_[5] / profN_,
             profAcc_[6] / profN_, profAcc_[7] / profN_, profAcc_[8] / profN_);
    profileLine_ = b;
    for (auto& a : profAcc_) a = 0;
    profN_ = 0;
  }
}
