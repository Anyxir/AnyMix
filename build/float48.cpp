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
int wmain() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IPolicyConfig* policy = nullptr;
  CoCreateInstance(CLSID_Policy, nullptr, CLSCTX_ALL, IID_Policy, reinterpret_cast<void**>(&policy));
  WAVEFORMATEXTENSIBLE fmt{};
  fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  fmt.Format.nChannels = 2;
  fmt.Format.nSamplesPerSec = 48000;
  fmt.Format.nAvgBytesPerSec = 384000;
  fmt.Format.nBlockAlign = 8;
  fmt.Format.wBitsPerSample = 32;
  fmt.Format.cbSize = 22;
  fmt.Samples.wValidBitsPerSample = 32;
  fmt.dwChannelMask = 3;
  fmt.SubFormat.Data1 = 3;
  fmt.SubFormat.Data3 = 0x10;
  fmt.SubFormat.Data4[0] = 0x80;
  fmt.SubFormat.Data4[3] = 0xaa;
  fmt.SubFormat.Data4[5] = 0x38;
  fmt.SubFormat.Data4[6] = 0x9b;
  fmt.SubFormat.Data4[7] = 0x71;
  IMMDeviceEnumerator* e = nullptr;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&e);
  const EDataFlow flows[2] = {eRender, eCapture};
  int ok = 0, fail = 0;
  for (EDataFlow flow : flows) {
    IMMDeviceCollection* col = nullptr;
    e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col);
    UINT n = 0; col->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
      IMMDevice* d = nullptr; col->Item(i, &d);
      IPropertyStore* s = nullptr; d->OpenPropertyStore(STGM_READ, &s);
      PROPVARIANT name; PropVariantInit(&name);
      s->GetValue(PKEY_Device_FriendlyName, &name);
      const wchar_t* t = name.vt == VT_LPWSTR ? name.pwszVal : L"";
      if (wcsstr(t, L"ASIOVADPRO") != nullptr) {
        LPWSTR id = nullptr; d->GetId(&id);
        WAVEFORMATEX* dev = nullptr;
        policy->GetDeviceFormat(id, 0, &dev);
        const bool already = dev && dev->wFormatTag == WAVE_FORMAT_EXTENSIBLE && dev->nSamplesPerSec == 48000 && dev->wBitsPerSample == 32;
        if (!already) {
          HRESULT hr = policy->SetDeviceFormat(id, reinterpret_cast<WAVEFORMATEX*>(&fmt), reinterpret_cast<WAVEFORMATEX*>(&fmt));
          if (SUCCEEDED(hr)) ++ok; else ++fail;
          if (wcsstr(t, L"AnyMix") != nullptr) wprintf(L"%08X %ls\n", (unsigned)hr, t);
        }
        if (dev) CoTaskMemFree(dev);
        CoTaskMemFree(id);
      }
      PropVariantClear(&name); s->Release(); d->Release();
    }
    col->Release();
  }
  wprintf(L"ok %d fail %d\n", ok, fail);
  return 0;
}
