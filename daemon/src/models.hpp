#pragma once
#include <memory>
#include <onnxruntime_cxx_api.h>
#include <opencv2/core.hpp>
#include <opencv2/objdetect.hpp>
#include <string>
#include <vector>

// Thin ORT session wrapper (CPU EP; the models used here are all small enough
// that CPU inference is a few ms per frame). All sessions share one global
// thread pool (2 intra-op threads, no spinning) so the process does not end
// up with a worker thread per session per core, which is what hurts weak
// laptops most: the CPU time saved matters more than 2 ms of model latency.
class OrtModel {
public:
  bool load(const std::string& path, std::string* err);
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
  std::vector<const char*> outNamePtrs_;   // cached for run()
  std::vector<int64_t> inputShape_;
  Ort::MemoryInfo mem_{ nullptr };
};

// PP-HumanSeg (opencv_zoo, Apache-2.0): 192x192 RGB in, 2-class logits out.
class PersonSegmenter {
public:
  bool load(const std::string& modelPath, std::string* err) { return model_.load(modelPath, err); }
  bool loaded() const { return model_.loaded(); }
  // Returns a CV_32F probability map (person = 1) at model resolution (192x192).
  // `small` should already be a downscaled frame (a pyramid level near 192 px
  // wide); it is resized to 192x192 with INTER_LINEAR.
  bool infer(const cv::Mat& small, cv::Mat& prob);

private:
  OrtModel model_;
  std::vector<float> input_;
  cv::Mat sq_, rgb_;
};

// One detected face: box plus YuNet's 5 landmarks, all in the same coordinates.
struct Face {
  cv::Rect2f box;
  cv::Point2f lm[5];  // right eye, left eye, nose tip, right mouth corner, left mouth corner
  float score = 0;
};

// YuNet via OpenCV's FaceDetectorYN (BSD-3). Runs on a downscaled frame.
class FaceDetector {
public:
  bool load(const std::string& modelPath, std::string* err);
  bool loaded() const { return det_ != nullptr; }
  // Faces in the coordinates of `small` multiplied by `toSrc` (the caller
  // passes a pyramid level of the frame and its scale back to full size).
  // `small` wider than 320 px is resized down (INTER_LINEAR).
  std::vector<Face> detect(const cv::Mat& small, double toSrc = 1.0);

private:
  cv::Ptr<cv::FaceDetectorYN> det_;
  cv::Size inputSize_;
  cv::Mat in_;
};
