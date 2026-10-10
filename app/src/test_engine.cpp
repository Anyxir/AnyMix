#include "testsupport.h"

#include "anymix/Process.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

using anymix::GainRamp;
using anymix::kBlockFrames;
using anymix::kChannels;
using anymix::kGainRampSamples;
using anymix::kSampleRate;

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

// C1: ClampFaderDb is the single input gate for the fader dB values that reach
// SetGainDb / SetBusGainDb. The old std::max/std::min pair mapped NaN to the
// +24 dB ceiling (std::min(24, NaN) returns 24): a poisoned fader jumped to
// full blast instead of neutral. Contract: NaN -> 0.f (neutral), +-inf clamps
// to the ceiling/floor, else clamp to [kFaderMinDb, kFaderMaxDb] exactly.
void TestClampFaderDb() {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  const float gotNan = anymix::ClampFaderDb(nan);
  Expect(std::isfinite(gotNan), "ClampFaderDb keeps NaN finite");
  Expect(gotNan == 0.f, "ClampFaderDb maps NaN to the neutral 0 dB");
  Expect(anymix::ClampFaderDb(inf) == anymix::kFaderMaxDb,
         "ClampFaderDb maps +inf to the +24 dB ceiling");
  Expect(anymix::ClampFaderDb(-inf) == anymix::kFaderMinDb,
         "ClampFaderDb maps -inf to the -60 dB floor");
  Expect(anymix::ClampFaderDb(30.f) == anymix::kFaderMaxDb,
         "ClampFaderDb clamps 30 dB to the +24 dB ceiling");
  Expect(anymix::ClampFaderDb(-70.f) == anymix::kFaderMinDb,
         "ClampFaderDb clamps -70 dB to the -60 dB floor");
  Expect(anymix::ClampFaderDb(0.f) == 0.f, "ClampFaderDb keeps 0 dB unchanged");
  Expect(anymix::ClampFaderDb(7.3f) == 7.3f, "ClampFaderDb keeps an interior level unchanged");
}

// --- Bus gain ramp (work item 4) ---------------------------------------------
// The bus loop in Engine.cpp is not reachable headless (it lives inside the
// render thread's Impl::Loop over WASAPI/ASIO device state). Per the brief,
// the bus-application semantics are pinned here through a direct GainRamp unit
// test: a small per-sample loop that replicates exactly what the bus loop now
// does (lazy-adopt the first target, step re-targets from the applied value,
// snap exactly on the last sample, non-finite target holds the previous one),
// driven block-by-block like the engine drives it. Engine-level wiring is
// verified by code inspection + this test.

// One bus block through the ramped application — the per-sample body the
// engine bus loop runs (Engine.cpp bus loop) minus the mono fold, which the
// gain ramp does not interact with.
void ApplyBusGainBlock(float* samples, int frames, GainRamp& ramp, float target) {
  const bool gainValid = std::isfinite(target);
  if (!ramp.init) {
    if (gainValid) {
      ramp.applied = target;
      ramp.target = target;
    }
    ramp.samplesLeft = 0;
    ramp.init = true;
  } else if (gainValid && target != ramp.target) {
    ramp.target = target;
    ramp.step = (ramp.target - ramp.applied) / static_cast<float>(kGainRampSamples);
    ramp.samplesLeft = kGainRampSamples;
  }
  for (int i = 0; i < frames; ++i) {
    if (ramp.samplesLeft > 0) {
      ramp.applied += ramp.step;
      --ramp.samplesLeft;
      if (ramp.samplesLeft == 0) {
        ramp.applied = ramp.target;  // exact convergence, no float drift
      }
    }
    samples[i * 2] *= ramp.applied;
    samples[i * 2 + 1] *= ramp.applied;
  }
}

