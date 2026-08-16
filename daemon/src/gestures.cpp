#include "gestures.hpp"

#include <cmath>
#include <cstdio>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/geometry/2d.hpp>

namespace {
constexpr int PALM_IN = 192;
constexpr int LM_IN = 224;

// SSD anchors for the MediaPipe palm model: 24x24 grid x2, then 12x12 grid x6.
std::vector<cv::Point2f> makeAnchors() {
  std::vector<cv::Point2f> a;
  for (int y = 0; y < 24; y++)
    for (int x = 0; x < 24; x++)
      for (int k = 0; k < 2; k++) a.emplace_back((x + 0.5f) / 24.f, (y + 0.5f) / 24.f);
  for (int y = 0; y < 12; y++)
    for (int x = 0; x < 12; x++)
      for (int k = 0; k < 6; k++) a.emplace_back((x + 0.5f) / 12.f, (y + 0.5f) / 12.f);
  return a;
}

float dist(const cv::Point2f& a, const cv::Point2f& b) { return (float)cv::norm(a - b); }
}  // namespace

bool GestureDetector::load(const std::string& dir, std::string* err) {
  if (!palm_.load(dir + "/palm_detection.onnx", err)) return false;
  if (!landmark_.load(dir + "/hand_landmark.onnx", err)) return false;
  anchors_ = makeAnchors();
  return true;
}

