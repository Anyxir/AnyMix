#pragma once

#include <atomic>
#include <cstdint>

namespace anymix {

constexpr int kSampleRate = 48000;
constexpr int kBlockFrames = 512;
constexpr int kChannels = 2;
constexpr float kFaderMinDb = -60.f;
constexpr float kFaderMaxDb = 24.f;

constexpr int kVirtualStrips = 5;
constexpr int kMicStrips = 2;
constexpr int kStrips = kVirtualStrips + kMicStrips;
constexpr int kPhysicalBuses = 5;
constexpr int kVirtualBuses = 5;
constexpr int kBuses = kPhysicalBuses + kVirtualBuses;

constexpr int kGame = 0;
constexpr int kChat = 1;
constexpr int kMusic = 2;
constexpr int kAux = 3;
constexpr int kAdd = 4;
constexpr int kMic1 = 5;
constexpr int kMic2 = 6;

enum Strip : int {
  StripGame = kGame,
  StripChat = kChat,
  StripMusic = kMusic,
  StripAux = kAux,
  StripAdd = kAdd,
  StripMic1 = kMic1,
  StripMic2 = kMic2
};

inline constexpr const wchar_t* kStripNames[kStrips] = {
    L"Game", L"Chat", L"Music", L"Aux", L"Add", L"Mic 1", L"Mic 2"};

inline constexpr const wchar_t* kVirtualDeviceNames[kVirtualStrips] = {
    L"AnyMix Game", L"AnyMix Chat", L"AnyMix Music", L"AnyMix Aux", L"AnyMix Add"};

inline constexpr const wchar_t* kCaptureDeviceNames[kVirtualBuses] = {
    L"AnyMix B1", L"AnyMix B2", L"AnyMix B3", L"AnyMix B4", L"AnyMix B5"};

inline constexpr const wchar_t* kBusNames[kBuses] = {
    L"A1", L"A2", L"A3", L"A4", L"A5", L"B1", L"B2", L"B3", L"B4", L"B5"};

struct BiquadCoeffs {
  float b0 = 1.f;
  float b1 = 0.f;
  float b2 = 0.f;
  float a1 = 0.f;
  float a2 = 0.f;
};

struct BiquadState {
  float x1 = 0.f;
  float x2 = 0.f;
  float y1 = 0.f;
  float y2 = 0.f;
};

struct PublishedEq {
  std::atomic<uint32_t> seq{0};
  BiquadCoeffs bass{};
  BiquadCoeffs treble{};
};

struct DeviceInfo {
  wchar_t id[512]{};
  wchar_t name[256]{};
};

struct SessionInfo {
  wchar_t name[256]{};
  unsigned long sessionId = 0;
  bool muted = false;
  float volume = 1.f;
  float peak = 0.f;
};

}  // namespace anymix
