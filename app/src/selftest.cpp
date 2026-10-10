#include <windows.h>

#include "anymix/Devices.h"
#include "anymix/Engine.h"
#include "anymix/NoiseGate.h"
#include "anymix/Process.h"
#include "testsupport.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

float Rms(const float* data, int n) {
  double sum = 0.0;
  for (int i = 0; i < n; ++i) {
    sum += static_cast<double>(data[i]) * data[i];
  }
  return static_cast<float>(std::sqrt(sum / n));
}

void FillSine(float* dst, int frames, float freq, float amp, float phase) {
  for (int i = 0; i < frames; ++i) {
    const float s = amp * std::sin(phase + 2.f * 3.14159265f * freq * i / anymix::kSampleRate);
    dst[i * 2] = s;
    dst[i * 2 + 1] = s * 0.5f;
  }
}

void FillTone(float* dst, int frames, float freq, float amp) {
  for (int i = 0; i < frames; ++i) {
    const float s = amp * std::sin(2.f * 3.14159265f * freq * static_cast<float>(i) / 48000.f);
    dst[i * 2] = s;
    dst[i * 2 + 1] = s;
  }
}

void Feed(anymix::NoiseGate& gate, float* block, float freq, float amp, int blocks) {
  float phase = 0.f;
  const float step = 2.f * 3.14159265f * freq / 48000.f;
  for (int n = 0; n < blocks; ++n) {
    for (int i = 0; i < 512; ++i) {
      const float sample = amp * std::sin(phase);
      phase += step;
      if (phase > 6.2831853f) {
        phase -= 6.2831853f;
      }
      block[i * 2] = sample;
      block[i * 2 + 1] = sample;
    }
    gate.Process(block, 512);
  }
}

