#include "mic.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(__x86_64__) || defined(__i386__)
#include <pmmintrin.h>
#include <xmmintrin.h>
#endif

namespace {

constexpr float kPi = 3.14159265358979323846f;

float dbToLin(float db) { return std::pow(10.f, db / 20.f); }
float linToDb(float v) { return 20.f * std::log10(std::max(v, 1e-7f)); }
// One-pole smoothing coefficient for a time constant in milliseconds.
float coefMs(float rate, float ms) { return ms <= 0 ? 1.f : 1.f - std::exp(-1.f / (rate * ms * 0.001f)); }
float mixf(float a, float b, float t) { return a + (b - a) * t; }

// In-place iterative radix-2 complex FFT (n a power of two). inverse: the
// usual conjugate/scale variant.
void fft(float* re, float* im, int n, bool inverse) {
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
  }
  for (int len = 2; len <= n; len <<= 1) {
    float ang = 2 * kPi / len * (inverse ? 1.f : -1.f);
    float wr = std::cos(ang), wi = std::sin(ang);
    for (int i = 0; i < n; i += len) {
      float cr = 1, ci = 0;
      for (int k = 0; k < len / 2; k++) {
        float ur = re[i + k], ui = im[i + k];
        float vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
        float vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
        re[i + k] = ur + vr; im[i + k] = ui + vi;
        re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
        float nr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = nr;
      }
    }
  }
  if (inverse) for (int i = 0; i < n; i++) { re[i] /= n; im[i] /= n; }
}

}  // namespace

// ---------------------------------------------------------------------------
// Names

const std::vector<std::string>& MicSettings::toneNames() {
  static const std::vector<std::string> v{ "none", "warm", "bright", "clarity", "podcast", "telephone" };
  return v;
}
const std::vector<std::string>& MicSettings::voiceNames() {
  static const std::vector<std::string> v{ "none", "deep", "chipmunk", "robot", "alien", "megaphone", "monster" };
  return v;
}
const std::vector<std::string>& MicSettings::spaceNames() {
  static const std::vector<std::string> v{ "none", "room", "hall", "cathedral", "echo", "underwater" };
  return v;
}

// ---------------------------------------------------------------------------
// Biquad (RBJ cookbook)

