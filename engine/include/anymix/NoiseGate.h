#pragma once

#include <algorithm>
#include <cmath>

namespace anymix {

// Internal parameters driven by one user level. CalculateGateSettings is the
// only mapping from that level onto the detector and the envelope.
struct GateSettings {
  bool enabled = false;
  float thresholdDb = -56.f;
  float attackMs = 2.5f;
  float holdMs = 180.f;
  float releaseMs = 280.f;
  float rangeDb = -12.f;
  float hysteresisDb = 4.f;
};

struct NoiseGateParams {
  bool enabled = false;
  bool bypass = false;
  float thresholdDb = -40.f;
  float attackMs = 3.f;
  float holdMs = 120.f;
  float releaseMs = 180.f;
  float rangeDb = -60.f;
};

inline GateSettings CalculateGateSettings(float gateLevel) {
  if (!(gateLevel >= 0.f)) {
    gateLevel = 0.f;
  }
  gateLevel = std::min(10.f, gateLevel);
  GateSettings off;
  if (gateLevel <= 0.001f) {
    return off;
  }
  struct Knot {
    float thresholdDb;
    float attackMs;
    float holdMs;
    float releaseMs;
    float rangeDb;
    float hysteresisDb;
  };
  // 1–2 still leave a quiet room almost untouched. From 4 upward the threshold
  // climbs into typical PC noise, and the closed attenuation gets deep. Attack
  // stays under 2 ms so word onsets are not shaved off.
  static constexpr Knot kKnots[10] = {
      {-50.f, 2.0f, 150.f, 220.f, -24.f, 5.f}, {-46.f, 1.8f, 140.f, 200.f, -34.f, 6.f},
      {-43.f, 1.6f, 130.f, 180.f, -42.f, 6.5f}, {-40.f, 1.5f, 120.f, 160.f, -50.f, 7.f},
      {-36.f, 1.4f, 110.f, 150.f, -58.f, 7.5f}, {-33.f, 1.2f, 100.f, 130.f, -64.f, 8.f},
      {-30.f, 1.1f, 95.f, 120.f, -72.f, 8.5f}, {-28.f, 1.0f, 90.f, 110.f, -78.f, 9.f},
      {-26.f, 0.9f, 85.f, 100.f, -84.f, 9.5f}, {-24.f, 0.8f, 80.f, 90.f, -90.f, 10.f},
  };
  const float pos = std::min(9.f, std::max(0.f, gateLevel - 1.f));
  const int index = std::min(8, static_cast<int>(pos));
  const float blend = pos - static_cast<float>(index);
  const Knot& a = kKnots[index];
  const Knot& b = kKnots[index + 1];
  const auto mix = [blend](float from, float to) { return from + (to - from) * blend; };
  GateSettings settings;
  settings.enabled = true;
  settings.thresholdDb = mix(a.thresholdDb, b.thresholdDb);
  settings.attackMs = mix(a.attackMs, b.attackMs);
  settings.holdMs = mix(a.holdMs, b.holdMs);
  settings.releaseMs = mix(a.releaseMs, b.releaseMs);
  settings.rangeDb = mix(a.rangeDb, b.rangeDb);
  settings.hysteresisDb = mix(a.hysteresisDb, b.hysteresisDb);
  return settings;
}

inline float GateLevelFromLegacy(float thresholdDb, float rangeDb) {
  float best = 5.f;
  float bestScore = 1.0e9f;
  for (int step = 1; step <= 40; ++step) {
    const float level = static_cast<float>(step) * 0.25f;
    const GateSettings settings = CalculateGateSettings(level);
    const float score = std::fabs(settings.thresholdDb - thresholdDb) +
                        0.45f * std::fabs(settings.rangeDb - rangeDb);
    if (score < bestScore) {
      bestScore = score;
      best = level;
    }
  }
  return best;
}

// Stereo noise gate. One detector drives both channels. The detector listens
// to a high-passed copy so rumble does not open the gate; the audio path is
// the original samples times a continuous gain. Pass only the bus to gate.
class NoiseGate {
 public:
  static constexpr float kHysteresisDb = 6.f;
  static constexpr int kMaxWindow = 960;

  void Prepare(float sampleRate) {
    sampleRate_ = sampleRate > 1000.f ? sampleRate : 48000.f;
    const int window = static_cast<int>(std::lround(0.015 * static_cast<double>(sampleRate_)));
    window_ = std::max(64, std::min(kMaxWindow, window));
    passCoeff_ = Coeff(10.f, sampleRate_);
    levelCoeff_ = Coeff(40.f, sampleRate_);
    paramCoeff_ = Coeff(30.f, sampleRate_);
    SetHighpass(120.f);
    Reset();
  }

  void SetLevel(float level) {
    if (!(level >= 0.f)) {
      level = 0.f;
    }
    targetLevel_ = std::min(10.f, level);
    useLevel_ = true;
  }

