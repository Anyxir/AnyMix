#include "anymix/Process.h"

#include <cmath>
#include <cstring>

namespace anymix {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// Per-strip fader smoothing state, kept OUT of StripEq (see the note in
// Process.h: a pre-existing test hands ShapeStrips an undersized StripEq, so
// eq[] cannot carry load-bearing state). Confined to the single audio thread
// that runs ShapeStrips; no allocation, no locks.
GainRamp gStripRamps[kStrips]{};

BiquadCoeffs Normalize(float b0, float b1, float b2, float a0, float a1, float a2) {
  BiquadCoeffs c;
  const float inv = 1.f / a0;
  c.b0 = b0 * inv;
  c.b1 = b1 * inv;
  c.b2 = b2 * inv;
  c.a1 = a1 * inv;
  c.a2 = a2 * inv;
  return c;
}

}  // namespace

void ResetStripGainRamps() {
  for (int strip = 0; strip < kStrips; ++strip) {
    gStripRamps[strip] = GainRamp{};
  }
}

BiquadCoeffs MakeLowShelf(float gainDb, float freqHz, float sampleRate) {
  if (std::fabs(gainDb) < 0.001f) {
    return {};
  }
  const float A = std::pow(10.f, gainDb / 40.f);
  const float w0 = 2.f * kPi * freqHz / sampleRate;
  const float cosw = std::cos(w0);
  const float sinw = std::sin(w0);
  const float alpha = sinw * 0.5f * std::sqrt(2.f);
  const float two = 2.f * std::sqrt(A) * alpha;
  return Normalize(A * ((A + 1.f) - (A - 1.f) * cosw + two),
                   2.f * A * ((A - 1.f) - (A + 1.f) * cosw),
                   A * ((A + 1.f) - (A - 1.f) * cosw - two),
                   (A + 1.f) + (A - 1.f) * cosw + two,
                   -2.f * ((A - 1.f) + (A + 1.f) * cosw),
                   (A + 1.f) + (A - 1.f) * cosw - two);
}

BiquadCoeffs MakeHighShelf(float gainDb, float freqHz, float sampleRate) {
  if (std::fabs(gainDb) < 0.001f) {
    return {};
  }
  const float A = std::pow(10.f, gainDb / 40.f);
  const float w0 = 2.f * kPi * freqHz / sampleRate;
  const float cosw = std::cos(w0);
  const float sinw = std::sin(w0);
  const float alpha = sinw * 0.5f * std::sqrt(2.f);
  const float two = 2.f * std::sqrt(A) * alpha;
  return Normalize(A * ((A + 1.f) + (A - 1.f) * cosw + two),
                   -2.f * A * ((A - 1.f) + (A + 1.f) * cosw),
                   A * ((A + 1.f) + (A - 1.f) * cosw - two),
                   (A + 1.f) - (A - 1.f) * cosw + two,
                   2.f * ((A - 1.f) - (A + 1.f) * cosw),
                   (A + 1.f) - (A - 1.f) * cosw - two);
}

void LoadEq(const PublishedEq& published, BiquadCoeffs& bass, BiquadCoeffs& treble) {
  uint32_t first = 0;
  uint32_t second = 0;
  do {
    first = published.seq.load(std::memory_order_acquire);
    bass = published.bass;
    treble = published.treble;
    second = published.seq.load(std::memory_order_acquire);
  } while (first != second || (first & 1u) != 0u);
}