void Biquad::highPass(float rate, float f, float q) {
  float w = 2 * kPi * std::min(f, rate * 0.45f) / rate, c = std::cos(w), s = std::sin(w), al = s / (2 * q);
  float a0 = 1 + al;
  b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = b0;
  a1 = -2 * c / a0; a2 = (1 - al) / a0;
}
void Biquad::bandPass(float rate, float f, float q) {
  float w = 2 * kPi * std::min(f, rate * 0.45f) / rate, c = std::cos(w), sn = std::sin(w), al = sn / (2 * q);
  float a0 = 1 + al;
  b0 = al / a0; b1 = 0; b2 = -al / a0;
  a1 = -2 * c / a0; a2 = (1 - al) / a0;
}
void Biquad::lowPass(float rate, float f, float q) {
  float w = 2 * kPi * std::min(f, rate * 0.45f) / rate, c = std::cos(w), s = std::sin(w), al = s / (2 * q);
  float a0 = 1 + al;
  b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0;
  a1 = -2 * c / a0; a2 = (1 - al) / a0;
}
void Biquad::peak(float rate, float f, float q, float gainDb) {
  float A = std::pow(10.f, gainDb / 40.f);
  float w = 2 * kPi * std::min(f, rate * 0.45f) / rate, c = std::cos(w), s = std::sin(w), al = s / (2 * q);
  float a0 = 1 + al / A;
  b0 = (1 + al * A) / a0; b1 = -2 * c / a0; b2 = (1 - al * A) / a0;
  a1 = -2 * c / a0; a2 = (1 - al / A) / a0;
}
void Biquad::notch(float rate, float f, float q) {
  float w = 2 * kPi * std::min(f, rate * 0.45f) / rate, c = std::cos(w), sn = std::sin(w), al = sn / (2 * q);
  float a0 = 1 + al;
  b0 = 1 / a0; b1 = -2 * c / a0; b2 = 1 / a0;
  a1 = -2 * c / a0; a2 = (1 - al) / a0;
}
void Biquad::lowShelf(float rate, float f, float gainDb) {
  float A = std::pow(10.f, gainDb / 40.f);
  float w = 2 * kPi * std::min(f, rate * 0.45f) / rate, c = std::cos(w), s = std::sin(w);
  float al = s / 2 * std::sqrt((A + 1 / A) * (1 / 0.9f - 1) + 2);
  float sq = 2 * std::sqrt(A) * al;
  float a0 = (A + 1) + (A - 1) * c + sq;
  b0 = A * ((A + 1) - (A - 1) * c + sq) / a0;
  b1 = 2 * A * ((A - 1) - (A + 1) * c) / a0;
  b2 = A * ((A + 1) - (A - 1) * c - sq) / a0;
  a1 = -2 * ((A - 1) + (A + 1) * c) / a0;
  a2 = ((A + 1) + (A - 1) * c - sq) / a0;
}
void Biquad::highShelf(float rate, float f, float gainDb) {
  float A = std::pow(10.f, gainDb / 40.f);
  float w = 2 * kPi * std::min(f, rate * 0.45f) / rate, c = std::cos(w), s = std::sin(w);
  float al = s / 2 * std::sqrt((A + 1 / A) * (1 / 0.9f - 1) + 2);
  float sq = 2 * std::sqrt(A) * al;
  float a0 = (A + 1) - (A - 1) * c + sq;
  b0 = A * ((A + 1) + (A - 1) * c + sq) / a0;
  b1 = -2 * A * ((A - 1) + (A + 1) * c) / a0;
  b2 = A * ((A + 1) + (A - 1) * c - sq) / a0;
  a1 = 2 * ((A - 1) - (A + 1) * c) / a0;
  a2 = ((A + 1) - (A - 1) * c - sq) / a0;
}

// ---------------------------------------------------------------------------
// Spectral denoise

void SpectralDenoise::init(int rate) {
  rate_ = rate;
  win_.resize(kN);
  for (int i = 0; i < kN; i++) win_[i] = 0.5f * (1 - std::cos(2 * kPi * i / kN));
  in_.assign(kN, 0.f);
  out_.assign(kN, 0.f);
  re_.assign(kN, 0.f);
  im_.assign(kN, 0.f);
  noise_.assign(kN / 2 + 1, 0.f);
  gain_.assign(kN / 2 + 1, 1.f);
  q_.assign(4 * kN, 0.f);
  reset();
}

void SpectralDenoise::reset() {
  std::fill(in_.begin(), in_.end(), 0.f);
  std::fill(out_.begin(), out_.end(), 0.f);
  std::fill(noise_.begin(), noise_.end(), 0.f);
  std::fill(gain_.begin(), gain_.end(), 1.f);
  std::fill(q_.begin(), q_.end(), 0.f);
  fill_ = 0;
  // Prime the output queue with the algorithmic latency (one frame minus one
  // hop) so a sample is always there to hand back: the delay is heard once,
  // when the effect is switched on, instead of as a dropout every frame.
  // No priming: the first hop after switching on comes out as silence (once),
  // which is cheaper than carrying an extra hop of latency for ever.
  qr_ = 0; qw_ = 0; qn_ = 0;
  ready_ = false;
}

