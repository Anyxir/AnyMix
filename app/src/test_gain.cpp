#include "testsupport.h"

#include "anymix/Process.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {

using anymix::kBuses;
using anymix::kBlockFrames;
using anymix::kChannels;
using anymix::kMic1;
using anymix::kSampleRate;
using anymix::kStrips;

// Local helpers, same minimal style as the ones in selftest.cpp; kept local to
// this TU on purpose (the brief forbids sharing them through selftest.cpp).
float Rms(const float* data, int n) {
  // Accumulated in double: the signal path is float32, so a double sum adds
  // only ~1e-7 of measurement noise, while a float sum over 1024 samples could
  // perturb the measured ratio enough to blur a 1e-3 tolerance.
  double sum = 0.0;
  for (int i = 0; i < n; ++i) {
    sum += static_cast<double>(data[i]) * data[i];
  }
  return static_cast<float>(std::sqrt(sum / n));
}

void FillSine(float* dst, int frames, float freq, float amp, float& phase) {
  const float step = 2.f * 3.14159265f * freq / static_cast<float>(kSampleRate);
  for (int i = 0; i < frames; ++i) {
    const float s = amp * std::sin(phase);
    phase += step;
    if (phase > 6.2831853f) {
      phase -= 6.2831853f;
    }
    dst[i * 2] = s;
    dst[i * 2 + 1] = s;
  }
}

// The real ShapeStrips + MixToBuses path for one strip: kMic1 feeds bus A1 with
// a flat EQ (MakeLowShelf(0,..)/MakeHighShelf(0,..) return identity), mono off,
// pan 0, route 1u. Every other strip has no input and no route.
struct GainPath {
  float input[kBlockFrames * kChannels]{};
  float shapedStore[kStrips][kBlockFrames * kChannels]{};
  float busStore[kBuses][kBlockFrames * kChannels]{};
  float* shaped[kStrips];
  float* buses[kBuses];
  const float* inputs[kStrips];
  anymix::BlockParams params[kStrips]{};
  anymix::StripEq eq[kStrips]{};

  GainPath() {
    for (int i = 0; i < kStrips; ++i) {
      shaped[i] = shapedStore[i];
      inputs[i] = nullptr;
    }
    for (int i = 0; i < kBuses; ++i) {
      buses[i] = busStore[i];
    }
    anymix::BlockParams& p = params[kMic1];
    p.gain = 1.f;
    p.audible = true;
    p.mono = false;
    p.pan = 0.f;
    p.route = 1u;
    p.bass = anymix::MakeLowShelf(0.f, 150.f, static_cast<float>(kSampleRate));
    p.treble = anymix::MakeHighShelf(0.f, 6000.f, static_cast<float>(kSampleRate));
  }

  // Fresh strip state for every measurement: characterization must observe the
  // steady-state gain law, not a smoothing transient. The fader ramps live in
  // the engine (outside StripEq), so they are reset explicitly too.
  void ResetEq() {
    for (int i = 0; i < kStrips; ++i) {
      eq[i] = anymix::StripEq{};
    }
    anymix::ResetStripGainRamps();
  }

  void Process() {
    inputs[kMic1] = input;
    anymix::ShapeStrips(inputs, kBlockFrames, params, eq, shaped);
    inputs[kMic1] = nullptr;
    anymix::MixToBuses(shaped, kBlockFrames, params, buses);
  }
};

// Max adjacent sample delta of a steady sine at the given amplitude; the
// per-sample slew a continuous wave of that amplitude can ever produce.
float MaxAdjacentDelta(float amp, float freq) {
  constexpr int kWindow = kBlockFrames * 4;
  float buf[kWindow * kChannels];
  float phase = 0.f;
  FillSine(buf, kWindow, freq, amp, phase);
  float maxDelta = 0.f;
  for (int i = 1; i < kWindow * kChannels; ++i) {
    maxDelta = std::fmax(maxDelta, std::fabs(buf[i] - buf[i - 1]));
  }
  return maxDelta;
}

// Max adjacent delta inside an interleaved block, including the boundary
// against the previous block's last sample (left and right).
float MaxBlockDelta(const float* block, float prevL, float prevR) {
  float maxDelta = std::fmax(std::fabs(block[0] - prevL), std::fabs(block[1] - prevR));
  for (int i = 2; i < kBlockFrames * kChannels; ++i) {
    maxDelta = std::fmax(maxDelta, std::fabs(block[i] - block[i - 1]));
  }
  return maxDelta;
}

