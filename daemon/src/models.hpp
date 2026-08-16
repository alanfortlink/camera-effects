#pragma once
#include <memory>
#include <onnxruntime_cxx_api.h>
#include <opencv2/core.hpp>
#include <opencv2/objdetect.hpp>
#include <string>
#include <vector>

// Thin ORT session wrapper (CPU EP; the models used here are all small enough
// that CPU inference is a few ms per frame).
class OrtModel {
public:
  bool load(const std::string& path, std::string* err, int threads = 2);
  bool loaded() const { return session_ != nullptr; }
  // Runs with a single float input tensor of the given shape; returns outputs.
  std::vector<Ort::Value> run(const float* data, const std::vector<int64_t>& shape);
  const std::vector<int64_t>& inputShape() const { return inputShape_; }
  const std::string& inputName() const { return inputName_; }

private:
  static Ort::Env& env();
  std::unique_ptr<Ort::Session> session_;
  std::string inputName_;
  std::vector<std::string> outputNames_;
  std::vector<int64_t> inputShape_;
};

// PP-HumanSeg (opencv_zoo, Apache-2.0): 192x192 RGB in, 2-class logits out.
class PersonSegmenter {
public:
  bool load(const std::string& modelPath, std::string* err) { return model_.load(modelPath, err, 0); }
  bool loaded() const { return model_.loaded(); }
  // Returns a CV_32F probability map (person = 1) at model resolution (192x192).
  bool infer(const cv::Mat& bgr, cv::Mat& prob);

private:
  OrtModel model_;
  std::vector<float> input_;
};

// YuNet via OpenCV's FaceDetectorYN (BSD-3). Runs on a downscaled frame.
class FaceDetector {
public:
  bool load(const std::string& modelPath, std::string* err);
  bool loaded() const { return det_ != nullptr; }
  // Face boxes in the coordinates of `bgr`.
  std::vector<cv::Rect2f> detect(const cv::Mat& bgr);

private:
  cv::Ptr<cv::FaceDetectorYN> det_;
  cv::Size inputSize_;
};
