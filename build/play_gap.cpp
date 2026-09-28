#include "anymix/Devices.h"

#include <audioclient.h>
#include <cmath>
#include <cstdio>
#include <mmdeviceapi.h>
#include <windows.h>

namespace {

constexpr float kLevel = 0.12589254117941673f;

struct Play {
  IAudioClient* client = nullptr;
  IAudioRenderClient* render = nullptr;
  UINT32 buffer = 0;
  float phase = 0.f;
  float hz = 440.f;
  bool on = false;
};

bool OpenPlay(const wchar_t* id, Play& play) {
  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&enumerator)))) {
    return false;
  }
  IMMDevice* device = nullptr;
  const HRESULT found = enumerator->GetDevice(id, &device);
  enumerator->Release();
  if (FAILED(found) || device == nullptr) {
    return false;
  }
  if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&play.client)))) {
    device->Release();
    return false;
  }
  device->Release();
  WAVEFORMATEX format{};
  format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
  format.nChannels = 2;
  format.nSamplesPerSec = 48000;
  format.wBitsPerSample = 32;
  format.nBlockAlign = 8;
  format.nAvgBytesPerSec = 48000 * 8;
  if (FAILED(play.client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, &format, nullptr))) {
    return false;
  }
  if (FAILED(play.client->GetBufferSize(&play.buffer))) {
    return false;
  }
  return SUCCEEDED(play.client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&play.render)));
}

void WriteSine(Play& play) {
  if (!play.on || play.client == nullptr) {
    return;
  }
  UINT32 padding = 0;
  if (FAILED(play.client->GetCurrentPadding(&padding)) || padding >= play.buffer) {
    return;
  }
  const UINT32 frames = play.buffer - padding;
  BYTE* data = nullptr;
  if (FAILED(play.render->GetBuffer(frames, &data)) || data == nullptr) {
    return;
  }
  float* samples = reinterpret_cast<float*>(data);
  for (UINT32 i = 0; i < frames; ++i) {
    const float s = std::sin(play.phase) * kLevel;
    play.phase += 2.f * 3.14159265f * play.hz / 48000.f;
    samples[i * 2] = s;
    samples[i * 2 + 1] = s;
  }
  play.render->ReleaseBuffer(frames, 0);
}

void Start(Play& play) {
  WriteSine(play);
  play.client->Start();
  play.on = true;
}

void Stop(Play& play) {
  play.on = false;
  if (play.client != nullptr) {
    play.client->Stop();
  }
}

long long Qpc() {
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  return now.QuadPart;
}

}  // namespace

int wmain() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  anymix::DeviceInfo devices[64];
  const int count = anymix::EnumRenderDevices(devices, 64);
  const wchar_t* names[5] = {L"AnyMix Game", L"AnyMix Chat", L"AnyMix Music", L"AnyMix Aux", L"AnyMix Add"};
  const float hz[5] = {1000.f, 880.f, 440.f, 660.f, 330.f};
  const wchar_t* ids[5] = {};
  for (int i = 0; i < count; ++i) {
    for (int n = 0; n < 5; ++n) {
      if (wcscmp(devices[i].name, names[n]) == 0) {
        ids[n] = devices[i].id;
      }
    }
  }
  Play plays[5];
  for (int n = 0; n < 5; ++n) {
    if (ids[n] == nullptr || !OpenPlay(ids[n], plays[n])) {
      std::fwprintf(stderr, L"missing %ls\n", names[n]);
      return 1;
    }
    plays[n].hz = hz[n];
    Start(plays[n]);
  }
  const long long allOn = Qpc();
  const ULONGLONG t0 = GetTickCount64();
  while (GetTickCount64() - t0 < 4000) {
    for (int n = 0; n < 5; ++n) {
      WriteSine(plays[n]);
    }
    Sleep(2);
  }
  Stop(plays[2]);
  Stop(plays[3]);
  const long long paused = Qpc();
  const ULONGLONG t1 = GetTickCount64();
  while (GetTickCount64() - t1 < 2500) {
    for (int n = 0; n < 5; ++n) {
      WriteSine(plays[n]);
    }
    Sleep(2);
  }
  Start(plays[2]);
  Start(plays[3]);
  const long long resumed = Qpc();
  const ULONGLONG t2 = GetTickCount64();
  while (GetTickCount64() - t2 < 6000) {
    for (int n = 0; n < 5; ++n) {
      WriteSine(plays[n]);
    }
    Sleep(2);
  }
  const long long done = Qpc();
  for (int n = 0; n < 5; ++n) {
    Stop(plays[n]);
  }
  std::fwprintf(stdout, L"allOn=%lld paused=%lld resumed=%lld done=%lld\n", allOn, paused, resumed, done);
  return 0;
}