void TestNoiseGate() {
  using namespace anymix;
  const GateSettings off = CalculateGateSettings(0.f);
  const GateSettings light = CalculateGateSettings(2.f);
  const GateSettings vocal = CalculateGateSettings(5.f);
  const GateSettings strong = CalculateGateSettings(8.f);
  const GateSettings maxed = CalculateGateSettings(10.f);
  Expect(!off.enabled, "gate level 0 is bypass");
  Expect(light.enabled && vocal.enabled && strong.enabled && maxed.enabled, "gate levels above 0 are active");
  Expect(light.thresholdDb < vocal.thresholdDb && vocal.thresholdDb < maxed.thresholdDb,
         "threshold rises gently with the gate level");
  Expect(maxed.thresholdDb <= -22.f && maxed.thresholdDb >= -30.f,
         "level 10 gates room noise and stays under loud speech");
  Expect(light.rangeDb > strong.rangeDb && strong.rangeDb > maxed.rangeDb, "higher levels reduce the closed mic more");
  Expect(light.attackMs <= 4.f && maxed.attackMs <= 4.f && maxed.attackMs <= light.attackMs,
         "attack stays fast enough for word onsets");
  Expect(maxed.holdMs >= 80.f && light.holdMs >= maxed.holdMs, "hold covers short pauses at every level");
  Expect(maxed.releaseMs >= 80.f && light.releaseMs > maxed.releaseMs, "release stays progressive");
  const float legacy = GateLevelFromLegacy(-48.f, -42.f);
  Expect(legacy > 1.f && legacy < 5.f, "an old gentle gate stays in the lower half of the scale");

  NoiseGateParams params;
  params.enabled = true;
  params.thresholdDb = -40.f;
  params.attackMs = 3.f;
  params.holdMs = 100.f;
  params.releaseMs = 40.f;
  params.rangeDb = -60.f;
  params.bypass = false;

  float block[512 * 2];
  NoiseGate quiet;
  quiet.Prepare(48000.f);
  quiet.SetParams(params);
  bool opened = false;
  for (int n = 0; n < 25; ++n) {
    FillTone(block, 512, 1000.f, 0.001f);
    if (quiet.Process(block, 512)) {
      opened = true;
    }
  }
  Expect(!opened, "gate stays closed under the threshold");
  Expect(quiet.Gain() < 0.02f, "closed gate attenuates by its range");
  Expect(quiet.Gain() > 0.0002f, "closed gate does not mute to zero");

  NoiseGate spike;
  spike.Prepare(48000.f);
  spike.SetParams(params);
  FillTone(block, 512, 1000.f, 0.f);
  spike.Process(block, 512);
  FillTone(block, 512, 1000.f, 0.f);
  block[20] = 0.15f;
  block[21] = 0.15f;
  Expect(!spike.Process(block, 512), "an isolated spike does not open the gate");

  NoiseGate speech;
  speech.Prepare(48000.f);
  speech.SetParams(params);
  bool speechOpen = false;
  for (int n = 0; n < 4; ++n) {
    FillTone(block, 512, 1000.f, 0.1f);
    if (speech.Process(block, 512)) {
      speechOpen = true;
    }
  }
  Expect(speechOpen, "a signal above the threshold opens the gate");
  Expect(Rms(block, 512 * 2) > 0.05f, "an open gate passes the mic");

  NoiseGate fromClosed;
  fromClosed.Prepare(48000.f);
  fromClosed.SetParams(params);
  Feed(fromClosed, block, 1000.f, 0.001f, 25);
  float dry[512 * 2];
  FillTone(dry, 512, 1000.f, 0.2f);
  std::memcpy(block, dry, sizeof(dry));
  fromClosed.Process(block, 512);
  float endGain = 0.f;
  int endCount = 0;
  float maxStep = 0.f;
  float prevGain = 0.f;
  bool haveGain = false;
  for (int i = 0; i < 512; ++i) {
    if (std::fabs(dry[i * 2]) < 0.05f) {
      continue;
    }
    const float gain = block[i * 2] / dry[i * 2];
    if (i > 400) {
      endGain += gain;
      endCount += 1;
    }
    if (haveGain) {
      maxStep = std::max(maxStep, std::fabs(gain - prevGain));
    }
    prevGain = gain;
    haveGain = true;
  }
  Expect(endCount > 0 && endGain / static_cast<float>(endCount) > 0.7f, "attack opens a word without staying shut");
  Expect(maxStep < 0.05f, "attack gain moves one sample at a time");

  NoiseGate held;
  held.Prepare(48000.f);
  params.holdMs = 100.f;
  params.releaseMs = 80.f;
  held.SetParams(params);
  Feed(held, block, 1000.f, 0.1f, 2);
  bool stillOpen = true;
  for (int n = 0; n < 4; ++n) {
    FillTone(block, 512, 1000.f, 0.f);
    stillOpen = held.Process(block, 512);
  }
  Expect(stillOpen, "hold keeps the gate open through a short pause");
  for (int n = 0; n < 12; ++n) {
    FillTone(block, 512, 1000.f, 0.f);
    stillOpen = held.Process(block, 512);
  }
  Expect(!stillOpen, "the gate closes after hold and release");

  NoiseGate bypassed;
  bypassed.Prepare(48000.f);
  params.bypass = true;
  bypassed.SetParams(params);
  FillTone(dry, 512, 1000.f, 0.2f);
  dry[0] = 0.33f;
  dry[1] = 0.33f;
  std::memcpy(block, dry, sizeof(dry));
  bypassed.Process(block, 512);
  Expect(std::fabs(block[0] - dry[0]) < 0.02f, "bypass keeps the dry mic");
  Expect(!bypassed.Open(), "level bypass does not show the gate as open");

  const float quietAmp = std::pow(10.f, -62.f / 20.f);
  NoiseGate level2;
  level2.Prepare(48000.f);
  level2.SetLevel(2.f);
  Feed(level2, block, 1000.f, quietAmp, 80);
  const float gain2 = level2.Gain();
  NoiseGate level8;
  level8.Prepare(48000.f);
  level8.SetLevel(8.f);
  Feed(level8, block, 1000.f, quietAmp, 80);
  const float gain8 = level8.Gain();
  Expect(!level2.Open() && !level8.Open(), "room noise stays under the gate at level 2 and 8");
  Expect(gain2 < 0.5f && gain2 > gain8 * 4.f, "level 8 ducks steady noise harder than level 2");

  NoiseGate level0;
  level0.Prepare(48000.f);
  level0.SetLevel(0.f);
  FillTone(dry, 512, 1000.f, 0.2f);
  std::memcpy(block, dry, sizeof(dry));
  level0.Process(block, 512);
  float drift = 0.f;
  for (int i = 0; i < 512 * 2; ++i) {
    drift = std::max(drift, std::fabs(block[i] - dry[i]));
  }
  Expect(drift < 0.001f, "gate level 0 leaves the signal unchanged");

  NoiseGate level5;
  level5.Prepare(48000.f);
  level5.SetLevel(5.f);
  Feed(level5, block, 200.f, 0.1f, 8);
  Expect(level5.Open() && level5.Gain() > 0.8f, "level 5 passes a normal voice");
  bool pauseOpen = true;
  for (int n = 0; n < 3; ++n) {
    FillTone(block, 512, 200.f, 0.f);
    pauseOpen = level5.Process(block, 512);
  }
  Expect(pauseOpen, "level 5 holds through a short pause");

  NoiseGate level10;
  level10.Prepare(48000.f);
  level10.SetLevel(10.f);
  Feed(level10, block, 1000.f, quietAmp, 80);
  Expect(!level10.Open() && level10.Gain() < gain8, "level 10 reduces steady noise further");

  NoiseGate rumble;
  rumble.Prepare(48000.f);
  rumble.SetLevel(8.f);
  const float rumbleAmp = std::pow(10.f, -36.f / 20.f);
  Feed(rumble, block, 30.f, rumbleAmp, 20);
  Expect(!rumble.Open(), "level 8 ignores low rumble near the threshold");
  NoiseGate tone;
  tone.Prepare(48000.f);
  tone.SetLevel(8.f);
  const float voiceAmp = std::pow(10.f, -18.f / 20.f);
  Feed(tone, block, 1000.f, voiceAmp, 8);
  Expect(tone.Open(), "level 8 opens on a normal voice and ignores the rumble");
}

