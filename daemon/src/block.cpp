#include "block.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <thread>

#include "effects.hpp"  // coverFit

namespace {
double monoNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}

bool blockSourceValid(const std::string& p, std::string* why) {
  if (p.empty()) return true;
  auto fail = [&](const char* w) { if (why) *why = w; return false; };
  if (p.size() > 4096) return fail("path too long");
  if (p[0] != '/') return fail("path must be absolute");
  struct stat st{};
  if (stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return fail("not an existing regular file");
  return true;
}

void BlockFeed::drawCard(cv::Mat& m) {
  const int W = m.cols, H = m.rows;
  m.setTo(cv::Scalar(34, 30, 28));
  const cv::Scalar ink(165, 160, 150);
  // Camera-off glyph: a circle with a slash, above the caption.
  cv::Point c(W / 2, (int)(H * 0.44));
  int r = std::max(8, (int)(H * 0.11)), th = std::max(2, (int)(H * 0.012));
  cv::circle(m, c, r, ink, th, cv::LINE_AA);
  // A small camera body inside (lens circle + a viewfinder bump), then the slash over everything.
  int br = std::max(3, (int)(r * 0.32));
  cv::circle(m, c, br, ink, std::max(1, th / 2), cv::LINE_AA);
  cv::rectangle(m, cv::Point(c.x - (int)(r * 0.62), c.y - (int)(r * 0.42)), cv::Point(c.x + (int)(r * 0.62), c.y + (int)(r * 0.42)), ink, std::max(1, th / 2), cv::LINE_AA);
  double d = r * 0.85;
  cv::line(m, cv::Point((int)(c.x - d), (int)(c.y - d)), cv::Point((int)(c.x + d), (int)(c.y + d)), ink, th, cv::LINE_AA);
  const char* text = "Camera paused";
  double fs = std::max(0.4, H / 720.0 * 0.9);
  int fth = std::max(1, (int)std::lround(fs * 2)), base = 0;
  cv::Size ts = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, fs, fth, &base);
  cv::putText(m, text, cv::Point((W - ts.width) / 2, c.y + r + (int)(H * 0.09) + ts.height), cv::FONT_HERSHEY_SIMPLEX, fs, ink, fth, cv::LINE_AA);
}

void BlockFeed::open(const std::string& source, const cv::Size& out, int fps) {
  if (active_ && source == source_ && out == out_ && fps == fps_) return;
  close();
  source_ = source; out_ = out; fps_ = fps;
  period_ = 1.0 / std::max(1, fps);
  next_ = monoNow();
  active_ = true;
  error_.clear();
  if (!source.empty()) {
    cv::Mat img;
    try { img = cv::imread(source, cv::IMREAD_COLOR); } catch (const std::exception&) { img.release(); }
    if (!img.empty()) { coverFit(img, still_, out); return; }
    std::string err;
    if (cap_.open(source, fps, &err)) { video_ = true; return; }
    error_ = "block source not readable";
  }
  still_.create(out, CV_8UC3);
  drawCard(still_);
}

void BlockFeed::close() {
  cap_.close();
  active_ = video_ = false;
  still_.release(); frame_.release();
  source_.clear(); error_.clear();
}

const cv::Mat& BlockFeed::next() {
  if (video_) {
    // FileCapture paces at the file's rate and loops; a decode hiccup repeats
    // the previous frame rather than dropping to black.
    bool ok = false;
    try { ok = cap_.grab(raw_, 0) && !raw_.empty(); } catch (const std::exception&) { ok = false; }
    if (ok) {
      if (raw_.size() == out_) frame_ = raw_;
      else coverFit(raw_, frame_, out_);
    } else if (frame_.empty()) {
      std::this_thread::sleep_for(std::chrono::duration<double>(period_));
      frame_.create(out_, CV_8UC3); drawCard(frame_);
    }
    return frame_;
  }
  double now = monoNow();
  if (now < next_) std::this_thread::sleep_for(std::chrono::duration<double>(next_ - now));
  next_ = std::max(next_ + period_, monoNow() - period_);
  return still_;
}
