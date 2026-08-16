#include "models.hpp"

#include <cmath>
#include <opencv2/imgproc.hpp>

Ort::Env& OrtModel::env() {
  // One process-wide thread pool for every session (see models.hpp).
  static Ort::Env e = [] {
    Ort::ThreadingOptions to;
    to.SetGlobalIntraOpNumThreads(2);
    to.SetGlobalInterOpNumThreads(1);
    to.SetGlobalSpinControl(0);  // don't spin-wait between frames; give the CPU to the apps
    return Ort::Env(to, ORT_LOGGING_LEVEL_ERROR, "camfxd");
  }();
  return e;
}

bool OrtModel::load(const std::string& path, std::string* err) {
  try {
    Ort::SessionOptions so;
    so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    so.DisablePerSessionThreads();  // use the global pool from env()
    session_ = std::make_unique<Ort::Session>(env(), path.c_str(), so);
    Ort::AllocatorWithDefaultOptions alloc;
    inputName_ = session_->GetInputNameAllocated(0, alloc).get();
    inputShape_ = session_->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    for (auto& d : inputShape_) if (d < 0) d = 1;
    size_t n = session_->GetOutputCount();
    outputNames_.clear();
    for (size_t i = 0; i < n; i++) outputNames_.push_back(session_->GetOutputNameAllocated(i, alloc).get());
    outNamePtrs_.clear();
    for (auto& s : outputNames_) outNamePtrs_.push_back(s.c_str());
    mem_ = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    return true;
  } catch (const std::exception& e) {
    if (err) *err = e.what();
    session_.reset();
    return false;
  }
}

std::vector<Ort::Value> OrtModel::run(const float* data, const std::vector<int64_t>& shape) {
  size_t n = 1;
  for (auto d : shape) n *= (size_t)d;
  Ort::Value in = Ort::Value::CreateTensor<float>(mem_, const_cast<float*>(data), n, shape.data(), shape.size());
  const char* inNames[] = { inputName_.c_str() };
  return session_->Run(Ort::RunOptions{ nullptr }, inNames, &in, 1, outNamePtrs_.data(), outNamePtrs_.size());
}

// ---------------------------------------------------------------------------

bool PersonSegmenter::infer(const cv::Mat& small, cv::Mat& prob) {
  if (!model_.loaded()) return false;
  const int S = 192;
  // INTER_LINEAR from a pyramid level: the generic INTER_AREA path from a
  // full 720p frame costs ~2 ms, this ~0.05 ms.
  cv::resize(small, sq_, cv::Size(S, S), 0, 0, cv::INTER_LINEAR);
  cv::cvtColor(sq_, rgb_, cv::COLOR_BGR2RGB);
  input_.resize(3 * S * S);
  // NCHW, (x/255 - 0.5) / 0.5
  for (int c = 0; c < 3; c++) {
    float* dst = input_.data() + c * S * S;
    for (int y = 0; y < S; y++) {
      const uchar* row = rgb_.ptr<uchar>(y);
      for (int x = 0; x < S; x++) dst[y * S + x] = (row[x * 3 + c] / 255.0f - 0.5f) / 0.5f;
    }
  }
  auto out = model_.run(input_.data(), { 1, 3, S, S });
  if (out.empty()) return false;
  const float* logits = out[0].GetTensorData<float>();  // 1x2xSxS
  prob.create(S, S, CV_32F);
  const float* bg = logits;
  const float* fg = logits + S * S;
  for (int i = 0; i < S * S; i++) {
    float d = fg[i] - bg[i];
    prob.ptr<float>()[i] = 1.0f / (1.0f + std::exp(-d));  // softmax of two classes
  }
  return true;
}

// ---------------------------------------------------------------------------

bool FaceDetector::load(const std::string& modelPath, std::string* err) {
  try {
    inputSize_ = cv::Size(320, 320);
    det_ = cv::FaceDetectorYN::create(modelPath, "", inputSize_, 0.7f, 0.3f, 50);
    return det_ != nullptr;
  } catch (const std::exception& e) {
    if (err) *err = e.what();
    return false;
  }
}

std::vector<cv::Rect2f> FaceDetector::detect(const cv::Mat& small, double toSrc) {
  std::vector<cv::Rect2f> faces;
  if (!det_ || small.empty()) return faces;
  // Detect at <=320 wide; YuNet is fast enough there and small faces are not
  // what framing cares about.
  const cv::Mat* img = &small;
  double scale = 1.0;
  if (small.cols > 320) {
    scale = 320.0 / small.cols;
    cv::resize(small, in_, cv::Size(320, std::max(32, (int)std::lround(small.rows * scale))), 0, 0, cv::INTER_LINEAR);
    img = &in_;
  }
  if (img->size() != inputSize_) { det_->setInputSize(img->size()); inputSize_ = img->size(); }
  cv::Mat res;
  det_->detect(*img, res);
  double k = toSrc / scale;
  for (int i = 0; i < res.rows; i++) {
    const float* r = res.ptr<float>(i);
    faces.emplace_back((float)(r[0] * k), (float)(r[1] * k), (float)(r[2] * k), (float)(r[3] * k));
  }
  return faces;
}
