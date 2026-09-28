#include "anymix/Process.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kFrames = 48000;
constexpr float kDb18 = 0.12589254117941673f;  // 10^(-18/20)

struct Stats {
  double peak = 0;
  uint64_t above = 0;
  uint64_t below = 0;
  uint64_t nans = 0;
  uint64_t infs = 0;
  uint64_t samples = 0;
};

void Accumulate(Stats& s, const float* data, int count) {
  for (int i = 0; i < count; ++i) {
    const float v = data[i];
    s.samples += 1;
    if (std::isnan(v)) {
      s.nans += 1;
      continue;
    }
    if (std::isinf(v)) {
      s.infs += 1;
      continue;
    }
    const float a = std::fabs(v);
    if (a > s.peak) {
      s.peak = a;
    }
    if (v > 1.f) {
      s.above += 1;
    }
    if (v < -1.f) {
      s.below += 1;
    }
  }
}

void PrintStats(const char* name, const Stats& s) {
  const double dbfs = s.peak > 0 ? 20.0 * std::log10(s.peak) : -200.0;
  std::printf(
      "  %-22s peak=%8.5f (%7.2f dBFS) maxAbs=%8.5f >1=%llu <-1=%llu nan=%llu inf=%llu n=%llu\n",
      name, s.peak, dbfs, s.peak, static_cast<unsigned long long>(s.above),
      static_cast<unsigned long long>(s.below), static_cast<unsigned long long>(s.nans),
      static_cast<unsigned long long>(s.infs), static_cast<unsigned long long>(s.samples));
}

void FillSine(float* dst, int frames, float hz, float amp, float phase) {
  for (int i = 0; i < frames; ++i) {
    const float w = phase + 2.f * 3.14159265358979323846f * hz * static_cast<float>(i) /
                                 static_cast<float>(anymix::kSampleRate);
    const float s = std::sin(w) * amp;
    dst[i * 2] = s;
    dst[i * 2 + 1] = s;
  }
}

struct Probe {
  Stats before;
  Stats afterEq;
  Stats afterGain;
};

void ShapeTap(const float* in, int frames, const anymix::BlockParams& p, anymix::StripEq& eq, float* afterEq,
              float* afterGain) {
  for (int frame = 0; frame < frames; ++frame) {
    float left = anymix::ApplyBiquad(p.bass, eq.bass[0], in[frame * 2]);
    float right = anymix::ApplyBiquad(p.bass, eq.bass[1], in[frame * 2 + 1]);
    left = anymix::ApplyBiquad(p.treble, eq.treble[0], left);
    right = anymix::ApplyBiquad(p.treble, eq.treble[1], right);
    if (p.mono) {
      const float mid = 0.5f * (left + right);
      left = mid;
      right = mid;
    }
    afterEq[frame * 2] = left;
    afterEq[frame * 2 + 1] = right;
    afterGain[frame * 2] = left * p.gain;
    afterGain[frame * 2 + 1] = right * p.gain;
  }
}