// Phase A — characterization of the gain law as it behaves today. All of these
// must pass on unmodified code; a failure here is a real engine bug.

// A1: amplitude ratio law. At 1 kHz, amplitude 0.5, the measured RMS ratio
// out/in must equal 10^(dB/20) within relative 1e-3. Justification: the path
// is float32 (relative error ~1e-7 per multiply), the RMS is accumulated in
// double, and flat shelves are bit-exact identity, so 1e-3 is ~4 orders above
// the float noise while staying far below any real gain bug (a double-applied
// gain is ~100% off, a hidden EQ compensation is several percent).
void TestAmplitudeRatioLaw() {
  GainPath path;
  const float dbs[] = {-12.f, -6.f, 0.f, 3.f, 6.f, 12.f};
  for (const float db : dbs) {
    const float target = std::pow(10.f, db / 20.f);
    path.ResetEq();
    float phase = 0.f;
    FillSine(path.input, kBlockFrames, 1000.f, 0.5f, phase);
    path.params[kMic1].gain = target;
    path.Process();
    const float inRms = Rms(path.input, kBlockFrames * kChannels);
    const float outRms = Rms(path.busStore[0], kBlockFrames * kChannels);
    const double ratio = static_cast<double>(outRms) / static_cast<double>(inRms);
    std::printf("MEASURE gain %+0.0f dB ratio %.5f\n", db, ratio);
    char name[96];
    std::snprintf(name, sizeof(name), "1 kHz ratio at %+0.0f dB equals 10^(dB/20) within 1e-3", db);
    Expect(std::fabs(ratio - target) <= 1e-3 * target, name);
  }
}

// A2: the same law at input amplitudes 0.1, 0.3 and 0.75 — the fader must be
// linear, so the ratio may not depend on the signal level.
void TestMultiAmplitudeRatioLaw() {
  GainPath path;
  const float amps[] = {0.1f, 0.3f, 0.75f};
  const float dbs[] = {0.f, -12.f, 12.f};
  for (const float amp : amps) {
    for (const float db : dbs) {
      const float target = std::pow(10.f, db / 20.f);
      path.ResetEq();
      float phase = 0.f;
      FillSine(path.input, kBlockFrames, 1000.f, amp, phase);
      path.params[kMic1].gain = target;
      path.Process();
      const float inRms = Rms(path.input, kBlockFrames * kChannels);
      const float outRms = Rms(path.busStore[0], kBlockFrames * kChannels);
      const double ratio = static_cast<double>(outRms) / static_cast<double>(inRms);
      std::printf("MEASURE gain %+0.0f dB amp %.2f ratio %.5f\n", db, amp, ratio);
      char name[96];
      std::snprintf(name, sizeof(name), "ratio at %+0.0f dB and amplitude %.2f stays on the gain law",
                    db, amp);
      Expect(std::fabs(ratio - target) <= 1e-3 * target, name);
    }
  }
}

// A3: no double gain. At +6 dB the true gain is G = 10^(6/20) = 1.9953. The
// ratio must equal G (within 1e-3) and must be nowhere near G*G: ShapeStrips
// multiplies p.gain exactly once per sample, and mic strips take their gain at
// capture and pass 1.f here, so a second application anywhere on the path
// would show up as G squared = 3.981.
void TestNoDoubleGain() {
  GainPath path;
  const float target = std::pow(10.f, 6.f / 20.f);
  path.ResetEq();
  float phase = 0.f;
  FillSine(path.input, kBlockFrames, 1000.f, 0.5f, phase);
  path.params[kMic1].gain = target;
  path.Process();
  const float inRms = Rms(path.input, kBlockFrames * kChannels);
  const float outRms = Rms(path.busStore[0], kBlockFrames * kChannels);
  const double ratio = static_cast<double>(outRms) / static_cast<double>(inRms);
  std::printf("MEASURE gain %+0.0f dB ratio %.5f squared %.5f\n", 6.f, ratio, target * target);
  Expect(std::fabs(ratio - target) < 1e-3, "+6 dB applies the gain exactly once");
  Expect(std::fabs(ratio - target * target) > 0.1, "+6 dB is not the gain squared");
}

