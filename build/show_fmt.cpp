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
  IMMDeviceEnumerator* e = nullptr;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&e);
  IMMDeviceCollection* col = nullptr;
  e->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
  UINT n = 0; col->GetCount(&n);
  wprintf(L"render count %u\n", n);
  for (UINT i = 0; i < n; ++i) {
    IMMDevice* d = nullptr; col->Item(i, &d);
    IPropertyStore* s = nullptr; d->OpenPropertyStore(STGM_READ, &s);
    PROPVARIANT name; PropVariantInit(&name);
    s->GetValue(PKEY_Device_FriendlyName, &name);
    const wchar_t* t = name.vt == VT_LPWSTR ? name.pwszVal : L"";
    if (wcsstr(t, L"AnyMix") || wcsstr(t, L"Casque")) {
      LPWSTR id = nullptr; d->GetId(&id);
      WAVEFORMATEX* dev = nullptr;
      HRESULT hr = policy->GetDeviceFormat(id, 0, &dev);
      IAudioClient* c = nullptr; d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&c);
      WAVEFORMATEX* mix = nullptr; c->GetMixFormat(&mix);
      wprintf(L"idx %u dev %08X tag %u %uHz bits %u | mix tag %u %uHz bits %u | %ls\n", i,
              (unsigned)hr, dev ? dev->wFormatTag : 0, dev ? dev->nSamplesPerSec : 0, dev ? dev->wBitsPerSample : 0,
              mix->wFormatTag, mix->nSamplesPerSec, mix->wBitsPerSample, t);
      if (dev) CoTaskMemFree(dev);
      CoTaskMemFree(mix); c->Release(); CoTaskMemFree(id);
    }
    PropVariantClear(&name); s->Release(); d->Release();
  }
  return 0;
}