// One frame: window, FFT, per-bin gain from the tracked noise floor, IFFT,
// window again and overlap-add. The oldest hop of the accumulator is then
// finished and goes to the output queue.
void SpectralDenoise::frame() {
  // The noise floor is an average of past magnitudes; if anything ever pushes
  // it to inf/NaN the Wiener gain becomes NaN and stays NaN, which is silence
  // for ever. Start it over instead.
  if (!std::isfinite(noise_[0]) || !std::isfinite(noise_[kN / 4])) { reset(); return; }
  for (int i = 0; i < kN; i++) { re_[i] = in_[i] * win_[i]; im_[i] = 0; }
  fft(re_.data(), im_.data(), kN, false);
  const int bins = kN / 2 + 1;
  // 0 -> gentle, 1 -> aggressive: how far above the noise floor a bin must be
  // to survive, and how much of it is left when it does not.
  const float over = 1.f + 2.5f * intensity_;
  const float floorG = std::max(0.02f, 0.30f * (1.f - intensity_));
  for (int k = 0; k < bins; k++) {
    float mag = std::sqrt(re_[k] * re_[k] + im_[k] * im_[k]);
    float& n = noise_[k];
    if (!ready_) n = mag;
    else if (mag < n) n += 0.30f * (mag - n);       // follow a quieter floor quickly
    else n += 0.0006f * (mag - n);                  // creep up (speech must not become the floor)
    float g = mag > 1e-9f ? (mag - over * n) / mag : 0.f;
    g = std::clamp(g, floorG, 1.f);
    gain_[k] = mixf(gain_[k], g, 0.5f);             // over time: no chattering bins
  }
  // Smooth across neighbours too: isolated surviving bins are what "musical
  // noise" is made of.
  float prev = gain_[0];
  for (int k = 1; k < bins - 1; k++) {
    float sm = 0.25f * prev + 0.5f * gain_[k] + 0.25f * gain_[k + 1];
    prev = gain_[k];
    gain_[k] = sm;
  }
  for (int k = 0; k < bins; k++) {
    re_[k] *= gain_[k]; im_[k] *= gain_[k];
    if (k > 0 && k < kN / 2) { re_[kN - k] = re_[k]; im_[kN - k] = -im_[k]; }
  }
  fft(re_.data(), im_.data(), kN, true);
  // Hann analysis + Hann synthesis at 75% overlap sums to 1.5.
  const float norm = 2.f / 3.f;
  for (int i = 0; i < kN; i++) out_[i] += re_[i] * win_[i] * norm;
  ready_ = true;
  // The oldest hop is complete: queue it and slide the accumulator.
  for (int i = 0; i < kHop; i++) {
    q_[qw_] = out_[i];
    qw_ = (qw_ + 1) % (int)q_.size();
    if (qn_ < (int)q_.size()) qn_++; else qr_ = (qr_ + 1) % (int)q_.size();
  }
  std::memmove(out_.data(), out_.data() + kHop, (kN - kHop) * sizeof(float));
  std::fill(out_.end() - kHop, out_.end(), 0.f);
}

void SpectralDenoise::process(float* x, int n) {
  if (in_.empty()) return;
  for (int i = 0; i < n; i++) {
    in_[kN - kHop + fill_] = x[i];
    if (++fill_ == kHop) {
      frame();
      // Slide the analysis window by one hop; the next hop is written into
      // the space that leaves at the end.
      std::memmove(in_.data(), in_.data() + kHop, (kN - kHop) * sizeof(float));
      fill_ = 0;
    }
    if (qn_ > 0) {
      x[i] = q_[qr_];
      qr_ = (qr_ + 1) % (int)q_.size();
      qn_--;
    } else {
      x[i] = 0;   // cannot happen after reset(), which primes the queue
    }
  }
}

// ---------------------------------------------------------------------------
// Pitch shift

