#include "testsupport.h"

#include <cmath>
#include <cwchar>

#include "../include/UiMath.h"

namespace {

void ExpectNear(float got, float want, const char* name) {
  Expect(std::fabs(got - want) <= 1e-4f, name);
}

void ExpectText(const wchar_t* got, const wchar_t* want, const char* name) {
  Expect(got != nullptr && wcscmp(got, want) == 0, name);
}

}  // namespace

void RunUiMathTests() {
  wchar_t out[16];

  ExpectNear(anymix::ui::FaderTFromDb(0.f, -60.f, 24.f), 0.714286f,
             "FaderTFromDb puts 0 dB at the unity position");
  Expect(anymix::ui::FaderTFromDb(-60.f, -60.f, 24.f) == 0.f, "FaderTFromDb maps the minimum dB to 0");
  Expect(anymix::ui::FaderTFromDb(24.f, -60.f, 24.f) == 1.f, "FaderTFromDb maps the maximum dB to 1");
  Expect(anymix::ui::FaderTFromDb(30.f, -60.f, 24.f) == 1.f, "FaderTFromDb clamps above the maximum to 1");
  Expect(anymix::ui::FaderTFromDb(-70.f, -60.f, 24.f) == 0.f, "FaderTFromDb clamps below the minimum to 0");

  const float samples[] = {-70.f, -60.f, -45.f, -12.5f, 0.f, 3.f, 12.f, 24.f, 30.f};
  const int count = static_cast<int>(sizeof(samples) / sizeof(samples[0]));
  bool monotonic = true;
  float previous = anymix::ui::FaderTFromDb(samples[0], -60.f, 24.f);
  for (int i = 1; i < count; ++i) {
    const float t = anymix::ui::FaderTFromDb(samples[i], -60.f, 24.f);
    if (t < previous) {
      monotonic = false;
    }
    previous = t;
  }
  Expect(monotonic, "FaderTFromDb is monotonic across sample points");

  anymix::ui::FormatGainDb(0.f, out, 16);
  ExpectText(out, L"+0.0", "FormatGainDb prints +0.0 at unity");
  anymix::ui::FormatGainDb(-12.5f, out, 16);
  ExpectText(out, L"-12.5", "FormatGainDb prints a negative gain");
  anymix::ui::FormatGainDb(3.f, out, 16);
  ExpectText(out, L"+3.0", "FormatGainDb prints a positive gain");
  anymix::ui::FormatGainDb(-60.f, out, 16);
  ExpectText(out, L"-inf", "FormatGainDb prints -inf at the fader floor");
  anymix::ui::FormatGainDb(-60.5f, out, 16);
  ExpectText(out, L"-inf", "FormatGainDb prints -inf below the fader floor");
  anymix::ui::FormatGainDb(24.f, out, 16);
  ExpectText(out, L"+24.0", "FormatGainDb prints +24.0 at the fader ceiling");
}