  void SetParams(const NoiseGateParams& params) {
    useLevel_ = false;
    enabled_ = params.enabled;
    bypass_ = params.bypass;
    const float thresholdDb = std::max(-80.f, std::min(0.f, params.thresholdDb));
    const float attackMs = std::max(0.05f, std::min(500.f, params.attackMs));
    const float holdMs = std::max(0.f, std::min(2000.f, params.holdMs));
    const float releaseMs = std::max(0.05f, std::min(5000.f, params.releaseMs));
    const float rangeDb = std::max(-120.f, std::min(0.f, params.rangeDb));
    if (thresholdDb == thresholdDb_ && attackMs == attackMs_ && holdMs == holdMs_ && releaseMs == releaseMs_ &&
        rangeDb == rangeDb_) {
      return;
    }
    thresholdDb_ = thresholdDb;
    attackMs_ = attackMs;
    holdMs_ = holdMs;
    releaseMs_ = releaseMs;
    rangeDb_ = rangeDb;
    openRms_ = DbToLin(thresholdDb_);
    closeRms_ = DbToLin(thresholdDb_ - kHysteresisDb);
    floor_ = DbToLin(rangeDb_);
    attackCoeff_ = Coeff(attackMs_, sampleRate_);
    releaseCoeff_ = Coeff(releaseMs_, sampleRate_);
    holdSamples_ = static_cast<int>(holdMs_ * 0.001f * sampleRate_ + 0.5f);
  }

  void Reset() {
    for (int i = 0; i < kMaxWindow; ++i) {
      power_[i] = 0.f;
    }
    write_ = 0;
    filled_ = 0;
    sum_ = 0.0;
    gain_ = 1.f;
    holdLeft_ = 0;
    open_ = false;
    for (int channel = 0; channel < 2; ++channel) {
      hpfX1_[channel] = 0.f;
      hpfX2_[channel] = 0.f;
      hpfY1_[channel] = 0.f;
      hpfY2_[channel] = 0.f;
    }
  }

  // Interleaved stereo, in place. Returns true while the detector is open,
  // including hold. Level 0 and bypass ramp the gain back to 1.
  bool Process(float* interleaved, int frames) {
    if (interleaved == nullptr || frames <= 0 || window_ <= 0) {
      return enabled_ && !bypass_ && open_;
    }
    for (int frame = 0; frame < frames; ++frame) {
      if (useLevel_) {
        AdvanceLevel();
      }
      const float left = interleaved[frame * 2];
      const float right = interleaved[frame * 2 + 1];
      const float detectLeft = Highpass(left, 0);
      const float detectRight = Highpass(right, 1);
      float power = 0.5f * (detectLeft * detectLeft + detectRight * detectRight);
      if (!(power >= 0.f)) {
        power = 0.f;
      }
      sum_ -= static_cast<double>(power_[write_]);
      power_[write_] = power;
      sum_ += static_cast<double>(power);
      if (sum_ < 0.0) {
        sum_ = 0.0;
      }
      write_ += 1;
      if (write_ >= window_) {
        write_ = 0;
        if (filled_ >= window_) {
          double check = 0.0;
          for (int i = 0; i < window_; ++i) {
            check += static_cast<double>(power_[i]);
          }
          sum_ = check;
        }
      }
      if (filled_ < window_) {
        filled_ += 1;
      }
      const float rms = std::sqrt(static_cast<float>(sum_ / static_cast<double>(std::max(filled_, 1))));

      float target = 1.f;
      if (!enabled_) {
        open_ = false;
        holdLeft_ = 0;
      } else if (rms >= openRms_ || (open_ && rms >= closeRms_)) {
        open_ = true;
        holdLeft_ = holdSamples_;
        target = 1.f;
      } else if (holdLeft_ > 0) {
        holdLeft_ -= 1;
        target = 1.f;
      } else {
        open_ = false;
        target = floor_;
      }
      float coeff = passCoeff_;
      if (enabled_ && !bypass_) {
        coeff = target > gain_ ? attackCoeff_ : releaseCoeff_;
      } else {
        target = 1.f;
      }
      gain_ += (target - gain_) * coeff;
      if (!(gain_ >= 0.f)) {
        gain_ = 1.f;
        open_ = false;
      } else if (gain_ > 1.f) {
        gain_ = 1.f;
      }
      interleaved[frame * 2] = left * gain_;
      interleaved[frame * 2 + 1] = right * gain_;
    }
    return enabled_ && !bypass_ && open_;
  }

  bool Open() const { return enabled_ && !bypass_ && open_; }
  float Gain() const { return gain_; }

 private:
  static float Coeff(float ms, float sampleRate) {
    const float samples = std::max(1.f, ms * 0.001f * sampleRate);
    return 1.f - std::exp(-1.f / samples);
  }

