#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <cstdio>
#include <cstdint>

#pragma pack(push, 4)
struct Req {
  uint32_t magic;
  uint32_t stream;
  uint32_t frames;
  uint32_t reserved;
  float samples[512 * 2];
};
#pragma pack(pop)

static void ListEndpoints(const wchar_t* label, EDataFlow flow) {
  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&enumerator)))) {
    return;
  }
  IMMDeviceCollection* collection = nullptr;
  enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
  enumerator->Release();
  UINT count = 0;
  collection->GetCount(&count);
  wprintf(L"%ls count=%u\n", label, count);
  for (UINT i = 0; i < count; ++i) {
    IMMDevice* device = nullptr;
    collection->Item(i, &device);
    IPropertyStore* store = nullptr;
    wchar_t name[256] = L"?";
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
      PROPVARIANT value;
      PropVariantInit(&value);
      if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) {
        wcsncpy(name, value.pwszVal, 255);
      }
      PropVariantClear(&value);
      store->Release();
    }
    IAudioClient* client = nullptr;
    device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    device->Release();
    WAVEFORMATEX* mix = nullptr;
    if (client != nullptr && SUCCEEDED(client->GetMixFormat(&mix)) && mix != nullptr) {
      wprintf(L"  [%u] %ls rate=%lu ch=%u bits=%u align=%u\n", i, name, mix->nSamplesPerSec,
              mix->nChannels, mix->wBitsPerSample, mix->nBlockAlign);
      CoTaskMemFree(mix);
    } else {
      wprintf(L"  [%u] %ls mix-unavailable\n", i, name);
    }
    if (client != nullptr) {
      client->Release();
    }
  }
  collection->Release();
}
#if 0
  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&enumerator)))) {
    wprintf(L"%ls enumerator failed\n", label);
    return;
  }
  IMMDevice* device = nullptr;
  HRESULT hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device);
  enumerator->Release();
  if (FAILED(hr) || device == nullptr) {
    wprintf(L"%ls no default\n", label);
    return;
  }
  IPropertyStore* store = nullptr;
  if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
    PROPVARIANT name;
    PropVariantInit(&name);
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR) {
      wprintf(L"%ls name=%ls\n", label, name.pwszVal);
    }
    PropVariantClear(&name);
    store->Release();
  }
  IAudioClient* client = nullptr;
  device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
  device->Release();
  if (client == nullptr) {
    wprintf(L"%ls activate failed\n", label);
    return;
  }
  WAVEFORMATEX* mix = nullptr;
  if (SUCCEEDED(client->GetMixFormat(&mix)) && mix != nullptr) {
    const wchar_t* kind = L"other";
    unsigned sub = 0;
    if (mix->wFormatTag == WAVE_FORMAT_PCM) {
      kind = L"PCM";
    } else if (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
      kind = L"float";
    } else if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
      auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix);
      sub = ext->SubFormat.Data1;
      kind = sub == 3 ? L"extensible-float" : L"extensible-pcm-or-other";
    }
    wprintf(L"%ls mix rate=%lu channels=%u bits=%u align=%u avg=%lu kind=%ls sub=%u\n", label,
            mix->nSamplesPerSec, mix->nChannels, mix->wBitsPerSample, mix->nBlockAlign,
            mix->nAvgBytesPerSec, kind, sub);
    CoTaskMemFree(mix);
  }
  client->Release();
}
#endif

static uint32_t TrailingZeros(const Req& request) {
  uint32_t frames = request.frames > 512 ? 512 : request.frames;
  while (frames > 0) {
    const uint32_t i = frames - 1;
    if (request.samples[i * 2] != 0.f || request.samples[i * 2 + 1] != 0.f) {
      break;
    }
    --frames;
  }
  return request.frames > 512 ? 0 : request.frames - frames;
}

int wmain() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ListEndpoints(L"RENDER", eRender);
  ListEndpoints(L"CAPTURE", eCapture);

  HANDLE driver = CreateFileW(L"\\\\.\\AnyMix", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  wprintf(L"driver handle=%p error=%lu\n", driver, driver == INVALID_HANDLE_VALUE ? GetLastError() : 0);
  if (driver == INVALID_HANDLE_VALUE) {
    CoUninitialize();
    return 1;
  }
  const unsigned long ioctl = (0x0000001Du << 16) | (1u << 14) | (0x800u << 2);
  for (uint32_t stream = 0; stream < 5; ++stream) {
    uint32_t reported = 0;
    uint32_t zeroTails = 0;
    uint32_t emptyReports = 0;
    uint32_t nonzero = 0;
    for (int n = 0; n < 30; ++n) {
      Req request{};
      request.magic = 0x414D5831u;
      request.stream = stream;
      request.frames = 512;
      DWORD got = 0;
      const BOOL ok = DeviceIoControl(driver, ioctl, &request, sizeof(request), &request, sizeof(request),
                                      &got, nullptr);
      if (!ok) {
        wprintf(L"stream %lu ioctl fail %lu\n", stream, GetLastError());
        break;
      }
      reported += request.frames;
      if (request.frames == 0) {
        ++emptyReports;
      }
      const uint32_t tail = TrailingZeros(request);
      if (tail > 0 && request.frames > 0) {
        ++zeroTails;
      }
      if (request.frames > tail) {
        ++nonzero;
      }
    }
    wprintf(L"stream %lu reads=30 reportedFrames=%lu zeroTailReads=%lu empty=%lu nonzero=%lu\n", stream,
            reported, zeroTails, emptyReports, nonzero);
  }
  CloseHandle(driver);
  CoUninitialize();
  return 0;
}