void RunCase(const char* title, bool gameOn, bool musicOn, float gameHz, float musicHz, float amp, uint16_t route) {
  std::printf("\n== %s ==\n", title);
  float game[anymix::kBlockFrames * 2];
  float music[anymix::kBlockFrames * 2];
  float shapedStore[anymix::kStrips][anymix::kBlockFrames * 2];
  float busStore[anymix::kBuses][anymix::kBlockFrames * 2];
  float eqTap[2][anymix::kBlockFrames * 2];
  float gainTap[2][anymix::kBlockFrames * 2];
  float rawSum[2][anymix::kBlockFrames * 2];

  anymix::BlockParams params[anymix::kStrips]{};
  anymix::StripEq eq[anymix::kStrips];
  anymix::StripEq tapEq[2];
  for (int i = 0; i < anymix::kStrips; ++i) {
    params[i].gain = 1.f;
    params[i].audible = false;
    params[i].mono = false;
    params[i].route = 0;
    params[i].bass = anymix::MakeLowShelf(0.f, 150.f, static_cast<float>(anymix::kSampleRate));
    params[i].treble = anymix::MakeHighShelf(0.f, 6000.f, static_cast<float>(anymix::kSampleRate));
  }
  if (gameOn) {
    params[anymix::kGame].audible = true;
    params[anymix::kGame].route = route;
  }
  if (musicOn) {
    params[anymix::kMusic].audible = true;
    params[anymix::kMusic].route = route;
  }

  Probe strips[2];
  Stats rawA1;
  Stats rawA2;
  Stats busA1;
  Stats busA2;
  uint64_t frames = 0;
  uint64_t rewritten = 0;
  float phaseGame = 0.f;
  float phaseMusic = 0.f;

  for (int done = 0; done < kFrames; done += anymix::kBlockFrames) {
    FillSine(game, anymix::kBlockFrames, gameHz, gameOn ? amp : 0.f, phaseGame);
    FillSine(music, anymix::kBlockFrames, musicHz, musicOn ? amp : 0.f, phaseMusic);
    phaseGame += 2.f * 3.14159265358979323846f * gameHz * static_cast<float>(anymix::kBlockFrames) /
                 static_cast<float>(anymix::kSampleRate);
    phaseMusic += 2.f * 3.14159265358979323846f * musicHz * static_cast<float>(anymix::kBlockFrames) /
                  static_cast<float>(anymix::kSampleRate);

    const float* inputs[anymix::kStrips]{};
    float* shaped[anymix::kStrips];
    float* buses[anymix::kBuses];
    for (int i = 0; i < anymix::kStrips; ++i) {
      shaped[i] = shapedStore[i];
      inputs[i] = nullptr;
    }
    inputs[anymix::kGame] = game;
    inputs[anymix::kMusic] = music;
    for (int i = 0; i < anymix::kBuses; ++i) {
      buses[i] = busStore[i];
    }

    const float* sources[2] = {game, music};
    const int ids[2] = {anymix::kGame, anymix::kMusic};
    for (int s = 0; s < 2; ++s) {
      if (!params[ids[s]].audible) {
        continue;
      }
      ShapeTap(sources[s], anymix::kBlockFrames, params[ids[s]], tapEq[s], eqTap[s], gainTap[s]);
      Accumulate(strips[s].before, sources[s], anymix::kBlockFrames * 2);
      Accumulate(strips[s].afterEq, eqTap[s], anymix::kBlockFrames * 2);
      Accumulate(strips[s].afterGain, gainTap[s], anymix::kBlockFrames * 2);
    }

    anymix::ShapeStrips(inputs, anymix::kBlockFrames, params, eq, shaped);
    std::memset(rawSum[0], 0, sizeof(rawSum[0]));
    std::memset(rawSum[1], 0, sizeof(rawSum[1]));
    for (int strip = 0; strip < anymix::kStrips; ++strip) {
      const uint16_t r = params[strip].route;
      if (r == 0 || !params[strip].audible) {
        continue;
      }
      for (int bus = 0; bus < 2; ++bus) {
        if ((r & static_cast<uint16_t>(1u << bus)) == 0) {
          continue;
        }
        for (int i = 0; i < anymix::kBlockFrames * 2; ++i) {
          rawSum[bus][i] += shaped[strip][i];
        }
      }
    }
    Accumulate(rawA1, rawSum[0], anymix::kBlockFrames * 2);
    Accumulate(rawA2, rawSum[1], anymix::kBlockFrames * 2);

    anymix::MixToBuses(shaped, anymix::kBlockFrames, params, buses);
    Accumulate(busA1, busStore[0], anymix::kBlockFrames * 2);
    Accumulate(busA2, busStore[1], anymix::kBlockFrames * 2);
    for (int bus = 0; bus < 2; ++bus) {
      if ((route & static_cast<uint16_t>(1u << bus)) == 0 && !gameOn && !musicOn) {
        continue;
      }
      for (int i = 0; i < anymix::kBlockFrames * 2; ++i) {
        if (std::fabs(busStore[bus][i] - rawSum[bus][i]) > 1.0e-6f) {
          rewritten += 1;
        }
      }
    }
    frames += anymix::kBlockFrames;
  }

  std::printf("  frames=%llu  EQ=bypass (0 dB)  gain=0 dB  mono=off  solo=off  route=0x%X\n",
              static_cast<unsigned long long>(frames), route);
  std::printf("  samples rewritten by limiter (bus != raw sum)=%llu\n",
              static_cast<unsigned long long>(rewritten));
  if (gameOn) {
    std::printf("  GAME\n");
    PrintStats("avant DSP", strips[0].before);
    PrintStats("apres EQ", strips[0].afterEq);
    PrintStats("apres gain", strips[0].afterGain);
  }
  if (musicOn) {
    std::printf("  MUSIC\n");
    PrintStats("avant DSP", strips[1].before);
    PrintStats("apres EQ", strips[1].afterEq);
    PrintStats("apres gain", strips[1].afterGain);
  }
  PrintStats("somme brute A1", rawA1);
  PrintStats("bus A1 MixToBuses", busA1);
  PrintStats("somme brute A2", rawA2);
  PrintStats("bus A2 MixToBuses", busA2);
  const double expect = (gameOn ? amp : 0.f) + (musicOn && gameHz == musicHz ? amp : 0.f);
  if (gameOn && musicOn && gameHz == musicHz) {
    std::printf("  somme coherente attendue = %.5f (%.2f dBFS), depasse ±1 = %s\n", expect,
                20.0 * std::log10(expect), expect > 1.0 ? "OUI" : "non");
  }
}

}  // namespace

int main() {
  std::printf("Mix measure only. Process.cpp unchanged. No buffer, timer, ring, or driver.\n");
  std::printf("1 s at 48 kHz, blocks of 512. Amplitude -18 dBFS = %.6f\n", kDb18);
  RunCase("1 GAME -> A1  1 kHz -18 dBFS", true, false, 1000.f, 1000.f, kDb18, 0x1);
  RunCase("2 MUSIC -> A1  1 kHz -18 dBFS", false, true, 1000.f, 1000.f, kDb18, 0x1);
  RunCase("3 GAME+MUSIC -> A1  1 kHz -18 dBFS", true, true, 1000.f, 1000.f, kDb18, 0x1);
  RunCase("4 GAME+MUSIC -> A1+A2  1 kHz -18 dBFS", true, true, 1000.f, 1000.f, kDb18, 0x3);
  RunCase("ref deux sinus 1 kHz -18 dBFS -> A1", true, true, 1000.f, 1000.f, kDb18, 0x1);
  RunCase("ref 1 kHz + 440 Hz -18 dBFS -> A1", true, true, 1000.f, 440.f, kDb18, 0x1);
  RunCase("extra deux sinus 1 kHz 0 dBFS -> A1", true, true, 1000.f, 1000.f, 1.f, 0x1);
  return 0;
}
