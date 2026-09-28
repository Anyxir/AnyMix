#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <cmath>
#include <cstdint>
#include <cstdio>

#pragma pack(push, 4)
struct Req {
  uint32_t magic;
  uint32_t stream;
  uint32_t frames;
  uint32_t reserved;
  float samples[512 * 2];
};
#pragma pack(pop)

int wmain() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* enumerator = nullptr;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                   reinterpret_cast<void**>(&enumerator));
  IMMDevice* device = nullptr;
  enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
  enumerator->Release();
  IPropertyStore* store = nullptr;
  device->OpenPropertyStore(STGM_READ, &store);
  PROPVARIANT name;
  PropVariantInit(&name);
  store->GetValue(PKEY_Device_FriendlyName, &name);
  wprintf(L"playing to %ls\n", name.pwszVal);
  const bool anymix = name.pwszVal != nullptr && wcsstr(name.pwszVal, L"AnyMix") != nullptr;
  PropVariantClear(&name);
  store->Release();
  if (!anymix) {
    wprintf(L"default output is not AnyMix\n");
    device->Release();
    return 1;
  }
  IAudioClient* client = nullptr;
  device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
  device->Release();
  WAVEFORMATEX* mix = nullptr;
  client->GetMixFormat(&mix);
  client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, mix, nullptr);
  UINT32 buffer = 0;
  client->GetBufferSize(&buffer);
  IAudioRenderClient* render = nullptr;
  client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render));
  client->Start();

  HANDLE driver = CreateFileW(L"\\\\.\\AnyMix", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  const unsigned long ioctl = (0x0000001Du << 16) | (1u << 14) | (0x800u << 2);
  uint32_t bestStream = 0;
  uint32_t bestReal = 0;
  uint32_t reported[5] = {};
  uint32_t real[5] = {};
  uint32_t tailPad[5] = {};
  uint32_t reads[5] = {};
  const int loops = 200;
  float phase = 0.f;
  for (int n = 0; n < loops; ++n) {
    UINT32 padding = 0;
    client->GetCurrentPadding(&padding);
    UINT32 need = buffer > padding ? buffer - padding : 0;
    if (need > 0) {
      BYTE* data = nullptr;
      render->GetBuffer(need, &data);
      float* out = reinterpret_cast<float*>(data);
      for (UINT32 i = 0; i < need; ++i) {
        const float s = 0.125f * std::sin(phase);
        phase += 2.f * 3.14159265f * 1000.f / 48000.f;
        out[i * 2] = s;
        out[i * 2 + 1] = s;
      }
      render->ReleaseBuffer(need, 0);
    }
    for (uint32_t stream = 0; stream < 5; ++stream) {
      Req request{};
      request.magic = 0x414D5831u;
      request.stream = stream;
      request.frames = 512;
      DWORD got = 0;
      if (!DeviceIoControl(driver, ioctl, &request, sizeof(request), &request, sizeof(request), &got,
                           nullptr)) {
        continue;
      }
      ++reads[stream];
      const uint32_t frames = request.frames > 512 ? 512 : request.frames;
      reported[stream] += frames;
      uint32_t end = frames;
      while (end > 0 && request.samples[(end - 1) * 2] == 0.f && request.samples[(end - 1) * 2 + 1] == 0.f) {
        --end;
      }
      real[stream] += end;
      tailPad[stream] += frames - end;
      if (real[stream] > bestReal) {
        bestReal = real[stream];
        bestStream = stream;
      }
    }
    Sleep(5);
  }
  client->Stop();
  render->Release();
  client->Release();
  CoTaskMemFree(mix);
  CloseHandle(driver);
  wprintf(L"best stream=%u\n", bestStream);
  for (uint32_t stream = 0; stream < 5; ++stream) {
    wprintf(L"stream %u reads=%u reported=%u real=%u tailPad=%u\n", stream, reads[stream],
            reported[stream], real[stream], tailPad[stream]);
  }
  return 0;
}
