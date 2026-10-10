#include "testsupport.h"

#include <cmath>
#include <limits>

#include "../include/UiMath.h"

namespace {

// Real segment counts of the mixer strip meter: DrawMeter sees
// height = meterH - 4 (2 px inset top and bottom) at pitch 4 px (3 px segment
// + 1 px gap), so meterH 150 (compact) / 210 (regular) => 36 / 51 segments.
constexpr int kCompactCount = (150 - 4) / 4;
constexpr int kRegularCount = (210 - 4) / 4;

bool StrictlyIncreasing(int count) {
  float previous = anymix::ui::MeterDbAt(0, count);
  for (int i = 1; i < count; ++i) {
    const float db = anymix::ui::MeterDbAt(i, count);
    if (!(db > previous)) {  // also rejects NaN, which compares false everywhere
      return false;
    }
    previous = db;
  }
  return true;
}

}  // namespace

void RunMeterTests() {
  // One coherent scale: -60..+12 dB, 72 dB span, shared by segments and marks.
  Expect(anymix::ui::kMeterMinDb == -60.f && anymix::ui::kMeterMaxDb == 12.f,
         "the meter scale spans -60 to +12 dB");

  // MeterDbAt: segment centres rise strictly monotonically bottom to top.
  Expect(StrictlyIncreasing(10), "MeterDbAt rises segment by segment at count 10");
  Expect(StrictlyIncreasing(33), "MeterDbAt rises segment by segment at count 33");
  Expect(StrictlyIncreasing(70), "MeterDbAt rises segment by segment at count 70");

  // Bottom of the scale: the first segment centre sits just above the floor.
  // The (-60, -59) window holds for count >= 37, so the representative counts
  // are 70 (brief) and 7200 (limit used by the inverse-consistency check).
  const float bottom70 = anymix::ui::MeterDbAt(0, 70);
  Expect(bottom70 > -60.f && bottom70 < -59.f,
         "MeterDbAt bottom segment sits just above -60 at count 70");
  const float bottom7200 = anymix::ui::MeterDbAt(0, 7200);
  Expect(bottom7200 > -60.f && bottom7200 < -59.f,
         "MeterDbAt bottom segment sits just above -60 at count 7200");

  // Top of the scale - the mission assertion: the top segment must land in the
  // +12 region. The old -60..0 map put it just BELOW 0 (about -0.8 dB) at any
  // count; 12 - 36/count exceeds 11.5 only for count > 72, hence 7200 here and
  // the discriminating > 10.5 bound on the real 36/51-segment meters below.
  Expect(anymix::ui::MeterDbAt(7199, 7200) > 11.5f,
         "MeterDbAt top segment is in the +12 region at count 7200");
  const float topCompact = anymix::ui::MeterDbAt(kCompactCount - 1, kCompactCount);
  const float topRegular = anymix::ui::MeterDbAt(kRegularCount - 1, kRegularCount);
  Expect(topCompact > 10.5f && topRegular > 10.5f,
         "MeterDbAt top segment of the real 36/51-segment meter is far above the old 0 dB");

  // MeterMarkT: the printed marks sit on the same 72 dB map, measured from
  // the bottom (t = 0 at -60, t = 1 at +12).
  Expect(anymix::ui::MeterMarkT(12) == 1.f, "MeterMarkT maps +12 to the top of the meter");
  Expect(anymix::ui::MeterMarkT(-60) == 0.f, "MeterMarkT maps -60 to the bottom of the meter");
  Expect(std::fabs(anymix::ui::MeterMarkT(0) - 60.f / 72.f) <= 1e-5f,
         "MeterMarkT maps 0 dB to 60/72");
  Expect(std::fabs(anymix::ui::MeterMarkT(6) - 66.f / 72.f) <= 1e-5f,
         "MeterMarkT maps +6 dB to 66/72");
  Expect(std::fabs(anymix::ui::MeterMarkT(-12) - 48.f / 72.f) <= 1e-5f,
         "MeterMarkT maps -12 dB to 48/72");

  // Marks and segments share ONE map: as count grows, the segment map's
  // position for a mark converges to MeterMarkT(mark).
  const int marks[7] = {12, 6, 0, -6, -12, -30, -60};
  const float span = anymix::ui::kMeterMaxDb - anymix::ui::kMeterMinDb;
  const float bottom = anymix::ui::MeterDbAt(0, 7200);
  bool sharedMap = true;
  for (int mark : marks) {
    const float fromSegments = (static_cast<float>(mark) - bottom) / span;
    if (std::fabs(fromSegments - anymix::ui::MeterMarkT(mark)) > 2e-3f) {
      sharedMap = false;
    }
  }
  Expect(sharedMap, "every mark sits where the segment map puts it at count 7200");

  // MeterClampDb: levels clamp to the scale ends, interior values pass through.
  Expect(anymix::ui::MeterClampDb(12.5f) == 12.f, "MeterClampDb clamps above +12 to +12");
  Expect(anymix::ui::MeterClampDb(-60.5f) == -60.f, "MeterClampDb clamps below -60 to -60");
  Expect(anymix::ui::MeterClampDb(0.f) == 0.f, "MeterClampDb keeps 0 dB unchanged");
  Expect(anymix::ui::MeterClampDb(7.3f) == 7.3f, "MeterClampDb keeps an interior level unchanged");

  // A non-finite reading must never reach the bar: same policy as the rest of
  // the fix series - a non-finite value is shown as the neutral 0 dB.
  const float nanDb = std::numeric_limits<float>::quiet_NaN();
  Expect(std::isfinite(anymix::ui::MeterClampDb(nanDb)), "MeterClampDb never returns NaN");
  Expect(anymix::ui::MeterClampDb(nanDb) == 0.f, "MeterClampDb maps NaN to the neutral 0 dB");
}
