#pragma once
#include <atomic>
#include <string>
#include <vector>

// Microphone effects: the audio half of Camera Effects. Everything here is
// plain DSP on a mono float stream; the PipeWire plumbing (which mic is read,
// which node apps see) lives in audio.cpp, the way capture.cpp/pwout.cpp do
// for video.
//
// The chain, in order: rumble filter -> voice isolation (spectral denoise) ->
// noise gate -> auto level (compressor) -> de-esser -> tone (EQ preset) ->
// voice (fun changer) -> space (reverb/echo) -> volume + limiter. Every effect is bypassed
// when it is off; volume is the microphone's own gain and applies even with the
// master switch off, so at 0 dB with nothing on this is a passthrough.

struct MicSettings {
  bool enabled = true;                 // master switch; off = plain mic (volume still applies)
  bool voiceIsolation = false;         // spectral noise suppression (fans, keyboards, street)
  float voiceIsolationIntensity = 0.6f;
  bool noiseGate = false;              // silence between sentences
  float noiseGateIntensity = 0.55f;    // 0 = barely closes, 1 = aggressive
  bool autoLevel = false;              // compressor + makeup gain: even loudness
  float autoLevelIntensity = 0.6f;
  bool deEsser = false;                // tames "s" and "t" without dulling the rest
  float deEsserIntensity = 0.5f;
  bool highPass = true;                // 80 Hz rumble/handling filter
  bool humFilter = false;              // narrow notches on mains hum (50/60 Hz and harmonics)
  float volume = 0.5f;                 // 0..1 mapped to -18..+18 dB (0.5 = 0 dB); always applied
  std::string tone = "none";           // EQ preset, one of toneNames()
  std::string voice = "none";          // fun voice changer, one of voiceNames()
  std::string space = "none";          // reverb / echo, one of spaceNames()

  bool operator==(const MicSettings& o) const {
    return enabled == o.enabled && voiceIsolation == o.voiceIsolation && voiceIsolationIntensity == o.voiceIsolationIntensity &&
           noiseGate == o.noiseGate && noiseGateIntensity == o.noiseGateIntensity && autoLevel == o.autoLevel &&
           autoLevelIntensity == o.autoLevelIntensity && deEsser == o.deEsser && deEsserIntensity == o.deEsserIntensity &&
           highPass == o.highPass && humFilter == o.humFilter && volume == o.volume &&
           tone == o.tone && voice == o.voice && space == o.space;
  }
  bool operator!=(const MicSettings& o) const { return !(*this == o); }
  // Valid values ("none" first in each).
  static const std::vector<std::string>& toneNames();
  static const std::vector<std::string>& voiceNames();
  static const std::vector<std::string>& spaceNames();
};

// ---------------------------------------------------------------------------
// Building blocks

// Direct-form-2 transposed biquad, with the usual RBJ designers.
struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  float z1 = 0, z2 = 0;
  inline float run(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
  void reset() { z1 = z2 = 0; }
  void bypass() { b0 = 1; b1 = b2 = a1 = a2 = 0; reset(); }
  void highPass(float rate, float f, float q = 0.707f);
  void bandPass(float rate, float f, float q);   // constant 0 dB peak gain
  void lowPass(float rate, float f, float q = 0.707f);
  void peak(float rate, float f, float q, float gainDb);
  void notch(float rate, float f, float q);
  void lowShelf(float rate, float f, float gainDb);
  void highShelf(float rate, float f, float gainDb);
};

// Spectral noise suppression: 2048-point FFT, 75% overlap, per-bin noise floor
// tracked with min-statistics and a Wiener-style gain. The frame has to be
// long enough to resolve the harmonics of a voice (85-255 Hz apart): at 512
// points the bins are 94 Hz wide, speech and noise land in the same bin, and
// measuring the result showed the "intensity" slider making the
// signal-to-noise ratio slightly *worse*. 2048 points (23 Hz bins) is +3 dB
// and improves with intensity, at 32 ms of latency while it is on.
class SpectralDenoise {
public:
  void init(int rate);
  void reset();
  void setIntensity(float v) { intensity_ = v; }
  // Processes n samples in place through the overlap-add buffer.
  void process(float* x, int n);

private:
  void frame();  // one analysis/synthesis frame from in_
  static constexpr int kN = 2048;
  static constexpr int kHop = 512;
  int rate_ = 48000;
  float intensity_ = 0.6f;
  std::vector<float> win_, in_, out_, re_, im_, noise_, gain_;
  std::vector<float> q_;   // finished samples waiting to be handed back
  int qr_ = 0, qw_ = 0, qn_ = 0;
  int fill_ = 0;      // samples of the newest hop staged into in_
  bool ready_ = false;
};