void TestLimiter() {
  using namespace anymix;
  float block[kBlockFrames * 2];
  float dry[kBlockFrames * 2];
  PeakLimit state;
  float phase = 0.f;
  const float step = 2.f * 3.14159265f * 80.f / static_cast<float>(kSampleRate);
  auto fill = [&](float amp) {
    for (int i = 0; i < kBlockFrames; ++i) {
      const float s = amp * std::sin(phase);
      phase += step;
      if (phase > 6.2831853f) {
        phase -= 6.2831853f;
      }
      block[i * 2] = s;
      block[i * 2 + 1] = s;
    }
  };
  for (int n = 0; n < 40; ++n) {
    fill(4.f);
    LimitStereo(block, kBlockFrames, state);
  }
  fill(4.f);
  std::memcpy(dry, block, sizeof(dry));
  LimitStereo(block, kBlockFrames, state);
  double sum = 0.0;
  double sumSq = 0.0;
  int count = 0;
  float peak = 0.f;
  for (int i = 0; i < kBlockFrames * 2; ++i) {
    peak = std::max(peak, std::fabs(block[i]));
    if (std::fabs(dry[i]) > 2.f) {
      const double g = static_cast<double>(block[i]) / static_cast<double>(dry[i]);
      sum += g;
      sumSq += g * g;
      ++count;
    }
  }
  const double mean = count > 0 ? sum / count : 0.0;
  const double var = count > 0 ? sumSq / count - mean * mean : 1.0;
  Expect(peak <= 0.991f, "full volume stays under the ceiling");
  Expect(count > 100 && var < 1.0e-6, "full volume keeps the waveform shape");

  for (int n = 0; n < 160; ++n) {
    fill(0.25f);
    LimitStereo(block, kBlockFrames, state);
  }
  fill(0.25f);
  std::memcpy(dry, block, sizeof(dry));
  LimitStereo(block, kBlockFrames, state);
  float maxErr = 0.f;
  for (int i = 0; i < kBlockFrames * 2; ++i) {
    maxErr = std::max(maxErr, std::fabs(block[i] - dry[i]));
  }
  Expect(maxErr < 0.002f, "a normal level is left untouched once the limiter lets go");
}