// B1-equivalent for the bus: a bus fader step must not click across the block
// boundary. Same bound as the strip test: 4x the max adjacent delta of the
// steady sine at the FINAL amplitude. The instant application (the pre-ramp
// engine behaviour this test was written RED against) bursts through it with
// an extra delta of (G-1)*amp = 2.98*0.5 at the boundary.
void TestBusGainStepDoesNotClick() {
  const float upGain = std::pow(10.f, 12.f / 20.f);  // +12 dB = 3.981071
  GainRamp ramp{};
  float block[kBlockFrames * kChannels];
  float phase = 0.f;

  FillSine(block, kBlockFrames, 1000.f, 0.5f, phase);
  ApplyBusGainBlock(block, kBlockFrames, ramp, 1.f);  // fresh state adopts unity
  const float lastL = block[(kBlockFrames - 1) * 2];
  const float lastR = block[(kBlockFrames - 1) * 2 + 1];

  FillSine(block, kBlockFrames, 1000.f, 0.5f, phase);  // same sine continuation
  ApplyBusGainBlock(block, kBlockFrames, ramp, upGain);
  const float maxDelta = MaxBlockDelta(block, lastL, lastR);
  const float bound = 4.f * MaxAdjacentDelta(0.5f * upGain, 1000.f);
  std::printf("MEASURE bus click maxdelta %.5f bound %.5f\n", maxDelta, bound);
  Expect(maxDelta <= bound, "a bus gain step does not click across the block boundary");

  // Exact convergence: by the end of the block the ramp is done and the gain
  // stays at the target — measured on the next steady block.
  FillSine(block, kBlockFrames, 1000.f, 0.5f, phase);
  ApplyBusGainBlock(block, kBlockFrames, ramp, upGain);
  float peakSteady = 0.f;
  for (int i = 0; i < kBlockFrames * kChannels; ++i) {
    peakSteady = std::fmax(peakSteady, std::fabs(block[i]));
  }
  const float targetAmp = 0.5f * upGain;
  std::printf("MEASURE bus steady peak %.5f target amp %.5f\n", peakSteady, targetAmp);
  Expect(ramp.applied == upGain, "the bus ramp converges to the exact target gain");
  Expect(ramp.samplesLeft == 0, "the bus ramp finishes within one block");
  Expect(peakSteady <= targetAmp * 1.02f, "the bus gain ramp never overshoots the target");
}

// The engine drives the bus loop block by block with the atomically published
// gain; a non-finite published value (defensive: SetBusGainDb now clamps NaN,
// but the atomic store boundary is the same class of API as ShapeStrips' raw
// p.gain) must hold the previous target instead of poisoning the state.
void TestBusGainNonFiniteHoldsTarget() {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  GainRamp ramp{};
  float block[kBlockFrames * kChannels];
  float phase = 0.f;
  const float upGain = std::pow(10.f, 12.f / 20.f);

  FillSine(block, kBlockFrames, 1000.f, 0.5f, phase);
  ApplyBusGainBlock(block, kBlockFrames, ramp, upGain);  // fresh: adopt +12 dB
  FillSine(block, kBlockFrames, 1000.f, 0.5f, phase);
  ApplyBusGainBlock(block, kBlockFrames, ramp, nan);  // poisoned block
  int nonFinite = 0;
  for (int i = 0; i < kBlockFrames * kChannels; ++i) {
    if (!std::isfinite(block[i])) {
      ++nonFinite;
    }
  }
  Expect(nonFinite == 0, "a non-finite bus gain block produces no NaN output");
  Expect(ramp.applied == upGain, "a non-finite bus gain holds the applied gain");
  Expect(ramp.target == upGain, "a non-finite bus gain holds the current target");
}