void PitchShifter::init(int rate) {
  // ~43 ms of delay line, whatever the rate: the two read heads walk through it.
  buf_.assign(std::max(512, (int)(rate * 0.0427f)), 0.f);
  reset();
}
void PitchShifter::reset() {
  std::fill(buf_.begin(), buf_.end(), 0.f);
  w_ = 0;
  phase_ = 0;
}
void PitchShifter::process(float* x, int n) {
  if (buf_.empty()) return;
  const float L = (float)buf_.size();
  const float d = (1.f - ratio_) / L;
  for (int i = 0; i < n; i++) {
    buf_[w_] = x[i];
    w_ = (w_ + 1) % buf_.size();
    phase_ += d;
    phase_ -= std::floor(phase_);
    float p2 = phase_ + 0.5f;
    p2 -= std::floor(p2);
    auto tap = [&](float p) {
      float pos = (float)w_ - p * L;
      while (pos < 0) pos += L;
      int i0 = (int)pos;
      float fr = pos - i0;
      int i1 = (i0 + 1) % buf_.size();
      return mixf(buf_[i0 % buf_.size()], buf_[i1], fr);
    };
    // sin() crossfade: each head is silent exactly where it wraps.
    x[i] = std::sin(kPi * phase_) * tap(phase_) + std::sin(kPi * p2) * tap(p2);
  }
}

// ---------------------------------------------------------------------------
// Reverb / echo

void Space::setLine(Line& l, float ms, float fb) {
  size_t n = std::max<size_t>(16, (size_t)(rate_ * ms * 0.001f));
  if (n > l.buf.size()) n = l.buf.size();   // init() sized it for the longest preset
  l.len = std::max<size_t>(16, n);
  l.idx = 0;
  l.fb = fb;
  std::fill(l.buf.begin(), l.buf.end(), 0.f);
}

void Space::init(int rate) {
  rate_ = rate;
  // Every line is allocated once here, for the longest preset that uses it
  // (cathedral's combs, the echo delay), so configure() — which runs on the
  // audio thread whenever a setting changes — never touches the heap.
  const size_t comb = (size_t)(rate_ * 0.110f) + 16, all = (size_t)(rate_ * 0.010f) + 16, echo = (size_t)(rate_ * 0.400f) + 16;
  for (int i = 0; i < 4; i++) comb_[i].buf.assign(comb, 0.f);
  all_[0].buf.assign(all, 0.f);
  all_[1].buf.assign(all, 0.f);
  echo_.buf.assign(echo, 0.f);
  configure(kind_);
}

void Space::reset() {
  for (auto* l : { &comb_[0], &comb_[1], &comb_[2], &comb_[3], &all_[0], &all_[1], &echo_ }) {
    std::fill(l->buf.begin(), l->buf.end(), 0.f);
    l->idx = 0;
  }
  lp_[0] = lp_[1] = lp_[2] = lp_[3] = 0;
}

void Space::configure(const std::string& kind) {
  kind_ = kind;
  lfo_ = 0;
  trim_ = 1.f;
  if (kind == "underwater") {
    setLine(echo_, 90, 0.45f);
    lid_.lowPass(rate_, 900.f, 0.7f);
    lfoStep_ = 2 * kPi * 0.7f / rate_;   // 0.7 Hz wander
    wobble_ = 0.35f;                     // of the delay length
    mix_ = 0.85f;
    trim_ = 2.2f;   // most of the voice went into the lid; put the level back
    return;
  }
  if (kind == "echo") {
    setLine(echo_, 320, 0.38f);
    mix_ = 0.38f;
    trim_ = 1.2f;
    return;
  }
  bool hall = kind == "hall", cathedral = kind == "cathedral";
  const float scale = cathedral ? 2.3f : hall ? 1.6f : 1.f;
  const float fb = cathedral ? 0.91f : hall ? 0.84f : 0.70f;
  const float combMs[4] = { 29.7f, 37.1f, 41.1f, 43.7f };
  for (int i = 0; i < 4; i++) setLine(comb_[i], combMs[i] * scale, fb);
  setLine(all_[0], 5.0f, 0.5f);
  setLine(all_[1], 1.7f, 0.5f);
  setLine(echo_, 10, 0.f);
  damp_ = cathedral ? 0.45f : hall ? 0.35f : 0.25f;
  // Anything we do not know is a passthrough, not a surprise room.
  bool known = kind == "room" || hall || cathedral;   // (echo and underwater returned above)
  mix_ = !known ? 0.f : (cathedral ? 0.42f : hall ? 0.32f : 0.20f);
  // Mixing wet against dry costs level; hand it back so picking a room is not
  // also picking "quieter".
  trim_ = !known ? 1.f : (cathedral ? 1.35f : hall ? 1.35f : 1.2f);
}