void TestDsp() {
  using namespace anymix;
  float input[kBlockFrames * 2];
  float shapedStore[kStrips][kBlockFrames * 2];
  float busStore[kBuses][kBlockFrames * 2];
  float* shaped[kStrips];
  float* buses[kBuses];
  const float* inputs[kStrips];
  for (int i = 0; i < kStrips; ++i) {
    shaped[i] = shapedStore[i];
    inputs[i] = input;
  }
  for (int i = 0; i < kBuses; ++i) {
    buses[i] = busStore[i];
  }
  FillSine(input, kBlockFrames, 1000.f, 0.5f, 0.f);

  BlockParams params[kStrips]{};
  StripEq eq[kStrips];
  params[kMic1].gain = 1.f;
  params[kMic1].audible = true;
  params[kMic1].route = 1u;
  params[kMic1].bass = MakeLowShelf(0.f, 150.f, static_cast<float>(kSampleRate));
  params[kMic1].treble = MakeHighShelf(0.f, 6000.f, static_cast<float>(kSampleRate));
  ShapeStrips(inputs, kBlockFrames, params, eq, shaped);
  MixToBuses(shaped, kBlockFrames, params, buses);
  const float dry = Rms(input, kBlockFrames * 2);
  const float wet = Rms(busStore[0], kBlockFrames * 2);
  Expect(std::fabs(dry - wet) < 0.02f, "unity EQ and 0 dB fader keep the level");

  params[kMic1].gain = 0.5f;
  ShapeStrips(inputs, kBlockFrames, params, eq, shaped);
  MixToBuses(shaped, kBlockFrames, params, buses);
  Expect(std::fabs(Rms(busStore[0], kBlockFrames * 2) - dry * 0.5f) < 0.03f, "fader scales the signal");

  params[kMic1].gain = 1.f;
  params[kMic1].audible = false;
  ShapeStrips(inputs, kBlockFrames, params, eq, shaped);
  MixToBuses(shaped, kBlockFrames, params, buses);
  Expect(Rms(busStore[0], kBlockFrames * 2) == 0.f, "mute cuts the bus");

  params[kMic1].audible = true;
  params[kMic1].mono = true;
  params[kMic1].route = 1u;
  ShapeStrips(inputs, kBlockFrames, params, eq, shaped);
  bool monoOk = true;
  for (int i = 0; i < kBlockFrames; ++i) {
    if (shapedStore[kMic1][i * 2] != shapedStore[kMic1][i * 2 + 1]) {
      monoOk = false;
    }
  }
  Expect(monoOk, "mono sums left and right");

  params[kMic1].mono = false;
  params[kMic1].route = 1u << 7;  // B3
  ShapeStrips(inputs, kBlockFrames, params, eq, shaped);
  MixToBuses(shaped, kBlockFrames, params, buses);
  Expect(Rms(busStore[0], kBlockFrames * 2) == 0.f, "A1 stays silent when only B3 is routed");
  Expect(Rms(busStore[7], kBlockFrames * 2) > 0.1f, "B3 receives the routed signal");

  BlockParams soloParams[kStrips]{};
  soloParams[kMic1] = params[kMic1];
  soloParams[kMic1].route = 1u;
  soloParams[kMic1].audible = false;
  soloParams[kMic2].gain = 1.f;
  soloParams[kMic2].audible = true;
  soloParams[kMic2].route = 1u;
  soloParams[kMic2].bass = params[kMic1].bass;
  soloParams[kMic2].treble = params[kMic1].treble;
  const float* soloIn[kStrips];
  for (int i = 0; i < kStrips; ++i) {
    soloIn[i] = nullptr;
  }
  soloIn[kMic2] = input;
  ShapeStrips(soloIn, kBlockFrames, soloParams, eq, shaped);
  MixToBuses(shaped, kBlockFrames, soloParams, buses);
  Expect(Rms(busStore[0], kBlockFrames * 2) > 0.1f, "solo keeps the audible strip");

  float low[kBlockFrames * 2];
  FillSine(low, kBlockFrames, 80.f, 0.25f, 0.2f);
  inputs[kMic1] = low;
  params[kMic1].bass = MakeLowShelf(9.f, 150.f, static_cast<float>(kSampleRate));
  params[kMic1].audible = true;
  params[kMic1].mono = false;
  params[kMic1].gain = 1.f;
  params[kMic1].route = 1u;
  StripEq boosted[kStrips];
  ShapeStrips(inputs, kBlockFrames, params, boosted, shaped);
  const float boostedRms = Rms(shapedStore[kMic1], kBlockFrames * 2);
  const float lowRms = Rms(low, kBlockFrames * 2);
  Expect(boostedRms > lowRms * 1.4f, "bass shelf raises a low sine");
}

