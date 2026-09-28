#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <windows.h>
#include <cstdio>

struct IPolicyConfig : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
  virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
};
static const GUID CLSID_Policy = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
static const GUID IID_Policy = {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};

static void Dump(const wchar_t* name, WAVEFORMATEX* mix) {
  const unsigned char* b = reinterpret_cast<const unsigned char*>(mix);
  const int n = sizeof(WAVEFORMATEX) + mix->cbSize;
  wprintf(L"%ls tag %u rate %u ch %u bits %u align %u avg %u cb %u bytes", name, mix->wFormatTag, mix->nSamplesPerSec,
          mix->nChannels, mix->wBitsPerSample, mix->nBlockAlign, mix->nAvgBytesPerSec, mix->cbSize);
  for (int i = 0; i < n; ++i) wprintf(L" %02X", b[i]);
  wprintf(L"\n");
}

int wmain() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IPolicyConfig* policy = nullptr;
  CoCreateInstance(CLSID_Policy, nullptr, CLSCTX_ALL, IID_Policy, reinterpret_cast<void**>(&policy));
  IMMDeviceEnumerator* enumerator = nullptr;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                   reinterpret_cast<void**>(&enumerator));
  IMMDeviceCollection* col = nullptr;
  enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col);
  UINT count = 0;
  col->GetCount(&count);
  WAVEFORMATEX* good = nullptr;
  wchar_t badId[512] = {};
  for (UINT i = 0; i < count; ++i) {
    IMMDevice* device = nullptr;
    col->Item(i, &device);
    IPropertyStore* store = nullptr;
    device->OpenPropertyStore(STGM_READ, &store);
    PROPVARIANT name;
    PropVariantInit(&name);
    store->GetValue(PKEY_Device_FriendlyName, &name);
    const wchar_t* text = name.vt == VT_LPWSTR ? name.pwszVal : L"";
    IAudioClient* client = nullptr;
    device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    WAVEFORMATEX* mix = nullptr;
    client->GetMixFormat(&mix);
    if (wcscmp(text, L"AnyMix B1 (ASIOVADPRO Driver)") == 0) {
      Dump(L"B1", mix);
      good = mix;
      mix = nullptr;
    } else if (wcscmp(text, L"AnyMix B2 (ASIOVADPRO Driver)") == 0) {
      Dump(L"B2", mix);
      LPWSTR id = nullptr;
      device->GetId(&id);
      wcsncpy(badId, id, 511);
      CoTaskMemFree(id);
      CoTaskMemFree(mix);
    } else {
      CoTaskMemFree(mix);
    }
    client->Release();
    PropVariantClear(&name);
    store->Release();
    device->Release();
  }
  if (good != nullptr && badId[0] != 0) {
    HRESULT hr = policy->SetDeviceFormat(badId, good, good);
    wprintf(L"copy B1 format onto B2 %08X\n", static_cast<unsigned>(hr));
    WAVEFORMATEX pcm{};
    pcm.wFormatTag = WAVE_FORMAT_PCM;
    pcm.nChannels = 2;
    pcm.nSamplesPerSec = 48000;
    pcm.wBitsPerSample = 16;
    pcm.nBlockAlign = 4;
    pcm.nAvgBytesPerSec = 192000;
    hr = policy->SetDeviceFormat(badId, &pcm, &pcm);
    wprintf(L"pcm16 48k onto B2 %08X\n", static_cast<unsigned>(hr));
    CoTaskMemFree(good);
  }
  return 0;
}