void Space::process(float* x, int n) {
  if (kind_ == "none" || mix_ <= 0 || comb_[0].len > comb_[0].buf.size() || echo_.len > echo_.buf.size()) return;
  if (kind_ == "underwater") {
    const float half = echo_.len * 0.5f;
    for (int i = 0; i < n; i++) {
      lfo_ += lfoStep_;
      if (lfo_ > 2 * kPi) lfo_ -= 2 * kPi;
      // Read the line at a wandering distance behind the write head: the
      // moving delay is what bends the pitch about, like a wobbly tape.
      float back = half * (1.f + wobble_ * std::sin(lfo_));
      float pos = (float)echo_.idx - back;
      while (pos < 0) pos += (float)echo_.len;
      size_t i0 = (size_t)pos % echo_.len, i1 = (i0 + 1) % echo_.len;
      float fr = pos - std::floor(pos);
      float d = mixf(echo_.buf[i0], echo_.buf[i1], fr);
      echo_.buf[echo_.idx] = x[i] + d * echo_.fb;
      echo_.idx = (echo_.idx + 1) % echo_.len;
      x[i] = mixf(x[i], lid_.run(d), mix_) * trim_;
    }
    return;
  }
  if (kind_ == "echo") {
    for (int i = 0; i < n; i++) {
      float d = echo_.buf[echo_.idx];
      echo_.buf[echo_.idx] = x[i] + d * echo_.fb;
      echo_.idx = (echo_.idx + 1) % echo_.len;
      x[i] = (x[i] * (1 - mix_ * 0.5f) + d * mix_) * trim_;
    }
    return;
  }
  for (int i = 0; i < n; i++) {
    float in = x[i] * 0.25f, acc = 0;
    for (int c = 0; c < 4; c++) {
      Line& l = comb_[c];
      float y = l.buf[l.idx];
      lp_[c] = mixf(y, lp_[c], damp_);          // damped feedback: a room, not a spring
      l.buf[l.idx] = in + lp_[c] * l.fb;
      l.idx = (l.idx + 1) % l.len;
      acc += y;
    }
    for (int a = 0; a < 2; a++) {
      Line& l = all_[a];
      float y = l.buf[l.idx];
      float v = acc + y * -l.fb;
      l.buf[l.idx] = v;
      l.idx = (l.idx + 1) % l.len;
      acc = y + v * l.fb;
    }
    x[i] = mixf(x[i], acc, mix_) * trim_;
  }
}

// ---------------------------------------------------------------------------
// The chain

void MicProcessor::setRate(int rate) {
  if (rate <= 0) return;
  if (inited_ && rate == rate_) return;
  rate_ = rate;
  inited_ = true;
  denoise_.init(rate_);
  pitch_.init(rate_);
  space_.init(rate_);
  dirty_ = true;
  reset();
}

void MicProcessor::setSettings(const MicSettings& s) {
  if (s == s_) return;
  bool spaceChanged = s.space != s_.space;
  // Anything that holds audio (the echo line, the reverb combs, the pitch
  // delay, the denoiser's overlap-add queue) freezes while it is bypassed and
  // would play that stale audio out when switched back on — a minute later, in
  // the middle of a call. Emptying on every change that bypasses a stage is
  // cheap and is the same rule mute already follows.
  bool tailChanged = spaceChanged || s.voice != s_.voice || s.enabled != s_.enabled ||
                     s.voiceIsolation != s_.voiceIsolation;
  s_ = s;
  dirty_ = true;
  if (spaceChanged) space_.configure(s_.space);
  if (tailChanged) reset();
}