// --- Mic fader gain ramp (C1) -------------------------------------------------
// The mic application lives in Engine.cpp's capture loop, which — like the bus
// loop above — is not reachable headless (it runs on the render thread inside
// Impl::Loop over WASAPI state). The mic-application semantics are pinned the
// same way: a replicated unit driven block-by-block like the engine drives it,
// with the same click/convergence/overshoot bound math as the strip and bus
// tests.
//
// RED NOTE (historical): the first version of this helper replicated the
// pre-fix engine mic loop (Engine.cpp:2341-2346) verbatim — an instant
// per-block multiply with NO smoothing — plus the state bookkeeping the
// contract assertions needed to compile. Its RED run failed exactly the
// click and non-finite assertions (boundary delta 1.32720 up / 1.36221 down
// vs bound 1.03928, NaN block poisoned; PASS=130 FAIL=5 exit 1). Now that
// Engine.cpp ramps, the helper mirrors the new loop, exactly as
// ApplyBusGainBlock mirrors the bus loop. Engine-level wiring is verified by
// code inspection + this test.
//
// Body mirrors Engine.cpp's capture-loop mic ramp: lazy-adopt the first
// target, re-target steps from the applied value, snap exactly on the last
// sample, non-finite target holds the previous one; the same gain is applied
// to both channels of a frame (ramp advances once per frame, like the bus
// loop), and the unity fast path cannot skip ramp samples.
void ApplyMicGainBlock(float* samples, int frames, GainRamp& ramp, float target) {
  const bool gainValid = std::isfinite(target);
  if (!ramp.init) {
    if (gainValid) {
      ramp.applied = target;
      ramp.target = target;
    }
    ramp.samplesLeft = 0;
    ramp.init = true;
  } else if (gainValid && target != ramp.target) {
    ramp.target = target;
    ramp.step = (ramp.target - ramp.applied) / static_cast<float>(kGainRampSamples);
    ramp.samplesLeft = kGainRampSamples;
  }
  if (ramp.applied != 1.f || ramp.samplesLeft > 0) {
    for (int f = 0; f < frames; ++f) {
      if (ramp.samplesLeft > 0) {
        ramp.applied += ramp.step;
        --ramp.samplesLeft;
        if (ramp.samplesLeft == 0) {
          ramp.applied = ramp.target;  // exact convergence, no float drift
        }
      }
      samples[f * kChannels] *= ramp.applied;
      samples[f * kChannels + 1] *= ramp.applied;
    }
  }
}

// Least-squares gain of `out` over `dry` in the block's settled window (second
// half, past any 24-sample ramp): exact for out == dry * g. This is the
// "final ratio equals requested gain within 2e-4" convergence measure.
double SettledMicRatio(const float* out, const float* dry) {
  double num = 0.0;
  double den = 0.0;
  for (int i = 256 * kChannels; i < kBlockFrames * kChannels; ++i) {
    num += static_cast<double>(out[i]) * dry[i];
    den += static_cast<double>(dry[i]) * dry[i];
  }
  return den > 0.0 ? num / den : 0.0;
}

// C1 up-step: the mini-player's mic fader (slider, double-click 0 dB reset,
// wheel +-1 dB) steps from 1.0 to +12 dB mid-stream and must not click across
// the block boundary. Same bound as the strip and bus click tests: 4x the max
// adjacent delta of the steady sine at the FINAL amplitude. The instant
// application (the pre-ramp engine behaviour this is written RED against)
// bursts through it with the fader step itself, like the strip delta 1.32720
// vs bound 1.03928 in task-1-report.md.
void TestMicGainStepDoesNotClick() {
  const float upGain = std::pow(10.f, 12.f / 20.f);  // +12 dB = 3.981071
  GainRamp ramp{};
  float block[kBlockFrames * kChannels];
  float dry[kBlockFrames * kChannels];
  float phase = 0.f;

  FillSine(block, kBlockFrames, 1000.f, 0.5f, phase);
  ApplyMicGainBlock(block, kBlockFrames, ramp, 1.f);  // fresh state adopts unity
  const float lastL = block[(kBlockFrames - 1) * 2];
  const float lastR = block[(kBlockFrames - 1) * 2 + 1];

  FillSine(block, kBlockFrames, 1000.f, 0.5f, phase);  // same sine continuation
  std::memcpy(dry, block, sizeof(dry));
  ApplyMicGainBlock(block, kBlockFrames, ramp, upGain);
  const float maxDelta = MaxBlockDelta(block, lastL, lastR);
  const float bound = 4.f * MaxAdjacentDelta(0.5f * upGain, 1000.f);
  std::printf("MEASURE mic click up maxdelta %.5f bound %.5f\n", maxDelta, bound);
  Expect(maxDelta <= bound, "a mic gain step does not click across the block boundary");

  // Exact convergence measured on the signal: the settled window of the step
  // block must carry the requested gain within 2e-4.
  const double settled = SettledMicRatio(block, dry);
  std::printf("MEASURE mic up settled %.5f target %.5f\n", settled, upGain);
  Expect(std::fabs(settled - upGain) <= 2e-4 * upGain, "the mic ramp converges to the requested gain");
  Expect(ramp.samplesLeft == 0, "the mic ramp finishes within one block");

  FillSine(block, kBlockFrames, 1000.f, 0.5f, phase);  // steady block afterwards
  ApplyMicGainBlock(block, kBlockFrames, ramp, upGain);
  float peakSteady = 0.f;
  for (int i = 0; i < kBlockFrames * kChannels; ++i) {
    peakSteady = std::fmax(peakSteady, std::fabs(block[i]));
  }
  const float targetAmp = 0.5f * upGain;
  std::printf("MEASURE mic steady peak %.5f target amp %.5f\n", peakSteady, targetAmp);
  Expect(peakSteady <= targetAmp * 1.02f, "the mic gain ramp never overshoots the target");
}

