#pragma once

#include <algorithm>
#include <cstddef>
#include <cwchar>

namespace anymix::ui {

// Maps a dB gain onto the fader's [0,1] travel across [minDb, maxDb].
// Same linear mapping and same clamps as the ad-hoc slider arithmetic.
inline float FaderTFromDb(float db, float minDb, float maxDb) {
  const float t = (db - minDb) / (maxDb - minDb);
  return std::max(0.f, std::min(1.f, t));
}

// Formats a gain in dB into a bounded buffer: L"-inf" at the fader floor
// (db <= -59.9f), otherwise L"%+.1f" such as L"+0.0", L"-12.5", L"+24.0".
inline void FormatGainDb(float db, wchar_t* out, int capacity) {
  if (out == nullptr || capacity <= 0) {
    return;
  }
  if (db <= -59.9f) {
    std::swprintf(out, static_cast<std::size_t>(capacity), L"-inf");
  } else {
    std::swprintf(out, static_cast<std::size_t>(capacity), L"%+.1f", static_cast<double>(db));
  }
}

// Mixer VU meter scale: ONE linear -60..+12 dB map (72 dB span) shared by the
// segment geometry and the printed marks, so the top of the bar really is +12.
inline constexpr float kMeterMinDb = -60.f;
inline constexpr float kMeterMaxDb = 12.f;

// dB at the centre of segment `seg`, for 0 <= seg < count. The bottom centre
// sits just above -60 and the top centre just below +12 (12 - 36/count).
inline float MeterDbAt(int seg, int count) {
  return kMeterMinDb +
         (static_cast<float>(seg) + 0.5f) / static_cast<float>(count) * (kMeterMaxDb - kMeterMinDb);
}

// Position of a printed mark on that same map, measured from the bottom:
// t = (markDb + 60) / 72, so -60 -> 0 and +12 -> 1.
inline float MeterMarkT(int markDb) {
  return (static_cast<float>(markDb) - kMeterMinDb) / (kMeterMaxDb - kMeterMinDb);
}

// Clamps a level onto the meter scale. NaN is the only value that survives the
// min/max clamps unchanged (it compares false in every direction), so it is
// caught first and shown as the neutral 0 dB - a non-finite reading must never
// reach the bar as NaN; +/-inf already falls out of the clamps as the scale ends.
inline float MeterClampDb(float db) {
  if (db != db) {  // true only for NaN
    return 0.f;
  }
  return std::max(kMeterMinDb, std::min(kMeterMaxDb, db));
}

}  // namespace anymix::ui
