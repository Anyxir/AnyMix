#pragma once

#include "anymix/Types.h"

namespace anymix {

BiquadCoeffs MakeLowShelf(float gainDb, float freqHz, float sampleRate);
BiquadCoeffs MakeHighShelf(float gainDb, float freqHz, float sampleRate);

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

struct StripEq {
  BiquadState bass[kChannels]{};
  BiquadState treble[kChannels]{};
  float toneEnv = 0.f;
  float toneGain = 1.f;
};

struct BlockParams {
  float gain = 1.f;
  bool audible = true;
  bool mono = false;
  float pan = 0.f;
  uint16_t route = 0;
  float toneTrim = 1.f;
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

void LimitStereo(float* interleaved, int frames, PeakLimit& state);

void LoadEq(const PublishedEq& published, BiquadCoeffs& bass, BiquadCoeffs& treble);

}  // namespace anymix