// C1 down-step: +12 dB -> -6 dB mid-stream (mirrors test_gain.cpp B3). The
// continuity bound is the same as the up-step's — 4x the steady adjacent delta
// at the HIGHEST amplitude involved (1.9905 = the pre-step level); a bound
// scaled to the lower -6 dB amplitude is not attainable by ANY implementation
// (see the B3 comment in test_gain.cpp). Plus exact convergence and no
// overshoot on the settle.
void TestMicGainDownStepDoesNotClick() {
  const float upGain = std::pow(10.f, 12.f / 20.f);
  const float downGain = std::pow(10.f, -6.f / 20.f);
  const float targetAmp = 0.5f * downGain;
  GainRamp ramp{};
  float block[kBlockFrames * kChannels];
  float dry[kBlockFrames * kChannels];
  float phase = 0.f;

  FillSine(block, kBlockFrames, 1000.f, 0.5f, phase);
  ApplyMicGainBlock(block, kBlockFrames, ramp, upGain);  // fresh: adopt +12 dB
  const float lastL = block[(kBlockFrames - 1) * 2];
  const float lastR = block[(kBlockFrames - 1) * 2 + 1];

  FillSine(block, kBlockFrames, 1000.f, 0.5f, phase);  // same sine continuation
  std::memcpy(dry, block, sizeof(dry));
  ApplyMicGainBlock(block, kBlockFrames, ramp, downGain);
  const float maxDelta = MaxBlockDelta(block, lastL, lastR);
  const float bound = 4.f * MaxAdjacentDelta(0.5f * upGain, 1000.f);
  std::printf("MEASURE mic click down maxdelta %.5f bound %.5f\n", maxDelta, bound);
  Expect(maxDelta <= bound, "a mic gain down-step does not click across the block boundary");

  const double settled = SettledMicRatio(block, dry);
  std::printf("MEASURE mic down settled %.5f target %.5f\n", settled, downGain);
  Expect(std::fabs(settled - downGain) <= 2e-4 * downGain,
         "a mic down-step converges to the requested gain");

  FillSine(block, kBlockFrames, 1000.f, 0.5f, phase);  // fully steady block
  ApplyMicGainBlock(block, kBlockFrames, ramp, downGain);
  float peakSteady = 0.f;
  for (int i = 0; i < kBlockFrames * kChannels; ++i) {
    peakSteady = std::fmax(peakSteady, std::fabs(block[i]));
  }
  std::printf("MEASURE mic down steady peak %.5f target amp %.5f\n", peakSteady, targetAmp);
  Expect(peakSteady <= targetAmp * 1.02f, "a mic down-step settles without overshoot");
}