void MicProcessor::reset() {
  for (int i = 0; i < kHum; i++) hum_[i].reset();
  hp_.reset(); tone1_.reset(); tone2_.reset(); tone3_.reset(); voiceHp_.reset(); voiceLp_.reset();
  denoise_.reset();
  pitch_.reset();
  space_.reset();
  gateEnv_ = 0; gateGain_ = 0; compEnv_ = 0; compGain_ = 1;
  essHi_ = 0; essAll_ = 0; essGain_ = 1;
  ringPhase_ = 0;
  inHold_ = outHold_ = 0;
  inLevel_ = 0; outLevel_ = 0;
}

// Re-designs every filter from the current settings. Cheap and only run when
// something changed.
void MicProcessor::rebuild() {
  dirty_ = false;
  const float r = (float)rate_;
  hp_.highPass(r, 80.f);

  tone1_.bypass(); tone2_.bypass(); tone3_.bypass();
  if (s_.tone == "warm") { tone1_.lowShelf(r, 250.f, 4.f); tone2_.highShelf(r, 6000.f, -3.f); }
  else if (s_.tone == "bright") { tone1_.highShelf(r, 4000.f, 5.f); tone2_.lowShelf(r, 150.f, -2.f); }
  else if (s_.tone == "clarity") { tone1_.peak(r, 3000.f, 1.1f, 5.f); tone2_.lowShelf(r, 180.f, -3.f); tone3_.peak(r, 700.f, 1.2f, -3.f); }
  else if (s_.tone == "podcast") { tone1_.lowShelf(r, 120.f, 3.f); tone2_.peak(r, 2600.f, 1.0f, 4.f); tone3_.highShelf(r, 7500.f, -3.f); }
  else if (s_.tone == "telephone") { tone1_.highPass(r, 400.f, 0.8f); tone2_.lowPass(r, 3200.f, 0.8f); tone3_.peak(r, 1800.f, 1.0f, 4.f); }

  voiceHp_.bypass(); voiceLp_.bypass();
  pitchOn_ = false;
  ringHz_ = 0; ringMix_ = 0; drive_ = 0; driveOut_ = 1; outTrim_ = 1;
  const std::string& v = s_.voice;
  if (v == "deep") { pitchOn_ = true; pitch_.setRatio(0.76f); }
  else if (v == "chipmunk") { pitchOn_ = true; pitch_.setRatio(1.55f); }
  else if (v == "robot") { ringHz_ = 55.f; ringMix_ = 1.f; outTrim_ = 1.4f; }
  else if (v == "alien") { pitchOn_ = true; pitch_.setRatio(1.22f); ringHz_ = 330.f; ringMix_ = 0.75f; outTrim_ = 1.3f; }
  // A voice change should not also be a 15 dB level change: these trims keep
  // every voice within a couple of dB of the one before it.
  else if (v == "megaphone") { voiceHp_.highPass(r, 600.f, 0.9f); voiceLp_.lowPass(r, 3600.f, 0.9f); drive_ = 6.f; driveOut_ = 0.22f; }
  // Drive multiplies quiet parts by roughly 0.8 * drive; monster takes that
  // back out so it is not twice the loudness of every other voice.
  else if (v == "monster") { pitchOn_ = true; pitch_.setRatio(0.62f); voiceLp_.lowPass(r, 4200.f, 0.8f); drive_ = 3.f; driveOut_ = 0.5f; }

  // 50/60 and their first harmonic only: a notch at 150 or 180 Hz would sit
  // right on a low speaking voice, and that is where hum has the least energy.
  const float humF[kHum] = { 50.f, 100.f, 60.f, 120.f };
  // Q 35 was a 1.4 Hz notch: the mains wanders further than that in an hour,
  // and a float biquad cannot hold a null that narrow anyway (measured -26 dB
  // where the design says -200). Q 12 is ~4 Hz wide and actually lands on the
  // hum.
  for (int i = 0; i < kHum; i++) hum_[i].notch(r, humF[i], 12.f);

  essLp_.lowPass(r, 5500.f, 0.707f);   // de-esser split: everything above this is "s"

  outGain_ = dbToLin((std::clamp(s_.volume, 0.f, 1.f) - 0.5f) * 36.f);
}

