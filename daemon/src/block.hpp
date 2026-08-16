#pragma once
#include <opencv2/core.hpp>
#include <string>

#include "capture.hpp"

// Placeholder fed to the outputs while the camera is blocked (privacy
// shutter): the built-in "Camera paused" card, a cover-fitted still image, or
// a looped video (through FileCapture). Owned and driven by the main loop; no
// physical camera is involved.
class BlockFeed {
public:
  // (Re)configures for `source` (empty = built-in card) at the output size and
  // rate; a no-op when nothing changed. A source that cannot be read falls
  // back to the card and sets error().
  void open(const std::string& source, const cv::Size& out, int fps);
  void close();
  bool active() const { return active_; }
  const std::string& source() const { return source_; }
  const std::string& error() const { return error_; }  // "" or why the source is not shown
  // Next frame to publish (BGR, output size). Paces itself: sleeps until the
  // frame is due, so the caller can loop on it.
  const cv::Mat& next();

private:
  std::string source_, error_;
  cv::Size out_;
  int fps_ = 30;
  bool active_ = false, video_ = false;
  cv::Mat still_, frame_, raw_;
  FileCapture cap_;
  double next_ = 0, period_ = 1.0 / 30;
  static void drawCard(cv::Mat& m);
};

// True when `p` may be used as a block source: empty (built-in card), or an
// absolute path (<= 4096 chars) to an existing regular file. Readability is
// only known when it is opened (see BlockFeed::error()).
bool blockSourceValid(const std::string& p, std::string* why = nullptr);