// C1 non-finite: the engine drives the mic loop block by block with the
// atomically published gain (same class of API as the bus/strip p.gain); a
// non-finite value must hold the previous target instead of poisoning the
// state or the samples. At RED the instant path multiplies NaN straight in.
void TestMicGainNonFiniteHoldsTarget() {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  GainRamp ramp{};
  float block[kBlockFrames * kChannels];
  float phase = 0.f;
  const float upGain = std::pow(10.f, 12.f / 20.f);

  FillSine(block, kBlockFrames, 1000.f, 0.5f, phase);
  ApplyMicGainBlock(block, kBlockFrames, ramp, upGain);  // fresh: adopt +12 dB
  FillSine(block, kBlockFrames, 1000.f, 0.5f, phase);
  ApplyMicGainBlock(block, kBlockFrames, ramp, nan);  // poisoned block
  int nonFinite = 0;
  for (int i = 0; i < kBlockFrames * kChannels; ++i) {
    if (!std::isfinite(block[i])) {
      ++nonFinite;
    }
  }
  Expect(nonFinite == 0, "a non-finite mic gain block produces no NaN output");
  Expect(ramp.applied == upGain, "a non-finite mic gain holds the applied gain");
  Expect(ramp.target == upGain, "a non-finite mic gain holds the current target");
}

// --- ASIO mute gate (finding R12) ----------------------------------------------
// ExchangeAsio copies shaped[] into asioPending[], which PublishAsioBlock
// hands to the external ASIO host (Cantabile). That copy runs BEFORE the
// engine's anti-resurrection memset (Engine.cpp render loop), so a muted or
// solo-cut strip was sent to the host anyway — including audio a local VST
// just generated. Contract under test: a strip that is not audible is zero on
// every output path, so the copy that feeds the host has to be gated by
// `audible`. Engine-side wiring (ExchangeAsio taking params) is verified by
// code inspection, like the bus/mic loops above: ExchangeAsio is only reachable
// from the render thread over a live shared-memory bridge.
void TestAsioGateSkipsMutedStrips() {
  constexpr int kGateStrips = 2;
  float srcStore[kGateStrips][kBlockFrames * kChannels];
  float dstStore[kGateStrips][kBlockFrames * kChannels];
  float* src[kGateStrips];
  float* dst[kGateStrips];
  const bool audible[kGateStrips] = {true, false};
  const float amps[kGateStrips] = {0.5f, -0.25f};
  float phase = 0.f;
  for (int i = 0; i < kGateStrips; ++i) {
    src[i] = srcStore[i];
    dst[i] = dstStore[i];
    FillSine(srcStore[i], kBlockFrames, 1000.f, amps[i], phase);
    // Poison the destination: a copy that never lands must be visible, not
    // mistaken for the silence a correct gate writes.
    std::memset(dstStore[i], 0x43, sizeof(dstStore[i]));
  }

  anymix::CopyStripBlockGated(src, audible, kGateStrips, kBlockFrames, dst);

  float maxAudibleErr = 0.f;
  float mutedPeak = 0.f;
  for (int i = 0; i < kBlockFrames * kChannels; ++i) {
    maxAudibleErr = std::fmax(maxAudibleErr, std::fabs(dstStore[0][i] - srcStore[0][i]));
    mutedPeak = std::fmax(mutedPeak, std::fabs(dstStore[1][i]));
  }
  std::printf("MEASURE asio gate audible err %.5f muted peak %.5f\n", maxAudibleErr, mutedPeak);
  Expect(maxAudibleErr == 0.f, "the ASIO gate copies an audible strip exactly");
  Expect(mutedPeak == 0.f, "the ASIO gate silences a muted strip before the host send");
}

}  // namespace

void RunEngineTests() {
  TestClampFaderDb();
  TestBusGainStepDoesNotClick();
  TestBusGainNonFiniteHoldsTarget();
  TestMicGainStepDoesNotClick();
  TestMicGainDownStepDoesNotClick();
  TestMicGainNonFiniteHoldsTarget();
  TestAsioGateSkipsMutedStrips();
}