  static float DbToLin(float db) { return std::pow(10.f, db / 20.f); }

  void SetHighpass(float frequencyHz) {
    const float q = 0.707f;
    const float w0 = 2.f * 3.14159265f * frequencyHz / sampleRate_;
    const float cosw = std::cos(w0);
    const float sinw = std::sin(w0);
    const float alpha = sinw / (2.f * q);
    const float a0 = 1.f + alpha;
    hpfB0_ = ((1.f + cosw) * 0.5f) / a0;
    hpfB1_ = (-(1.f + cosw)) / a0;
    hpfB2_ = hpfB0_;
    hpfA1_ = (-2.f * cosw) / a0;
    hpfA2_ = (1.f - alpha) / a0;
  }

  float Highpass(float sample, int channel) {
    const float y = hpfB0_ * sample + hpfB1_ * hpfX1_[channel] + hpfB2_ * hpfX2_[channel] - hpfA1_ * hpfY1_[channel] -
                    hpfA2_ * hpfY2_[channel];
    hpfX2_[channel] = hpfX1_[channel];
    hpfX1_[channel] = sample;
    hpfY2_[channel] = hpfY1_[channel];
    hpfY1_[channel] = y;
    if (!(y > -8.f && y < 8.f)) {
      hpfX1_[channel] = 0.f;
      hpfX2_[channel] = 0.f;
      hpfY1_[channel] = 0.f;
      hpfY2_[channel] = 0.f;
      return 0.f;
    }
    return y;
  }

  void AdvanceLevel() {
    level_ += (targetLevel_ - level_) * levelCoeff_;
    if (!(level_ >= 0.f)) {
      level_ = 0.f;
    }
    if (!haveCook_ || std::fabs(level_ - cookedLevel_) >= 0.02f) {
      const GateSettings settings = CalculateGateSettings(level_);
      cookedLevel_ = level_;
      haveCook_ = true;
      enabled_ = settings.enabled;
      bypass_ = false;
      targetOpen_ = DbToLin(settings.thresholdDb);
      targetClose_ = DbToLin(settings.thresholdDb - settings.hysteresisDb);
      targetFloor_ = DbToLin(settings.rangeDb);
      targetAttack_ = Coeff(settings.attackMs, sampleRate_);
      targetRelease_ = Coeff(settings.releaseMs, sampleRate_);
      targetHold_ = static_cast<int>(settings.holdMs * 0.001f * sampleRate_ + 0.5f);
    }
    openRms_ += (targetOpen_ - openRms_) * paramCoeff_;
    closeRms_ += (targetClose_ - closeRms_) * paramCoeff_;
    floor_ += (targetFloor_ - floor_) * paramCoeff_;
    attackCoeff_ += (targetAttack_ - attackCoeff_) * paramCoeff_;
    releaseCoeff_ += (targetRelease_ - releaseCoeff_) * paramCoeff_;
    if (holdSamples_ < targetHold_) {
      holdSamples_ += 1;
    } else if (holdSamples_ > targetHold_) {
      holdSamples_ -= 1;
    }
  }

  float sampleRate_ = 48000.f;
  int window_ = 720;
  float power_[kMaxWindow]{};
  int write_ = 0;
  int filled_ = 0;
  double sum_ = 0.0;
  float gain_ = 1.f;
  float floor_ = 0.001f;
  float openRms_ = 0.01f;
  float closeRms_ = 0.005f;
  float attackCoeff_ = 0.01f;
  float releaseCoeff_ = 0.001f;
  float passCoeff_ = 0.01f;
  float levelCoeff_ = 0.01f;
  float paramCoeff_ = 0.01f;
  int holdSamples_ = 0;
  int holdLeft_ = 0;
  int targetHold_ = 0;
  bool open_ = false;
  bool enabled_ = false;
  bool bypass_ = false;
  bool useLevel_ = true;
  bool haveCook_ = false;
  float targetLevel_ = 0.f;
  float level_ = 0.f;
  float cookedLevel_ = -1.f;
  float targetOpen_ = 0.01f;
  float targetClose_ = 0.005f;
  float targetFloor_ = 0.001f;
  float targetAttack_ = 0.01f;
  float targetRelease_ = 0.001f;
  float thresholdDb_ = -1000.f;
  float attackMs_ = -1.f;
  float holdMs_ = -1.f;
  float releaseMs_ = -1.f;
  float rangeDb_ = -1000.f;
  float hpfB0_ = 1.f;
  float hpfB1_ = 0.f;
  float hpfB2_ = 0.f;
  float hpfA1_ = 0.f;
  float hpfA2_ = 0.f;
  float hpfX1_[2]{};
  float hpfX2_[2]{};
  float hpfY1_[2]{};
  float hpfY2_[2]{};
};

}  // namespace anymix