// A4: no hidden attenuation at 0 dB. With unity fader and flat EQ the output
// must equal the input in peak and RMS (this pins the removed toneTrim /
// toneGain compensation, which used to scale the signal down here).
void TestNoHiddenAttenuationAtUnity() {
  GainPath path;
  path.ResetEq();
  float phase = 0.f;
  FillSine(path.input, kBlockFrames, 1000.f, 0.5f, phase);
  path.params[kMic1].gain = 1.f;
  path.Process();
  float peakIn = 0.f;
  float peakOut = 0.f;
  for (int i = 0; i < kBlockFrames * kChannels; ++i) {
    peakIn = std::fmax(peakIn, std::fabs(path.input[i]));
    peakOut = std::fmax(peakOut, std::fabs(path.busStore[0][i]));
  }
  const float rmsIn = Rms(path.input, kBlockFrames * kChannels);
  const float rmsOut = Rms(path.busStore[0], kBlockFrames * kChannels);
  std::printf("MEASURE 0 dB peak %.5f/%.5f rms %.5f/%.5f\n", peakIn, peakOut, rmsIn, rmsOut);
  Expect(std::fabs(peakOut - peakIn) <= 1e-3 * peakIn, "0 dB leaves the peak untouched");
  Expect(std::fabs(rmsOut - rmsIn) <= 1e-3 * rmsIn, "0 dB leaves the RMS untouched");
}

// Phase B — gain ramp (NEW behaviour). Today p.gain steps instantly between
// blocks, so a fader move produces a sample-value discontinuity at the block
// boundary: the click. B1 and B3 must FAIL on unmodified code (RED). B2 and B4
// are guard tests that the instant path already satisfies; they exist to pin
// the properties the ramp must NOT break (exact convergence, no overshoot, no
// smearing of a constant gain).

// B1: a fader step must not click across the block boundary. The bound is 4x
// the max adjacent delta of the steady sine at the FINAL amplitude: a
// continuous wave of that amplitude can never jump faster than that, and 4x
// gives float-slew headroom — while a gain step (extra delta up to
// (G-1)*amp = 2.98*0.5) bursts straight through it.
void TestGainStepDoesNotClick() {
  GainPath path;
  path.ResetEq();
  const float upGain = std::pow(10.f, 12.f / 20.f);  // +12 dB = 3.981071
  float phase = 0.f;
  path.params[kMic1].gain = 1.f;
  FillSine(path.input, kBlockFrames, 1000.f, 0.5f, phase);
  path.Process();
  const float lastL = path.busStore[0][(kBlockFrames - 1) * 2];
  const float lastR = path.busStore[0][(kBlockFrames - 1) * 2 + 1];
  // Block N+1: SAME StripEq state, fader moved to +12 dB. The sine keeps its
  // phase (it is not restarted), so the only possible discontinuity in the
  // output is the gain change itself.
  path.params[kMic1].gain = upGain;
  FillSine(path.input, kBlockFrames, 1000.f, 0.5f, phase);
  path.Process();
  const float maxDelta = MaxBlockDelta(path.busStore[0], lastL, lastR);
  const float bound = 4.f * MaxAdjacentDelta(0.5f * upGain, 1000.f);
  std::printf("MEASURE click up maxdelta %.5f bound %.5f\n", maxDelta, bound);
  Expect(maxDelta <= bound, "a gain step does not click across the block boundary");
}

