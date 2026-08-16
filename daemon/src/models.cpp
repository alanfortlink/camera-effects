#include "models.hpp"

#include <cmath>
#include <opencv2/imgproc.hpp>

Ort::Env& OrtModel::env() {
  static Ort::Env e(ORT_LOGGING_LEVEL_ERROR, "camfxd");
  return e;
}

bool OrtModel::load(const std::string& path, std::string* err, int threads) {
  try {
    Ort::SessionOptions so;
    so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    if (threads > 0) so.SetIntraOpNumThreads(threads);
    so.SetInterOpNumThreads(1);
    // Don't spin-wait between frames; we'd rather give the CPU to the apps.
    so.AddConfigEntry("session.intra_op.allow_spinning", "0");
    session_ = std::make_unique<Ort::Session>(env(), path.c_str(), so);
    Ort::AllocatorWithDefaultOptions alloc;
    inputName_ = session_->GetInputNameAllocated(0, alloc).get();
    inputShape_ = session_->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    for (auto& d : inputShape_) if (d < 0) d = 1;
    size_t n = session_->GetOutputCount();
    outputNames_.clear();
    for (size_t i = 0; i < n; i++) outputNames_.push_back(session_->GetOutputNameAllocated(i, alloc).get());
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
  auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  Ort::Value in = Ort::Value::CreateTensor<float>(mem, const_cast<float*>(data), n, shape.data(), shape.size());
  const char* inNames[] = { inputName_.c_str() };
  std::vector<const char*> outNames;
  for (auto& s : outputNames_) outNames.push_back(s.c_str());
  return session_->Run(Ort::RunOptions{ nullptr }, inNames, &in, 1, outNames.data(), outNames.size());
}

// ---------------------------------------------------------------------------

bool PersonSegmenter::infer(const cv::Mat& bgr, cv::Mat& prob) {
  if (!model_.loaded()) return false;
  const int S = 192;
  cv::Mat small, rgb;
  cv::resize(bgr, small, cv::Size(S, S), 0, 0, cv::INTER_AREA);
  cv::cvtColor(small, rgb, cv::COLOR_BGR2RGB);
  input_.resize(3 * S * S);
  // NCHW, (x/255 - 0.5) / 0.5
  for (int c = 0; c < 3; c++) {
    float* dst = input_.data() + c * S * S;
    for (int y = 0; y < S; y++) {
      const uchar* row = rgb.ptr<uchar>(y);
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

std::vector<cv::Rect2f> FaceDetector::detect(const cv::Mat& bgr) {
  std::vector<cv::Rect2f> faces;
  if (!det_) return faces;
  // Detect on a 320-wide copy; YuNet is fast enough there and small faces are
  // not what framing cares about.
  float scale = 320.0f / bgr.cols;
  cv::Size sz(320, std::max(32, (int)std::lround(bgr.rows * scale)));
  cv::Mat small;
  cv::resize(bgr, small, sz, 0, 0, cv::INTER_AREA);
  if (sz != inputSize_) { det_->setInputSize(sz); inputSize_ = sz; }
  cv::Mat res;
  det_->detect(small, res);
  for (int i = 0; i < res.rows; i++) {
    const float* r = res.ptr<float>(i);
    faces.emplace_back(r[0] / scale, r[1] / scale, r[2] / scale, r[3] / scale);
  }
  return faces;
}
