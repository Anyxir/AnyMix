#pragma once

#include "anymix/Types.h"

#include <algorithm>
#include <cmath>

namespace anymix {

BiquadCoeffs MakeLowShelf(float gainDb, float freqHz, float sampleRate);
BiquadCoeffs MakeHighShelf(float gainDb, float freqHz, float sampleRate);

// Clamps a fader setting in dB to the legal fader range. NaN maps to the
// neutral 0 dB: the old std::max/std::min pair mapped it to the +24 dB ceiling
// (std::min(24, NaN) returns 24), i.e. a broken value produced full volume.
// +-inf follows the plain min/max path and clamps to the ceiling/floor, which
// is what "as loud as possible" / "as quiet as possible" should mean.
inline float ClampFaderDb(float db) {
  if (std::isnan(db)) {
    return 0.f;
  }
  return std::max(kFaderMinDb, std::min(kFaderMaxDb, db));
}

inline void ApplyBalance(float& left, float& right, float pan) {
  if (pan < 0.f) {
    right *= 1.f + pan;
  } else if (pan > 0.f) {
    left *= 1.f - pan;
  }
}

inline float ApplyBiquad(const BiquadCoeffs& c, BiquadState& s, float x) {
  const float y = c.b0 * x + c.b1 * s.x1 + c.b2 * s.x2 - c.a1 * s.y1 - c.a2 * s.y2;
  s.x2 = s.x1;
  s.x1 = x;
  s.y2 = s.y1;
  s.y1 = y;
  return y;
}

// Per-strip fader smoothing state. The engine publishes a new atomic target
// gain every block; multiplying it straight into the samples steps the output
// at the block boundary (a click). GainRamp walks the applied gain to the
// target one sample at a time. Plain data, no allocation, no locks: it lives
// on the audio path. Task 2 reuses this type for the bus ramps.
struct GainRamp {
  float applied = 1.f;   // gain currently applied to the samples
  float target = 1.f;    // last requested gain (p.gain)
  float step = 0.f;      // per-sample increment while a ramp runs
  int samplesLeft = 0;   // remaining ramp samples; 0 = steady at target
  bool init = false;     // first target seen: adopt it exactly, never ramp
};

// Ramp length: 24 samples = 0.5 ms at 48 kHz. Long enough to remove the block
// boundary discontinuity (per-sample slew stays within a few percent of the
// wave's own slew), short enough that a block still measures its steady gain:
// the pre-existing DSP suite RMS-measures the block right after a fader move,
// and its tightest assertion (TestGainPositive +3 dB step) only tolerates a
// 31-sample ramp. A ~256-sample (5.3 ms) ramp would fail that suite; B2 keeps
// 256 samples as the convergence deadline instead.
inline constexpr int kGainRampSamples = 24;

// Clears the per-strip fader smoothing state; call on engine (re)start and
// from tests that need fresh-state semantics.
void ResetStripGainRamps();

// NOTE: the fader ramp state deliberately stays file-local in Process.cpp
// (gStripRamps, reset through ResetStripGainRamps), not a member of StripEq:
// StripEq remains pure DSP state (biquad states only) and the reset path stays
// centralized. The pre-existing undersized-call hazard that used to force the
// old dead tone* fields to keep the layout has been fixed (selftest.cpp now
// passes a full StripEq[kStrips]).
struct StripEq {
  BiquadState bass[kChannels]{};
  BiquadState treble[kChannels]{};
};

struct BlockParams {
  float gain = 1.f;
  bool audible = true;
  bool mono = false;
  float pan = 0.f;
  uint16_t route = 0;
  BiquadCoeffs bass{};
  BiquadCoeffs treble{};
};

void ShapeStrips(const float* const inputs[kStrips],
                 int frames,
                 const BlockParams params[kStrips],
                 StripEq eq[kStrips],
                 float* const shaped[kStrips]);

struct PeakLimit {
  float env = 0.f;
  float gain = 1.f;
  int hold = 0;
};

void MixToBuses(const float* const shaped[kStrips],
                int frames,
                const BlockParams params[kStrips],
                float* const buses[kBuses]);

// Copies one block per strip from src[strip] to dst[strip], frame by frame
// (kChannels interleaved samples per frame), but writes silence to dst[strip]
// when audible[strip] is false. Used on the way to the external ASIO host: the
// mute contract is "a muted strip is zero on EVERY output path", and the host
// send happens before the engine's anti-resurrection memset, so the send
// itself has to be gated. stripCount strips are processed; the caller keeps
// the dst buffers untouched elsewhere.
void CopyStripBlockGated(float* const* src,
                         const bool* audible,
                         int stripCount,
                         int frames,
                         float* const* dst);

void LimitStereo(float* interleaved, int frames, PeakLimit& state);

void LoadEq(const PublishedEq& published, BiquadCoeffs& bass, BiquadCoeffs& treble);

}  // namespace anymix