// B2: the ramp must converge exactly — no permanent attenuation, no overshoot.
// The settled window is the second half of the step block: by ~256 samples
// (5.3 ms at 48 kHz) the ramp must be done.
void TestGainRampReachesTarget() {
  GainPath path;
  path.ResetEq();
  const float upGain = std::pow(10.f, 12.f / 20.f);
  const float targetAmp = 0.5f * upGain;
  float phase = 0.f;
  path.params[kMic1].gain = 1.f;
  FillSine(path.input, kBlockFrames, 1000.f, 0.5f, phase);
  path.Process();
  path.params[kMic1].gain = upGain;
  FillSine(path.input, kBlockFrames, 1000.f, 0.5f, phase);
  path.Process();  // block N+1 contains the ramp
  float peakAfter = 0.f;
  for (int i = 0; i < kBlockFrames * kChannels; ++i) {
    peakAfter = std::fmax(peakAfter, std::fabs(path.busStore[0][i]));
  }
  const float winOut = Rms(path.busStore[0] + 256 * kChannels, 256 * kChannels);
  const float winIn = Rms(path.input + 256 * kChannels, 256 * kChannels);
  const double settled = static_cast<double>(winOut) / static_cast<double>(winIn);
  FillSine(path.input, kBlockFrames, 1000.f, 0.5f, phase);
  path.Process();  // block N+2: fully steady
  for (int i = 0; i < kBlockFrames * kChannels; ++i) {
    peakAfter = std::fmax(peakAfter, std::fabs(path.busStore[0][i]));
  }
  const double sustained =
      static_cast<double>(Rms(path.busStore[0], kBlockFrames * kChannels)) /
      Rms(path.input, kBlockFrames * kChannels);
  std::printf("MEASURE ramp up settled %.5f sustained %.5f peak %.5f target amp %.5f\n", settled,
              sustained, peakAfter, targetAmp);
  Expect(std::fabs(settled - upGain) <= 2e-4 * upGain, "gain ramp converges within half a block");
  Expect(std::fabs(sustained - upGain) <= 2e-4 * upGain, "gain stays at the target afterwards");
  Expect(peakAfter <= targetAmp * 1.02f, "gain ramp never overshoots the target amplitude");
}

// B3: down-ramp from +12 dB to -6 dB mid-stream — same continuity bound as B1
// and exact convergence as B2. The continuity bound is the same one as B1
// (4x the steady adjacent delta at the highest steady amplitude involved in
// the transition, 1.9905 = the PRE-step level for a down-step; numerically the
// same value as B1's bound). A bound scaled to the lower -6 dB amplitude
// (0.1311) is not attainable by ANY implementation: while the block still
// carries the 1.99-amplitude wave its own adjacent deltas are ~0.26, and the
// instant step (the RED behaviour) bursts to ~1.36.
void TestGainDownRamp() {
  GainPath path;
  path.ResetEq();
  const float upGain = std::pow(10.f, 12.f / 20.f);
  const float downGain = std::pow(10.f, -6.f / 20.f);
  const float targetAmp = 0.5f * downGain;
  float phase = 0.f;
  path.params[kMic1].gain = upGain;
  FillSine(path.input, kBlockFrames, 1000.f, 0.5f, phase);
  path.Process();  // fresh state adopts +12 dB exactly — no ramp on the first block
  const float prevL = path.busStore[0][(kBlockFrames - 1) * 2];
  const float prevR = path.busStore[0][(kBlockFrames - 1) * 2 + 1];
  path.params[kMic1].gain = downGain;
  FillSine(path.input, kBlockFrames, 1000.f, 0.5f, phase);
  path.Process();  // block N+1 contains the down-ramp; sine phase is continuous
  const float maxDelta = MaxBlockDelta(path.busStore[0], prevL, prevR);
  const float bound = 4.f * MaxAdjacentDelta(0.5f * upGain, 1000.f);
  std::printf("MEASURE click down maxdelta %.5f bound %.5f\n", maxDelta, bound);
  Expect(maxDelta <= bound, "a down-ramp does not click across the block boundary");

  const float winOut = Rms(path.busStore[0] + 256 * kChannels, 256 * kChannels);
  const float winIn = Rms(path.input + 256 * kChannels, 256 * kChannels);
  const double settled = static_cast<double>(winOut) / static_cast<double>(winIn);
  std::printf("MEASURE ramp down settled %.5f target %.5f\n", settled, downGain);
  Expect(std::fabs(settled - downGain) <= 2e-4 * downGain, "down-ramp reaches the exact target gain");

  FillSine(path.input, kBlockFrames, 1000.f, 0.5f, phase);
  path.Process();  // block N+2: fully steady
  float peakSteady = 0.f;
  for (int i = 0; i < kBlockFrames * kChannels; ++i) {
    peakSteady = std::fmax(peakSteady, std::fabs(path.busStore[0][i]));
  }
  std::printf("MEASURE ramp down steady peak %.5f target amp %.5f\n", peakSteady, targetAmp);
  Expect(peakSteady <= targetAmp * 1.02f, "down-ramp settles without overshoot");
}