std::vector<Hand> GestureDetector::detect(const cv::Mat& bgr, const cv::Mat& small) {
  std::vector<Hand> hands;
  if (!loaded()) return hands;
  // Letterbox the small copy to 192x192 (INTER_LINEAR from a pyramid level:
  // the INTER_AREA path from 720p was ~2 ms, more than the model itself).
  float ratio = std::min((float)PALM_IN / small.cols, (float)PALM_IN / small.rows);
  int rw = (int)(small.cols * ratio), rh = (int)(small.rows * ratio);
  int padL = (PALM_IN - rw) / 2, padT = (PALM_IN - rh) / 2;
  cv::resize(small, resized_, cv::Size(rw, rh), 0, 0, cv::INTER_LINEAR);
  cv::copyMakeBorder(resized_, resized_, padT, PALM_IN - rh - padT, padL, PALM_IN - rw - padL, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
  cv::cvtColor(resized_, rgb_, cv::COLOR_BGR2RGB);
  in_.resize(PALM_IN * PALM_IN * 3);
  const uchar* p = rgb_.ptr<uchar>();
  for (int i = 0; i < PALM_IN * PALM_IN * 3; i++) in_[i] = p[i] / 255.0f;
  auto out = palm_.run(in_.data(), { 1, PALM_IN, PALM_IN, 3 });
  if (out.size() < 2) return hands;
  const float* reg = nullptr;
  const float* sc = nullptr;
  for (auto& o : out) {
    auto sh = o.GetTensorTypeAndShapeInfo().GetShape();
    if (sh.back() == 18) reg = o.GetTensorData<float>();
    else if (sh.back() == 1) sc = o.GetTensorData<float>();
  }
  if (!reg || !sc) return hands;
  // Palm coordinates come out in `small` space; scale them into `bgr` space.
  const float up = (float)bgr.cols / small.cols;
  const float scale = (float)std::max(small.cols, small.rows) * up;
  const float biasX = padL / ratio * up, biasY = padT / ratio * up;
  std::vector<cv::Rect2d> boxes;
  std::vector<float> scores;
  std::vector<std::vector<float>> palms;  // x1,y1,x2,y2, 7x(x,y), score
  for (size_t i = 0; i < anchors_.size(); i++) {
    float s = 1.f / (1.f + std::exp(-sc[i]));
    if (s < 0.6f) continue;
    const float* r = reg + i * 18;
    float cx = r[0] / PALM_IN + anchors_[i].x, cy = r[1] / PALM_IN + anchors_[i].y;
    float w = r[2] / PALM_IN, h = r[3] / PALM_IN;
    float x1 = (cx - w / 2) * scale - biasX, y1 = (cy - h / 2) * scale - biasY;
    float x2 = (cx + w / 2) * scale - biasX, y2 = (cy + h / 2) * scale - biasY;
    std::vector<float> pv = { x1, y1, x2, y2 };
    for (int k = 0; k < 7; k++) {
      pv.push_back((r[4 + k * 2] / PALM_IN + anchors_[i].x) * scale - biasX);
      pv.push_back((r[5 + k * 2] / PALM_IN + anchors_[i].y) * scale - biasY);
    }
    pv.push_back(s);
    boxes.emplace_back(x1, y1, x2 - x1, y2 - y1);
    scores.push_back(s);
    palms.push_back(std::move(pv));
  }
  std::vector<int> keep;
  cv::dnn::NMSBoxes(boxes, scores, 0.6f, 0.3f, keep);
  for (int idx : keep) {
    if (hands.size() >= 2) break;
    Hand h;
    if (landmarksFor(bgr, palms[idx], h)) hands.push_back(std::move(h));
  }
  return hands;
}

// Port of opencv_zoo MPHandPose pre/post-processing (crop, rotate so the hand
// is upright, run, de-rotate).
bool GestureDetector::landmarksFor(const cv::Mat& bgr, const std::vector<float>& palm, Hand& out) {
  auto cropAndPad = [&](const cv::Mat& img, cv::Point2f p0, cv::Point2f p1, bool forRotation, cv::Mat& crop,
                        cv::Point2f& b0, cv::Point2f& b1, cv::Point2f& bias) {
    cv::Point2f wh = p1 - p0;
    cv::Point2f shift = forRotation ? cv::Point2f(0, 0) : cv::Point2f(0, -0.4f * wh.y);
    p0 += shift; p1 += shift;
    cv::Point2f c = (p0 + p1) * 0.5f;
    wh = p1 - p0;
    float enlarge = forRotation ? 4.f : 3.f;
    cv::Point2f half = wh * (enlarge / 2);
    p0 = c - half; p1 = c + half;
    int x0 = std::clamp((int)p0.x, 0, img.cols), x1 = std::clamp((int)p1.x, 0, img.cols);
    int y0 = std::clamp((int)p0.y, 0, img.rows), y1 = std::clamp((int)p1.y, 0, img.rows);
    if (x1 <= x0 || y1 <= y0) return false;
    cv::Mat roi = img(cv::Rect(x0, y0, x1 - x0, y1 - y0));
    int side = forRotation ? (int)std::sqrt((double)roi.cols * roi.cols + (double)roi.rows * roi.rows) : std::max(roi.cols, roi.rows);
    int padW = side - roi.cols, padH = side - roi.rows;
    int left = padW / 2, top = padH / 2;
    cv::copyMakeBorder(roi, crop, top, padH - top, left, padW - left, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
    b0 = cv::Point2f((float)x0, (float)y0); b1 = cv::Point2f((float)x1, (float)y1);
    bias = cv::Point2f((float)(x0 - left), (float)(y0 - top));
    return true;
  };

  cv::Mat crop1;
  cv::Point2f pb0, pb1, bias;
  if (!cropAndPad(bgr, { palm[0], palm[1] }, { palm[2], palm[3] }, true, crop1, pb0, pb1, bias)) return false;
  cv::cvtColor(crop1, crop1, cv::COLOR_BGR2RGB);
  std::vector<cv::Point2f> plm(7);
  for (int k = 0; k < 7; k++) plm[k] = cv::Point2f(palm[4 + k * 2], palm[5 + k * 2]) - bias;
  cv::Point2f p1 = plm[0], p2 = plm[2];
  double rad = CV_PI / 2 - std::atan2(-(p2.y - p1.y), p2.x - p1.x);
  rad = rad - 2 * CV_PI * std::floor((rad + CV_PI) / (2 * CV_PI));
  double angle = rad * 180.0 / CV_PI;
  cv::Point2f center = ((pb0 - bias) + (pb1 - bias)) * 0.5f;
  cv::Mat R = cv::getRotationMatrix2D(center, angle, 1.0);
  cv::Mat rotated;
  cv::warpAffine(crop1, rotated, R, crop1.size());
  cv::Point2f rmin(1e9, 1e9), rmax(-1e9, -1e9);
  for (auto& q : plm) {
    float rx = (float)(R.at<double>(0, 0) * q.x + R.at<double>(0, 1) * q.y + R.at<double>(0, 2));
    float ry = (float)(R.at<double>(1, 0) * q.x + R.at<double>(1, 1) * q.y + R.at<double>(1, 2));
    rmin.x = std::min(rmin.x, rx); rmin.y = std::min(rmin.y, ry);
    rmax.x = std::max(rmax.x, rx); rmax.y = std::max(rmax.y, ry);
  }
  cv::Mat crop2;
  cv::Point2f rb0, rb1, bias2;
  if (!cropAndPad(rotated, rmin, rmax, false, crop2, rb0, rb1, bias2)) return false;
  cv::Mat blob;
  cv::resize(crop2, blob, cv::Size(LM_IN, LM_IN), 0, 0, cv::INTER_AREA);
  in_.resize(LM_IN * LM_IN * 3);
  const uchar* p = blob.ptr<uchar>();
  for (int i = 0; i < LM_IN * LM_IN * 3; i++) in_[i] = p[i] / 255.0f;
  auto res = landmark_.run(in_.data(), { 1, LM_IN, LM_IN, 3 });
  if (res.size() < 3) return false;
  const float* lm = res[0].GetTensorData<float>();
  float conf = res[1].GetTensorData<float>()[0];
  if (conf < 0.7f) return false;

  cv::Point2f whRot = rb1 - rb0;
  float sf = std::max(whRot.x / LM_IN, whRot.y / LM_IN);
  cv::Mat CR = cv::getRotationMatrix2D(cv::Point2f(0, 0), angle, 1.0);
  // inverse of R (rotation part transposed, translation inverted)
  double r00 = R.at<double>(0, 0), r01 = R.at<double>(0, 1), r10 = R.at<double>(1, 0), r11 = R.at<double>(1, 1);
  double t0 = R.at<double>(0, 2), t1 = R.at<double>(1, 2);
  double it0 = -(r00 * t0 + r10 * t1), it1 = -(r01 * t0 + r11 * t1);
  cv::Point2f rc = (rb0 + rb1) * 0.5f;
  double ocx = r00 * rc.x + r10 * rc.y + it0, ocy = r01 * rc.x + r11 * rc.y + it1;
  out.lm.resize(21);
  cv::Point2f mn(1e9, 1e9), mx(-1e9, -1e9);
  for (int i = 0; i < 21; i++) {
    float x = (lm[i * 3] - LM_IN / 2.f) * sf, y = (lm[i * 3 + 1] - LM_IN / 2.f) * sf;
    // rotated = [x y] . CR[:, :2]
    float rx = (float)(x * CR.at<double>(0, 0) + y * CR.at<double>(1, 0));
    float ry = (float)(x * CR.at<double>(0, 1) + y * CR.at<double>(1, 1));
    cv::Point2f pt(rx + (float)ocx + bias.x, ry + (float)ocy + bias.y);
    out.lm[i] = pt;
    mn.x = std::min(mn.x, pt.x); mn.y = std::min(mn.y, pt.y);
    mx.x = std::max(mx.x, pt.x); mx.y = std::max(mx.y, pt.y);
  }
  out.score = conf;
  out.box = cv::Rect2f(mn, mx);
  return true;
}

// Finger extension via distances from the wrist: an extended finger has its
// tip well beyond its PIP joint. Rotation-invariant, good enough for the
// coarse gesture set we care about.
std::string GestureDetector::singleHandGesture(const Hand& h, std::string* debug) {
  const auto& L = h.lm;
  if (L.size() < 21) return "";
  const cv::Point2f wrist = L[0];
  float size = std::max(dist(wrist, L[9]), 1.f);  // wrist -> middle MCP
  auto extended = [&](int mcp, int pip, int tip) { return dist(L[tip], wrist) > dist(L[pip], wrist) * 1.15f && dist(L[tip], wrist) > dist(L[mcp], wrist) * 1.25f; };
  bool idx = extended(5, 6, 8), mid = extended(9, 10, 12), ring = extended(13, 14, 16), pinky = extended(17, 18, 20);
  // Thumb: tip far from the index MCP and from the palm centre.
  cv::Point2f palmC = (L[0] + L[5] + L[17]) * (1.f / 3.f);
  // Thumb extended: tip well away from its own MCP, further from the palm
  // centre than the IP joint, and clear of the index MCP (not tucked in).
  float thumbLen = dist(L[4], L[2]);
  float straight = thumbLen / std::max(dist(L[4], L[3]) + dist(L[3], L[2]), 1e-3f);
  float idxProx = std::max(dist(L[5], L[6]), 1e-3f);
  // Two normalisations because a fist seen from the side makes wrist->MCP
  // long while a fist seen from the front makes the index phalanx long.
  bool thumb = (thumbLen > 0.7f * size || thumbLen > 1.5f * idxProx) && straight > 0.9f && dist(L[4], palmC) > 1.15f * dist(L[3], palmC);
  int n = idx + mid + ring + pinky;
  if (debug) {
    cv::Point2f dir = L[4] - L[2];
    float len = std::max((float)cv::norm(dir), 1e-3f);
    char b[256];
    snprintf(b, sizeof b, "idx=%d mid=%d ring=%d pinky=%d thumb=%d (len/idxProx=%.2f straight=%.2f d4p/d3p=%.2f) upness=%.2f", idx, mid, ring, pinky, thumb, thumbLen / idxProx, straight, dist(L[4], palmC) / dist(L[3], palmC), -dir.y / len);
    *debug = b;
  }
  if (thumb && n == 0) {
    cv::Point2f dir = L[4] - L[2];
    float len = std::max((float)cv::norm(dir), 1e-3f);
    float upness = -dir.y / len;  // image y grows downward
    if (upness > 0.45f) return "thumbsup";
    if (upness < -0.45f) return "thumbsdown";
    return "";
  }
  if (idx && mid && !ring && !pinky) return "peace";
  if (idx && pinky && !mid && !ring) return "horns";
  return "";
}

std::string GestureDetector::classify(const std::vector<Hand>& hands, const std::vector<cv::Rect2f>& faces) const {
  if (hands.empty()) return "";
  // Hands over the face are usually adjustments, not gestures.
  auto onFace = [&](const Hand& h) {
    cv::Point2f c = (h.box.tl() + h.box.br()) * 0.5f;
    for (auto& f : faces) {
      cv::Rect2f zone(f.x - f.width * 0.5f, f.y - f.height * 0.5f, f.width * 2.f, f.height * 2.2f);
      if (zone.contains(c)) return true;
    }
    return false;
  };
  std::vector<std::string> g;
  for (auto& h : hands) if (!onFace(h)) g.push_back(singleHandGesture(h));
  if (g.empty()) return "";
  if (hands.size() >= 2) {
    const Hand& a = hands[0];
    const Hand& b = hands[1];
    float size = std::max({ dist(a.lm[0], a.lm[9]), dist(b.lm[0], b.lm[9]), 1.f });
    // Two-hand heart: index tips touch, thumb tips touch, index tips above thumbs.
    if (dist(a.lm[8], b.lm[8]) < 0.9f * size && dist(a.lm[4], b.lm[4]) < 1.2f * size &&
        a.lm[8].y < a.lm[4].y - 0.4f * size && b.lm[8].y < b.lm[4].y - 0.4f * size)
      return "hearts";
    if (g.size() == 2 && g[0] == g[1]) {
      if (g[0] == "thumbsup") return "fireworks";
      if (g[0] == "thumbsdown") return "rain";
      if (g[0] == "peace") return "confetti";
      if (g[0] == "horns") return "lasers";
    }
  }
  for (auto& s : g) {
    if (s == "thumbsup") return "thumbsup";
    if (s == "thumbsdown") return "thumbsdown";
    if (s == "peace") return "balloons";
  }
  return "";
}
