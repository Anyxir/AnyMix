#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <cmath>
#include <cstdio>

int wmain() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* enumerator = nullptr;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                   reinterpret_cast<void**>(&enumerator));
  IMMDeviceCollection* collection = nullptr;
  enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
  UINT total = 0;
  collection->GetCount(&total);
  struct Cap {
    IMMDevice* device;
    IAudioClient* client;
    IAudioCaptureClient* capture;
    WAVEFORMATEX* mix;
    wchar_t name[160];
    float peak;
    UINT32 frames;
  };
  Cap caps[16]{};
  int used = 0;
  for (UINT i = 0; i < total && used < 16; ++i) {
    IMMDevice* device = nullptr;
    collection->Item(i, &device);
    IPropertyStore* store = nullptr;
    device->OpenPropertyStore(STGM_READ, &store);
    PROPVARIANT name;
    PropVariantInit(&name);
    const wchar_t* text = name.vt == VT_LPWSTR ? L"" : L"";
    store->GetValue(PKEY_Device_FriendlyName, &name);
    text = name.vt == VT_LPWSTR ? name.pwszVal : L"";
    const bool hit = wcsstr(text, L"AnyMix B") != nullptr || wcsstr(text, L"Mix 01") != nullptr ||
                     wcsstr(text, L"CABLE Output") != nullptr;
    if (!hit) {
      PropVariantClear(&name);
      store->Release();
      device->Release();
      continue;
    }
    Cap& cap = caps[used];
    wcsncpy(cap.name, text, 159);
    cap.device = device;
    device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&cap.client));
    cap.client->GetMixFormat(&cap.mix);
    const HRESULT hr = cap.client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, cap.mix, nullptr);
    if (FAILED(hr)) {
      wprintf(L"init %08X %ls\n", static_cast<unsigned>(hr), cap.name);
      PropVariantClear(&name);
      store->Release();
      continue;
    }
    cap.client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&cap.capture));
    cap.client->Start();
    ++used;
    PropVariantClear(&name);
    store->Release();
  }
  const DWORD start = GetTickCount();
  while (GetTickCount() - start < 1200) {
    for (int c = 0; c < used; ++c) {
      UINT32 packet = 0;
      caps[c].capture->GetNextPacketSize(&packet);
      while (packet > 0) {
        BYTE* data = nullptr;
        UINT32 count = 0;
        DWORD flags = 0;
        caps[c].capture->GetBuffer(&data, &count, &flags, nullptr, nullptr);
        caps[c].frames += count;
        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && data != nullptr && caps[c].mix->wBitsPerSample == 32) {
          const float* samples = reinterpret_cast<const float*>(data);
          const UINT32 n = count * caps[c].mix->nChannels;
          for (UINT32 s = 0; s < n; ++s) {
            caps[c].peak = std::fmax(caps[c].peak, std::fabs(samples[s]));
          }
        }
        caps[c].capture->ReleaseBuffer(count);
        caps[c].capture->GetNextPacketSize(&packet);
      }
    }
    Sleep(5);
  }
  for (int c = 0; c < used; ++c) {
    caps[c].client->Stop();
    wprintf(L"peak %.5f frames %u %ls\n", caps[c].peak, caps[c].frames, caps[c].name);
  }
  return 0;
}