void TestGainPositive() {
  using namespace anymix;
  float input[kBlockFrames * 2];
  float shapedStore[kStrips][kBlockFrames * 2];
  float busStore[kBuses][kBlockFrames * 2];
  float* shaped[kStrips];
  float* buses[kBuses];
  const float* inputs[kStrips];
  for (int i = 0; i < kStrips; ++i) {
    shaped[i] = shapedStore[i];
    inputs[i] = input;
  }
  for (int i = 0; i < kBuses; ++i) {
    buses[i] = busStore[i];
  }
  FillSine(input, kBlockFrames, 1000.f, 0.5f, 0.f);

  BlockParams params[kStrips]{};
  StripEq eq[kStrips];
  params[kMic1].gain = 1.f;
  params[kMic1].audible = true;
  params[kMic1].route = 1u;
  params[kMic1].bass = MakeLowShelf(0.f, 150.f, static_cast<float>(kSampleRate));
  params[kMic1].treble = MakeHighShelf(0.f, 6000.f, static_cast<float>(kSampleRate));

  // Test +6 dB : le signal doit être amplifié par ~1.995
  params[kMic1].gain = std::pow(10.f, 6.f / 20.f);  // +6 dB = ~1.995
  ShapeStrips(inputs, kBlockFrames, params, eq, shaped);
  MixToBuses(shaped, kBlockFrames, params, buses);
  const float dry = Rms(input, kBlockFrames * 2);
  const float wet = Rms(busStore[0], kBlockFrames * 2);
  const float expected = dry * std::pow(10.f, 6.f / 20.f);
  Expect(std::fabs(wet - expected) < 0.05f, "+6 dB amplifies the signal (not limited)");

  // Test +12 dB : le signal doit être amplifié par ~3.981
  params[kMic1].gain = std::pow(10.f, 12.f / 20.f);  // +12 dB = ~3.981
  ShapeStrips(inputs, kBlockFrames, params, eq, shaped);
  MixToBuses(shaped, kBlockFrames, params, buses);
  const float wet12 = Rms(busStore[0], kBlockFrames * 2);
  const float expected12 = dry * std::pow(10.f, 12.f / 20.f);
  Expect(std::fabs(wet12 - expected12) < 0.1f, "+12 dB amplifies the signal (not limited)");

  // Test +3 dB : le signal doit être amplifié par ~1.413
  params[kMic1].gain = std::pow(10.f, 3.f / 20.f);  // +3 dB = ~1.413
  ShapeStrips(inputs, kBlockFrames, params, eq, shaped);
  MixToBuses(shaped, kBlockFrames, params, buses);
  const float wet3 = Rms(busStore[0], kBlockFrames * 2);
  const float expected3 = dry * std::pow(10.f, 3.f / 20.f);
  Expect(std::fabs(wet3 - expected3) < 0.03f, "+3 dB amplifies the signal (not limited)");

  // Test -6 dB : le signal doit être réduit par ~0.501
  params[kMic1].gain = std::pow(10.f, -6.f / 20.f);  // -6 dB = ~0.501
  ShapeStrips(inputs, kBlockFrames, params, eq, shaped);
  MixToBuses(shaped, kBlockFrames, params, buses);
  const float wetNeg = Rms(busStore[0], kBlockFrames * 2);
  const float expectedNeg = dry * std::pow(10.f, -6.f / 20.f);
  Expect(std::fabs(wetNeg - expectedNeg) < 0.03f, "-6 dB reduces the signal");

  // Test 0 dB : le signal doit rester identique
  params[kMic1].gain = 1.f;
  ShapeStrips(inputs, kBlockFrames, params, eq, shaped);
  MixToBuses(shaped, kBlockFrames, params, buses);
  const float wet0 = Rms(busStore[0], kBlockFrames * 2);
  Expect(std::fabs(wet0 - dry) < 0.02f, "0 dB keeps the signal unchanged");

  // Test avec limiteur : +6 dB avec signal faible (0.4) ne doit pas être compressé
  // Le signal après +6 dB = 0.4 * 1.995 = 0.798, sous le plafond de 0.98
  {
    float lowInput[kBlockFrames * 2];
    FillSine(lowInput, kBlockFrames, 1000.f, 0.4f, 0.f);
    const float* lowInputs[kStrips];
    for (int i = 0; i < kStrips; ++i) {
      lowInputs[i] = lowInput;
    }
    params[kMic1].gain = std::pow(10.f, 6.f / 20.f);  // +6 dB = ~1.995
    ShapeStrips(lowInputs, kBlockFrames, params, eq, shaped);
    MixToBuses(shaped, kBlockFrames, params, buses);
    const float lowWet = Rms(busStore[0], kBlockFrames * 2);
    const float lowExpected = Rms(lowInput, kBlockFrames * 2) * std::pow(10.f, 6.f / 20.f);
    Expect(std::fabs(lowWet - lowExpected) < 0.05f, "+6 dB with low signal amplifies correctly");

    float limitedLow[kBlockFrames * 2];
    std::memcpy(limitedLow, busStore[0], sizeof(limitedLow));
    PeakLimit lowLimitState;
    LimitStereo(limitedLow, kBlockFrames, lowLimitState);
    const float limitedLowRms = Rms(limitedLow, kBlockFrames * 2);
    Expect(limitedLowRms > lowWet * 0.95f, "limiter transparent with +6 dB and low signal");
  }
}