void ShapeStrips(const float* const inputs[kStrips],
                 int frames,
                 const BlockParams params[kStrips],
                 StripEq eq[kStrips],
                 float* const shaped[kStrips]) {
  for (int strip = 0; strip < kStrips; ++strip) {
    float* out = shaped[strip];
    const int samples = frames * kChannels;
    if (!params[strip].audible || inputs[strip] == nullptr) {
      // Mute (or no input): silence the strip and leave the ramp state as it
      // is, so an unmute with a moved fader ramps in instead of stepping.
      std::memset(out, 0, sizeof(float) * static_cast<size_t>(samples));
      continue;
    }
    const float* in = inputs[strip];
    const BlockParams& p = params[strip];
    GainRamp& ramp = gStripRamps[strip];  // not eq[]: see the layout note in Process.h
    // A non-finite published gain (NaN/inf) is treated as "keep the current
    // target": NaN compares unequal to everything, so adopting it would
    // re-arm the ramp on every block and multiply the whole strip by NaN
    // forever. Baseline (no ramp state) self-healed on the next block; this
    // must not be worse than baseline. Costs one isfinite per block.
    const bool gainValid = std::isfinite(p.gain);
    if (!ramp.init) {
      // First block through fresh state: adopt the target exactly, so a
      // steady gain is never smeared by a phantom ramp.
      if (gainValid) {
        ramp.applied = p.gain;
        ramp.target = p.gain;
      }
      ramp.samplesLeft = 0;
      ramp.init = true;
    } else if (gainValid && p.gain != ramp.target) {
      // New fader target: linear ramp from the currently applied gain,
      // snapped exactly to the target on the last sample.
      ramp.target = p.gain;
      ramp.step = (ramp.target - ramp.applied) / static_cast<float>(kGainRampSamples);
      ramp.samplesLeft = kGainRampSamples;
    }
    for (int frame = 0; frame < frames; ++frame) {
      float left = ApplyBiquad(p.bass, eq[strip].bass[0], in[frame * 2]);
      float right = ApplyBiquad(p.bass, eq[strip].bass[1], in[frame * 2 + 1]);
      left = ApplyBiquad(p.treble, eq[strip].treble[0], left);
      right = ApplyBiquad(p.treble, eq[strip].treble[1], right);
      if (p.mono) {
        const float mid = 0.5f * (left + right);
        left = mid;
        right = mid;
      }
      ApplyBalance(left, right, p.pan);
      if (ramp.samplesLeft > 0) {
        ramp.applied += ramp.step;
        --ramp.samplesLeft;
        if (ramp.samplesLeft == 0) {
          ramp.applied = ramp.target;  // exact convergence, no float drift
        }
      }
      left *= ramp.applied;
      right *= ramp.applied;
      out[frame * 2] = left;
      out[frame * 2 + 1] = right;
    }
  }
}

void MixToBuses(const float* const shaped[kStrips],
                int frames,
                const BlockParams params[kStrips],
                float* const buses[kBuses]) {
  const int samples = frames * kChannels;
  for (int bus = 0; bus < kBuses; ++bus) {
    std::memset(buses[bus], 0, sizeof(float) * static_cast<size_t>(samples));
  }
  for (int strip = 0; strip < kStrips; ++strip) {
    const uint16_t route = params[strip].route;
    if (route == 0 || !params[strip].audible) {
      continue;
    }
    const float* in = shaped[strip];
    for (int bus = 0; bus < kBuses; ++bus) {
      if ((route & static_cast<uint16_t>(1u << bus)) == 0) {
        continue;
      }
      float* out = buses[bus];
      for (int i = 0; i < samples; ++i) {
        out[i] += in[i];
      }
    }
  }
}

void CopyStripBlockGated(float* const* src,
                         const bool* audible,
                         int stripCount,
                         int frames,
                         float* const* dst) {
  // Copies the strips that reach the host as they are and writes explicit
  // silence for the muted/solo-cut ones: the send to the ASIO host happens
  // before the engine's anti-resurrection memset, so the gate has to live here
  // too, or a muted strip (and any local VST output) leaks to Cantabile.
  const size_t bytes = sizeof(float) * static_cast<size_t>(frames) * kChannels;
  for (int strip = 0; strip < stripCount; ++strip) {
    if (audible[strip]) {
      std::memcpy(dst[strip], src[strip], bytes);
    } else {
      std::memset(dst[strip], 0, bytes);
    }
  }
}

void LimitStereo(float* interleaved, int frames, PeakLimit& state) {
  // The fader can add 24 dB. A full-scale wave then sits well over 0 dBFS.
  // The gain has to come down, but it must stay flat across a cycle: chasing
  // each peak reshapes the wave and is what sounds distorted at full volume.
  constexpr float ceiling = 0.98f;
  constexpr float clip = 0.999f;
  constexpr float attack = 0.35f;
  constexpr float release = 0.00008f;
  constexpr int holdSamples = 960;
  for (int frame = 0; frame < frames; ++frame) {
    float& left = interleaved[frame * 2];
    float& right = interleaved[frame * 2 + 1];
    const float peak = std::max(std::fabs(left), std::fabs(right));
    if (!(peak >= 0.f)) {
      left = 0.f;
      right = 0.f;
      continue;
    }
    if (peak >= state.env) {
      state.env = peak;
      state.hold = holdSamples;
    } else if (state.hold > 0) {
      --state.hold;
    } else {
      state.env += (0.f - state.env) * release;
    }
    const float target = state.env > ceiling ? ceiling / state.env : 1.f;
    const float coeff = target < state.gain ? attack : release;
    state.gain += (target - state.gain) * coeff;
    if (!(state.gain > 0.f) || state.gain > 1.f) {
      state.gain = target > 0.f && target <= 1.f ? target : 1.f;
    }
    left *= state.gain;
    right *= state.gain;
    if (left > clip) {
      left = clip;
    } else if (left < -clip) {
      left = -clip;
    }
    if (right > clip) {
      right = clip;
    } else if (right < -clip) {
      right = -clip;
    }
  }
}

}  // namespace anymix
