#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static bool Want(const wchar_t* text) {
  return wcsstr(text, L"ASIOVADPRO") != nullptr || wcsstr(text, L"CABLE Output") != nullptr;
}

int wmain() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* enumerator = nullptr;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                   reinterpret_cast<void**>(&enumerator));
  IMMDeviceCollection* renders = nullptr;
  enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &renders);
  UINT renderCount = 0;
  renders->GetCount(&renderCount);
  IMMDevice* game = nullptr;
  for (UINT i = 0; i < renderCount; ++i) {
    IMMDevice* device = nullptr;
    renders->Item(i, &device);
    IPropertyStore* store = nullptr;
    device->OpenPropertyStore(STGM_READ, &store);
    PROPVARIANT name;
    PropVariantInit(&name);
    store->GetValue(PKEY_Device_FriendlyName, &name);
    const bool hit = name.vt == VT_LPWSTR && wcscmp(name.pwszVal, L"AnyMix Game (ASIOVADPRO Driver)") == 0;
    PropVariantClear(&name);
    store->Release();
    if (hit) {
      game = device;
      break;
    }
    device->Release();
  }
  if (game == nullptr) {
    wprintf(L"game missing\n");
    return 1;
  }
  IAudioClient* play = nullptr;
  game->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&play));
  WAVEFORMATEX* playMix = nullptr;
  play->GetMixFormat(&playMix);
  HRESULT hr = play->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, playMix, nullptr);
  wprintf(L"play init %08X\n", static_cast<unsigned>(hr));
  if (FAILED(hr)) {
    return 1;
  }
  IAudioRenderClient* render = nullptr;
  play->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render));
  UINT32 playBuffer = 0;
  play->GetBufferSize(&playBuffer);

  struct Cap {
    IAudioClient* client;
    IAudioCaptureClient* capture;
    WAVEFORMATEX* mix;
    wchar_t name[180];
    float peak;
  };
  std::vector<Cap> caps;
  IMMDeviceCollection* captures = nullptr;
  enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &captures);
  UINT captureCount = 0;
  captures->GetCount(&captureCount);
  for (UINT i = 0; i < captureCount; ++i) {
    IMMDevice* device = nullptr;
    captures->Item(i, &device);
    IPropertyStore* store = nullptr;
    device->OpenPropertyStore(STGM_READ, &store);
    PROPVARIANT name;
    PropVariantInit(&name);
    store->GetValue(PKEY_Device_FriendlyName, &name);
    const wchar_t* text = name.vt == VT_LPWSTR ? name.pwszVal : L"";
    if (!Want(text)) {
      PropVariantClear(&name);
      store->Release();
      device->Release();
      continue;
    }
    Cap cap{};
    wcsncpy(cap.name, text, 179);
    device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&cap.client));
    cap.client->GetMixFormat(&cap.mix);
    const HRESULT init = cap.client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, cap.mix, nullptr);
    if (FAILED(init)) {
      wprintf(L"skip %08X %ls\n", static_cast<unsigned>(init), cap.name);
      PropVariantClear(&name);
      store->Release();
      device->Release();
      continue;
    }
    cap.client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&cap.capture));
    cap.client->Start();
    caps.push_back(cap);
    PropVariantClear(&name);
    store->Release();
    device->Release();
  }
  play->Start();
  const DWORD start = GetTickCount();
  double phase = 0;
  while (GetTickCount() - start < 3500) {
    UINT32 padding = 0;
    play->GetCurrentPadding(&padding);
    const UINT32 need = playBuffer > padding ? playBuffer - padding : 0;
    if (need > 0) {
      BYTE* dest = nullptr;
      render->GetBuffer(need, &dest);
      float* samples = reinterpret_cast<float*>(dest);
      const int channels = playMix->nChannels;
      for (UINT32 i = 0; i < need; ++i) {
        const float s = 0.2f * std::sin(static_cast<float>(phase));
        phase += 2.0 * 3.1415926535 * 440.0 / playMix->nSamplesPerSec;
        for (int c = 0; c < channels; ++c) {
          samples[i * channels + c] = s;
        }
      }
      render->ReleaseBuffer(need, 0);
    }
    for (Cap& cap : caps) {
      UINT32 packet = 0;
      cap.capture->GetNextPacketSize(&packet);
      while (packet > 0) {
        BYTE* data = nullptr;
        UINT32 count = 0;
        DWORD flags = 0;
        cap.capture->GetBuffer(&data, &count, &flags, nullptr, nullptr);
        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && data != nullptr) {
          const UINT32 n = count * cap.mix->nChannels;
          if (cap.mix->wBitsPerSample == 32 && cap.mix->wFormatTag != WAVE_FORMAT_PCM) {
            const float* samples = reinterpret_cast<const float*>(data);
            for (UINT32 s = 0; s < n; ++s) {
              cap.peak = std::fmax(cap.peak, std::fabs(samples[s]));
            }
          } else if (cap.mix->wBitsPerSample == 16) {
            const int16_t* samples = reinterpret_cast<const int16_t*>(data);
            for (UINT32 s = 0; s < n; ++s) {
              cap.peak = std::fmax(cap.peak, std::fabs(samples[s] / 32768.f));
            }
          } else if (cap.mix->wBitsPerSample == 32) {
            const int32_t* samples = reinterpret_cast<const int32_t*>(data);
            for (UINT32 s = 0; s < n; ++s) {
              cap.peak = std::fmax(cap.peak, std::fabs(samples[s] / 2147483648.f));
            }
          }
        }
        cap.capture->ReleaseBuffer(count);
        cap.capture->GetNextPacketSize(&packet);
      }
    }
    Sleep(5);
  }
  play->Stop();
  for (Cap& cap : caps) {
    cap.client->Stop();
    wprintf(L"peak %.5f tag %u bits %u rate %lu %ls\n", cap.peak, cap.mix->wFormatTag, cap.mix->wBitsPerSample,
            cap.mix->nSamplesPerSec, cap.name);
  }
  return 0;
}