// Two-head crossfaded delay line: cheap granular pitch shift.
class PitchShifter {
public:
  void init(int rate);
  void reset();
  void setRatio(float r) { ratio_ = r; }
  void process(float* x, int n);

private:
  std::vector<float> buf_;
  size_t w_ = 0;
  float phase_ = 0, ratio_ = 1;
};

// Schroeder reverb (4 combs + 2 allpass) and a feedback delay, sharing the
// same "space" stage.
class Space {
public:
  void init(int rate);
  void reset();
  void configure(const std::string& kind);   // none | room | hall | cathedral | echo | underwater
  void process(float* x, int n);

private:
  // Every line is allocated once, for the longest preset; configure() only
  // moves `len` inside it. Nothing here may allocate: it runs on the audio
  // thread, from setSettings.
  struct Line { std::vector<float> buf; size_t len = 16, idx = 0; float fb = 0; };
  void setLine(Line& l, float ms, float fb);
  int rate_ = 48000;
  std::string kind_ = "none";
  Line comb_[4], all_[2], echo_;
  float mix_ = 0, damp_ = 0, trim_ = 1, lp_[4] = { 0, 0, 0, 0 };
  // Underwater: the echo line read through a slowly wandering tap, plus a lid
  // on the top end.
  Biquad lid_;
  float lfo_ = 0, lfoStep_ = 0, wobble_ = 0;
};

// ---------------------------------------------------------------------------

// The whole chain. Owned and driven by the PipeWire capture callback in
// audio.cpp: setRate/setSettings are called from that same thread.
class MicProcessor {
public:
  void setRate(int rate);
  void setSettings(const MicSettings& s);
  void reset();
  // Mono, in place. Also updates the levels below.
  void process(float* x, int n);
  // Peak levels of the last blocks, 0..1, for the panel's meter (any thread).
  float inLevel() const { return inLevel_.load(); }
  float outLevel() const { return outLevel_.load(); }
  void clearLevels() { inHold_ = outHold_ = 0; inLevel_ = 0; outLevel_ = 0; }

private:
  void rebuild();   // re-design the filters after a rate or settings change

  MicSettings s_;
  int rate_ = 48000;
  bool dirty_ = true;
  bool inited_ = false;   // the buffers below are sized for rate_
  Biquad hp_, tone1_, tone2_, tone3_, voiceHp_, voiceLp_, essLp_;
  // Mains hum: 50 and 60 Hz plus two harmonics each, narrow enough to leave
  // even a deep voice alone. Both mains frequencies are notched, so nobody has
  // to know which country's wiring their buzz comes from.
  static constexpr int kHum = 4;
  Biquad hum_[kHum];
  SpectralDenoise denoise_;
  PitchShifter pitch_;
  Space space_;
  float gateEnv_ = 0, gateGain_ = 0;      // envelope follower and the gate's current gain
  float compEnv_ = 0, compGain_ = 1;
  float essHi_ = 0, essAll_ = 0, essGain_ = 1;   // de-esser: high band, whole band, current cut
  float ringPhase_ = 0, ringHz_ = 0, ringMix_ = 0;
  float drive_ = 0, driveOut_ = 1;         // >0: soft-clip drive (megaphone, monster) and its make-down
  float outTrim_ = 1;                      // per-voice / per-space level match
  float outGain_ = 1;
  bool pitchOn_ = false;
  std::atomic<float> inLevel_{0}, outLevel_{0};
  // Level meters decay so a short peak stays readable in the panel.
  float inHold_ = 0, outHold_ = 0;
};
