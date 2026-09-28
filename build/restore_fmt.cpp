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
  WAVEFORMATEXTENSIBLE back{};
  back.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  back.Format.nChannels = 2;
  back.Format.nSamplesPerSec = 44100;
  back.Format.nAvgBytesPerSec = 352800;
  back.Format.nBlockAlign = 8;
  back.Format.wBitsPerSample = 32;
  back.Format.cbSize = 22;
  back.Samples.wValidBitsPerSample = 32;
  back.dwChannelMask = 3;
  back.SubFormat.Data1 = 0x00000003;
  back.SubFormat.Data2 = 0x0000;
  back.SubFormat.Data3 = 0x0010;
  back.SubFormat.Data4[0] = 0x80;
  back.SubFormat.Data4[1] = 0x00;
  back.SubFormat.Data4[2] = 0x00;
  back.SubFormat.Data4[3] = 0xaa;
  back.SubFormat.Data4[4] = 0x00;
  back.SubFormat.Data4[5] = 0x38;
  back.SubFormat.Data4[6] = 0x9b;
  back.SubFormat.Data4[7] = 0x71;
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
        IAudioClient* client = nullptr;
        device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
        WAVEFORMATEX* mix = nullptr;
        client->GetMixFormat(&mix);
        if (mix->wFormatTag == WAVE_FORMAT_PCM && mix->nSamplesPerSec == 48000) {
          LPWSTR id = nullptr;
          device->GetId(&id);
          const HRESULT hr = policy->SetDeviceFormat(id, reinterpret_cast<WAVEFORMATEX*>(&back),
                                                      reinterpret_cast<WAVEFORMATEX*>(&back));
          wprintf(L"restore %08X %ls\n", static_cast<unsigned>(hr), text);
          CoTaskMemFree(id);
        }
        CoTaskMemFree(mix);
        client->Release();
      }
      PropVariantClear(&name);
      store->Release();
      device->Release();
    }
    col->Release();
  }
  return 0;
}