void TestAsio() {
  const HMODULE dll = LoadLibraryW(L"build\\AnyMixInsertASIO.dll");
  Expect(dll != nullptr, "AnyMix Insert ASIO DLL loads");
  if (dll == nullptr) {
    return;
  }
  using CheckFn = int (*)();
  auto check = reinterpret_cast<CheckFn>(GetProcAddress(dll, "AnyMix_CheckInsert"));
  Expect(check != nullptr, "ASIO DLL exports a real insert check");
  if (check != nullptr) {
    const int code = check();
    std::printf("asio check=%d\n", code);
    Expect(code == 0, "ASIO insert exchanges a real stereo buffer");
  }
  FreeLibrary(dll);
}

void TestHardware() {
  using namespace anymix;
  wchar_t renderId[512] = {};
  wchar_t captureId[512] = {};
  wchar_t renderName[256] = {};
  wchar_t captureName[256] = {};
  const bool hasRender = DefaultRenderId(renderId, 512, renderName, 256);
  const bool hasCapture = DefaultCaptureId(captureId, 512, captureName, 256);
  Expect(hasRender, "default Windows render endpoint exists");
  Expect(hasCapture, "default Windows capture endpoint exists");
  if (!hasRender) {
    return;
  }
  std::wprintf(L"Render: %s\n", renderName);
  if (hasCapture) {
    std::wprintf(L"Capture: %s\n", captureName);
  }

  Engine engine;
  engine.SetRenderDevice(0, renderId);
  if (hasCapture) {
    engine.SetCaptureDevice(0, captureId);
  }
  engine.SetGainDb(kMic1, 0.f);
  engine.SetRoute(kMic1, 0, true);
  std::wstring error;
  Expect(engine.Start(error), "engine thread starts");
  if (!error.empty()) {
    std::wprintf(L"%s\n", error.c_str());
  }
  Sleep(1500);
  const EngineStats stats = engine.Stats();
  std::printf("blocks=%llu captured=%llu rendered=%llu\n",
              static_cast<unsigned long long>(stats.blocks),
              static_cast<unsigned long long>(stats.framesCaptured),
              static_cast<unsigned long long>(stats.framesRendered));
  Expect(stats.blocks > 40, "audio thread produced blocks");
  if (stats.framesRendered <= 1000 || stats.framesCaptured <= 1000) {
    std::wprintf(L"Device error: %ls\n", engine.DeviceError());
  }
  Expect(stats.framesRendered > 1000, "A1 received real render buffers");
  if (hasCapture) {
    Expect(stats.framesCaptured > 1000, "microphone capture delivered buffers");
  }

  wchar_t folder[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, folder);
  if (folder[0] != 0 && folder[wcslen(folder) - 1] == L'\\') {
    folder[wcslen(folder) - 1] = 0;
  }
  std::wstring recError;
  const bool recording = engine.StartRecording(0, folder, recError);
  Expect(recording, "WAV recorder opens on A1");
  Sleep(400);
  engine.StopRecording();
  engine.Stop();
  if (recording) {
    const wchar_t* path = engine.RecordingPath();
    WIN32_FILE_ATTRIBUTE_DATA info{};
    const bool exists = path != nullptr && GetFileAttributesExW(path, GetFileExInfoStandard, &info);
    Expect(exists && info.nFileSizeLow > 44, "WAV file contains audio data");
    std::wprintf(L"WAV: %s (%lu bytes)\n", path, info.nFileSizeLow);
    DeleteFileW(path);
  }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  TestDsp();
  TestGainPositive();
  TestNoiseGate();
  TestLimiter();
  RunGainTests();
  RunEngineTests();
  RunUiMathTests();
  RunMeterTests();
  if (argc > 1 && argv[1] != nullptr && wcscmp(argv[1], L"dsp") == 0) {
    std::printf(TestFailures() == 0 ? "ALL PASSED\n" : "FAILURES %d\n", TestFailures());
    return TestFailures() == 0 ? 0 : 1;
  }
  TestAsio();
  TestHardware();
  std::printf(TestFailures() == 0 ? "ALL PASSED\n" : "FAILURES %d\n", TestFailures());
  return TestFailures() == 0 ? 0 : 1;
}