void MicProcessor::process(float* x, int n) {
  if (n <= 0) return;
  if (!inited_) setRate(rate_);   // no format callback yet: size the buffers now
  if (dirty_) rebuild();

#if defined(__x86_64__) || defined(__i386__)
  // Everything here decays exponentially and never quite reaches zero (the
  // reverb combs worst of all). Without this, a few seconds of silence fills
  // the delay lines with denormals and every sample after that takes a
  // microcode assist — in the one callback that must not overrun.
  unsigned mxcsr = _mm_getcsr();
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
  _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif

  float inPeak = 0;
  for (int i = 0; i < n; i++) {
    // A single wild sample used to live on in the filter memories: inf and NaN
    // for ever, and a merely huge value (1e19 is an ordinary bit pattern for
    // uninitialised memory) as ten seconds of full-scale noise out of the
    // narrow notches. Nothing above full scale is real audio.
    if (!std::isfinite(x[i])) x[i] = 0;
    else if (x[i] > 4.f) x[i] = 4.f;
    else if (x[i] < -4.f) x[i] = -4.f;
    inPeak = std::max(inPeak, std::fabs(x[i]));
  }

  if (s_.enabled) {
    if (s_.highPass) for (int i = 0; i < n; i++) x[i] = hp_.run(x[i]);
    if (s_.humFilter)
      for (int i = 0; i < n; i++) {
        float y = x[i];
        for (int h = 0; h < kHum; h++) y = hum_[h].run(y);
        x[i] = y;
      }
    if (s_.voiceIsolation) {
      denoise_.setIntensity(std::clamp(s_.voiceIsolationIntensity, 0.f, 1.f));
      denoise_.process(x, n);
    }
    if (s_.noiseGate) {
      const float att = coefMs((float)rate_, 1.f), rel = coefMs((float)rate_, 80.f);
      const float gAtt = coefMs((float)rate_, 3.f), gRel = coefMs((float)rate_, 120.f);
      // A real room sits around -35 dBFS, so a range topping out at -26 dB
      // meant the default never closed. -60..-15 dB puts the useful part of
      // the slider in the middle.
      const float open = dbToLin(mixf(-60.f, -15.f, std::clamp(s_.noiseGateIntensity, 0.f, 1.f)));
      const float close = open * 0.5f;   // 6 dB of hysteresis
      for (int i = 0; i < n; i++) {
        float a = std::fabs(x[i]);
        gateEnv_ += (a > gateEnv_ ? att : rel) * (a - gateEnv_);
        float want = gateEnv_ > (gateGain_ > 0.5f ? close : open) ? 1.f : 0.f;
        gateGain_ += (want > gateGain_ ? gAtt : gRel) * (want - gateGain_);
        x[i] *= gateGain_;
      }
    }
    if (s_.autoLevel) {
      const float t = std::clamp(s_.autoLevelIntensity, 0.f, 1.f);
      const float thrDb = mixf(-14.f, -32.f, t), ratio = mixf(2.f, 6.f, t);
      const float makeup = dbToLin(-thrDb * (1.f - 1.f / ratio) * 0.85f);
      const float att = coefMs((float)rate_, 6.f), rel = coefMs((float)rate_, 140.f);
      const float makeupDb = linToDb(makeup);
      for (int i = 0; i < n; i++) {
        float a = std::fabs(x[i]);
        compEnv_ += (a > compEnv_ ? att : rel) * (a - compEnv_);
        float envDb = linToDb(compEnv_);
        float overDb = envDb - thrDb;
        float grDb = overDb > 0 ? -overDb * (1.f - 1.f / ratio) : 0.f;
        // Make-up gain belongs to the voice, not to the room: it fades in
        // between -45 and -30 dBFS, so "even loudness" cannot turn into a
        // 16 dB louder fan. (Nothing else brings the floor back down: the
        // noise gate is a separate switch and off by default.)
        float lift = std::clamp((envDb + 45.f) / 15.f, 0.f, 1.f);
        float want = dbToLin(grDb + makeupDb * lift);
        compGain_ += (want < compGain_ ? att : rel) * (want - compGain_);
        x[i] *= compGain_;
      }
    }
    if (s_.deEsser) {
      // Split at 5.5 kHz (lo + hi is exactly the input, so an idle de-esser
      // changes nothing) and turn the top down whenever it carries more of the
      // sound than speech ever does — which is what an "s" is.
      const float att = coefMs((float)rate_, 1.f), rel = coefMs((float)rate_, 45.f);
      const float share = mixf(0.55f, 0.18f, std::clamp(s_.deEsserIntensity, 0.f, 1.f));
      for (int i = 0; i < n; i++) {
        float lo = essLp_.run(x[i]), hi = x[i] - lo;
        float ah = std::fabs(hi), af = std::fabs(x[i]);
        essHi_ += (ah > essHi_ ? att : rel) * (ah - essHi_);
        essAll_ += (af > essAll_ ? att : rel) * (af - essAll_);
        // Below this the high band is hiss, not sibilance: a ratio test alone
        // would duck a quiet bright microphone's whole top end, permanently.
        float allow = share * essAll_ + 1e-6f;
        float g = (essHi_ > allow && essHi_ > 0.02f) ? allow / essHi_ : 1.f;
        if (g < 0.25f) g = 0.25f;   // 12 dB is plenty; more sounds lisped
        essGain_ += (g < essGain_ ? att : rel) * (g - essGain_);
        x[i] = lo + hi * essGain_;
      }
    }
    if (s_.tone != "none") for (int i = 0; i < n; i++) x[i] = tone3_.run(tone2_.run(tone1_.run(x[i])));
    if (s_.voice != "none") {
      if (pitchOn_) pitch_.process(x, n);
      if (ringHz_ > 0) {  // robot / alien: ring modulation
        const float step = 2 * kPi * ringHz_ / rate_;
        for (int i = 0; i < n; i++) {
          ringPhase_ += step;
          if (ringPhase_ > 2 * kPi) ringPhase_ -= 2 * kPi;
          float m = std::sin(ringPhase_);
          x[i] = mixf(x[i], x[i] * m, ringMix_);
        }
      }
      if (drive_ > 0) {   // megaphone / monster: band-limited soft clipping
        const float norm = 1.f / std::tanh(drive_);
        for (int i = 0; i < n; i++) x[i] = std::tanh(voiceLp_.run(voiceHp_.run(x[i])) * drive_) * norm * 0.8f * driveOut_;
      }
    }
    if (s_.space != "none") space_.process(x, n);
  }

  float outPeak = 0;
  for (int i = 0; i < n; i++) {
    // Volume is the microphone's own gain, not an effect: it applies even with
    // the master switch off (that is the only knob left in that case).
    float y = x[i] * outGain_ * outTrim_;
    // Gentle soft limiter so an effect (or the makeup gain) can never clip hard.
    float a = std::fabs(y);
    if (a > 0.8f) y = std::copysign(0.8f + 0.2f * std::tanh((a - 0.8f) / 0.2f), y);
    if (!std::isfinite(y)) y = 0;
    x[i] = y;
    outPeak = std::max(outPeak, std::fabs(y));
  }

  // Meter: jump to a new peak, fall back slowly so the panel can show it.
  const float fall = std::pow(0.75f, (float)n / 1024.f);
  inHold_ = std::max(inPeak, inHold_ * fall);
  outHold_ = std::max(outPeak, outHold_ * fall);
  inLevel_ = std::min(1.f, inHold_);
  outLevel_ = std::min(1.f, outHold_);

#if defined(__x86_64__) || defined(__i386__)
  _mm_setcsr(mxcsr);   // the callback borrowed the thread; hand it back as found
#endif
}