// B4: regression — the ramp must not smear a constant gain: a 0 dB block
// through a fresh StripEq is bit-for-bit unchanged (float == is exact here:
// the flat path computes x*1.0f, then the bus adds 0.0f).
void TestZeroDbBlockIsBitExact() {
  GainPath path;
  path.ResetEq();
  float phase = 0.f;
  FillSine(path.input, kBlockFrames, 1000.f, 0.5f, phase);
  path.params[kMic1].gain = 1.f;
  path.Process();
  bool exact = true;
  for (int i = 0; i < kBlockFrames * kChannels; ++i) {
    if (path.busStore[0][i] != path.input[i]) {
      exact = false;
    }
  }
  Expect(exact, "a 0 dB block through fresh state is bit-for-bit unchanged");
}

// B5 (fix round 1): a non-finite p.gain must not poison the ramp state.
// SetGainDb clamps to the fader floor/ceiling, but ShapeStrips is the API
// boundary and takes a raw float, so NaN/inf is reachable through it. Before
// the guard, a NaN target makes `p.gain != ramp.target` true forever (NaN
// compares unequal to everything), so the strip re-ramps from a NaN applied
// gain and every block comes out NaN - unlike the baseline instant path, which
// self-healed on the next finite block. The contract added here: hold the
// current (finite) target while the published gain is non-finite.
void TestNonFiniteGainHoldsCurrentTarget() {
  GainPath path;
  path.ResetEq();
  float phase = 0.f;

  // Steady unity block through fresh state: pins that a held unity gain is a
  // bit-exact copy of the input (the same property B4 checks), so the NaN
  // block below can be compared against its own input.
  path.params[kMic1].gain = 1.f;
  FillSine(path.input, kBlockFrames, 1000.f, 0.5f, phase);
  path.Process();

  // The NaN block: same sine continuation, fader reports NaN.
  path.params[kMic1].gain = std::numeric_limits<float>::quiet_NaN();
  FillSine(path.input, kBlockFrames, 1000.f, 0.5f, phase);
  path.Process();
  int nonFinite = 0;
  int mismatched = 0;
  for (int i = 0; i < kBlockFrames * kChannels; ++i) {
    if (!std::isfinite(path.busStore[0][i])) {
      ++nonFinite;
    } else if (path.busStore[0][i] != path.input[i]) {
      // A held unity gain must leave the samples bit-exactly untouched, the
      // same property B4 pins for a steady 0 dB block.
      ++mismatched;
    }
  }
  std::printf("MEASURE nonfinite gain block nonfinite %d mismatched %d\n", nonFinite, mismatched);
  Expect(nonFinite == 0, "a non-finite gain block produces no NaN output");
  Expect(mismatched == 0, "a non-finite gain block holds the current gain exactly");

  // A following finite-gain block must behave normally: finite output for the
  // WHOLE block (the poison must not even leak into the ramp-up samples) and a
  // ramp that settles at +12 dB exactly as B2 does after a plain fader move.
  const float upGain = std::pow(10.f, 12.f / 20.f);
  path.params[kMic1].gain = upGain;
  FillSine(path.input, kBlockFrames, 1000.f, 0.5f, phase);
  path.Process();
  int followNonFinite = 0;
  for (int i = 0; i < kBlockFrames * kChannels; ++i) {
    if (!std::isfinite(path.busStore[0][i])) {
      ++followNonFinite;
    }
  }
  const float winOut = Rms(path.busStore[0] + 256 * kChannels, 256 * kChannels);
  const float winIn = Rms(path.input + 256 * kChannels, 256 * kChannels);
  const double settled = static_cast<double>(winOut) / static_cast<double>(winIn);
  std::printf("MEASURE after non-finite settled %.5f target %.5f follow-nonfinite %d\n", settled,
              upGain, followNonFinite);
  Expect(followNonFinite == 0, "a finite gain after a non-finite block stays finite");
  Expect(std::fabs(settled - upGain) <= 2e-4 * upGain,
         "a finite gain after a non-finite block ramps to its target");
}

}  // namespace

void RunGainTests() {
  TestAmplitudeRatioLaw();
  TestMultiAmplitudeRatioLaw();
  TestNoDoubleGain();
  TestNoHiddenAttenuationAtUnity();
  TestGainStepDoesNotClick();
  TestGainRampReachesTarget();
  TestGainDownRamp();
  TestZeroDbBlockIsBitExact();
  TestNonFiniteGainHoldsCurrentTarget();
}
