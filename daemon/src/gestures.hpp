#pragma once
#include <opencv2/core.hpp>
#include <string>
#include <vector>

#include "models.hpp"

struct Hand {
  std::vector<cv::Point2f> lm;  // 21 screen landmarks (frame coordinates)
  float score = 0;
  cv::Rect2f box;
};

// MediaPipe palm detection + hand landmarks (opencv_zoo ONNX exports,
// Apache-2.0), plus rule-based classification into the macOS reaction gestures.
class GestureDetector {
public:
  bool load(const std::string& modelsDir, std::string* err);
  bool loaded() const { return palm_.loaded() && landmark_.loaded(); }
  // `small` is a downscaled copy of `bgr` (same aspect, ~192-320 px wide) used
  // for palm detection; landmarks are taken from the full-resolution `bgr`.
  std::vector<Hand> detect(const cv::Mat& bgr, const cv::Mat& small);
  std::vector<Hand> detect(const cv::Mat& bgr) { return detect(bgr, bgr); }
  // Classify the set of hands into a reaction name ("" if none):
  // hearts | thumbsup | thumbsdown | balloons | fireworks | rain | confetti | lasers
  std::string classify(const std::vector<Hand>& hands, const std::vector<cv::Rect2f>& faces) const;
  std::string lastGesture() const { return last_; }
  void noteGesture(const std::string& g) { last_ = g; }

private:
  OrtModel palm_, landmark_;
  std::vector<cv::Point2f> anchors_;
  std::vector<float> in_;
  cv::Mat resized_, rgb_;
  std::string last_;
  bool landmarksFor(const cv::Mat& bgr, const std::vector<float>& palm, Hand& out);
public:
  static std::string singleHandGesture(const Hand& h, std::string* debug = nullptr);
};
