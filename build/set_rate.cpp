#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <windows.h>
#include <cstdio>
#include <cstring>

struct IPolicyConfig : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
  virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
};

static const GUID CLSID_Policy = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
static const GUID IID_Policy = {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};

static void Fix(IPolicyConfig* policy, IMMDevice* device, const wchar_t* name) {
  LPWSTR id = nullptr;
  device->GetId(&id);
  IAudioClient* client = nullptr;
  device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
  WAVEFORMATEX* mix = nullptr;
  client->GetMixFormat(&mix);
  wprintf(L"was %u Hz ch %u %ls\n", mix->nSamplesPerSec, mix->nChannels, name);
  if (mix->nSamplesPerSec != 48000) {
    WAVEFORMATEX pcm{};
    pcm.wFormatTag = WAVE_FORMAT_PCM;
    pcm.nChannels = 2;
    pcm.nSamplesPerSec = 48000;
    pcm.wBitsPerSample = 16;
    pcm.nBlockAlign = 4;
    pcm.nAvgBytesPerSec = 192000;
    const HRESULT hr = policy->SetDeviceFormat(id, &pcm, &pcm);
    wprintf(L"set %08X %ls\n", static_cast<unsigned>(hr), name);
  }
  CoTaskMemFree(mix);
  client->Release();
  CoTaskMemFree(id);
}

int wmain() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IPolicyConfig* policy = nullptr;
  const HRESULT qi = CoCreateInstance(CLSID_Policy, nullptr, CLSCTX_ALL, IID_Policy, reinterpret_cast<void**>(&policy));
  wprintf(L"policy %08X\n", static_cast<unsigned>(qi));
  if (FAILED(qi) || policy == nullptr) return 1;
  IMMDeviceEnumerator* enumerator = nullptr;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                   reinterpret_cast<void**>(&enumerator));
  const EDataFlow flows[2] = {eRender, eCapture};
  for (EDataFlow flow : flows) {
    IMMDeviceCollection* col = nullptr;
    enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col);
    UINT count = 0;
    col->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
      IMMDevice* device = nullptr;
      col->Item(i, &device);
      IPropertyStore* store = nullptr;
      device->OpenPropertyStore(STGM_READ, &store);
      PROPVARIANT name;
      PropVariantInit(&name);
      store->GetValue(PKEY_Device_FriendlyName, &name);
      const wchar_t* text = name.vt == VT_LPWSTR ? name.pwszVal : L"";
      if (wcsstr(text, L"ASIOVADPRO") != nullptr) {
        Fix(policy, device, text);
      }
      PropVariantClear(&name);
      store->Release();
      device->Release();
    }
    col->Release();
  }
  policy->Release();
  return 0;
}
