#include "anymix/Devices.h"

#include <audioclient.h>
#include <cmath>
#include <cstdio>
#include <mmdeviceapi.h>

namespace {

constexpr float kDb18 = 0.12589254117941673f;

bool OpenPlay(const wchar_t* id, IAudioClient*& client, IAudioRenderClient*& render, UINT32& bufferFrames) {
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
  if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client)))) {
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
  if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, &format, nullptr))) {
    return false;
  }
  if (FAILED(client->GetBufferSize(&bufferFrames))) {
    return false;
  }
  return SUCCEEDED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render)));
}

void WriteSine(IAudioClient* client, IAudioRenderClient* render, UINT32 bufferFrames, float hz, float& phase) {
  UINT32 padding = 0;
  if (FAILED(client->GetCurrentPadding(&padding)) || padding >= bufferFrames) {
    return;
  }
  const UINT32 frames = bufferFrames - padding;
  BYTE* data = nullptr;
  if (FAILED(render->GetBuffer(frames, &data)) || data == nullptr) {
    return;
  }
  float* samples = reinterpret_cast<float*>(data);
  for (UINT32 i = 0; i < frames; ++i) {
    const float s = std::sin(phase) * kDb18;
    phase += 2.f * 3.14159265358979323846f * hz / 48000.f;
    samples[i * 2] = s;
    samples[i * 2 + 1] = s;
  }
  render->ReleaseBuffer(frames, 0);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  const wchar_t* mode = argc > 1 ? argv[1] : L"both";
  const ULONGLONG playMs = argc > 2 ? _wtoi(argv[2]) * 1000ull : 30000ull;
  const bool playGame = wcscmp(mode, L"music") != 0;
  const bool playMusic = wcscmp(mode, L"game") != 0;
  const bool playChat = wcscmp(mode, L"three") == 0;
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  anymix::DeviceInfo devices[64];
  const int count = anymix::EnumRenderDevices(devices, 64);
  const wchar_t* gameId = nullptr;
  const wchar_t* musicId = nullptr;
  const wchar_t* chatId = nullptr;
  for (int i = 0; i < count; ++i) {
    if (wcscmp(devices[i].name, L"AnyMix Game") == 0) {
      gameId = devices[i].id;
    }
    if (wcscmp(devices[i].name, L"AnyMix Music") == 0) {
      musicId = devices[i].id;
    }
    if (wcscmp(devices[i].name, L"AnyMix Chat") == 0) {
      chatId = devices[i].id;
    }
  }
  if ((playGame && gameId == nullptr) || (playMusic && musicId == nullptr) ||
      (playChat && chatId == nullptr)) {
    std::fwprintf(stderr, L"AnyMix Game or Music endpoint missing\n");
    return 1;
  }
  IAudioClient* gameClient = nullptr;
  IAudioClient* musicClient = nullptr;
  IAudioRenderClient* gameRender = nullptr;
  IAudioRenderClient* musicRender = nullptr;
  IAudioClient* chatClient = nullptr;
  IAudioRenderClient* chatRender = nullptr;
  UINT32 gameBuffer = 0;
  UINT32 musicBuffer = 0;
  UINT32 chatBuffer = 0;
  if ((playGame && !OpenPlay(gameId, gameClient, gameRender, gameBuffer)) ||
      (playMusic && !OpenPlay(musicId, musicClient, musicRender, musicBuffer)) ||
      (playChat && !OpenPlay(chatId, chatClient, chatRender, chatBuffer))) {
    std::fwprintf(stderr, L"Cannot open the virtual endpoint\n");
    return 1;
  }
  float gamePhase = 0.f;
  float musicPhase = 0.f;
  float chatPhase = 0.f;
  if (playGame) {
    WriteSine(gameClient, gameRender, gameBuffer, 1000.f, gamePhase);
    gameClient->Start();
  }
  if (playMusic) {
    WriteSine(musicClient, musicRender, musicBuffer, 440.f, musicPhase);
    musicClient->Start();
  }
  if (playChat) {
    WriteSine(chatClient, chatRender, chatBuffer, 880.f, chatPhase);
    chatClient->Start();
  }
  const ULONGLONG end = GetTickCount64() + playMs;
  while (GetTickCount64() < end) {
    if (playGame) {
      WriteSine(gameClient, gameRender, gameBuffer, 1000.f, gamePhase);
    }
    if (playMusic) {
      WriteSine(musicClient, musicRender, musicBuffer, 440.f, musicPhase);
    }
    if (playChat) {
      WriteSine(chatClient, chatRender, chatBuffer, 880.f, chatPhase);
    }
    Sleep(2);
  }
  if (playGame) {
    gameClient->Stop();
  }
  if (playMusic) {
    musicClient->Stop();
  }
  if (playChat) {
    chatClient->Stop();
  }
  std::fwprintf(stdout, L"played %llu ms mode=%ls\n", playMs, mode);
  return 0;
}
