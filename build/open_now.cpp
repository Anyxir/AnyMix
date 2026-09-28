#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <windows.h>
#include <cstdio>
int wmain() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* e = nullptr;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&e);
  const EDataFlow flows[2] = {eRender, eCapture};
  const wchar_t* want[] = {L"AnyMix Game (ASIOVADPRO Driver)", L"AnyMix Add (ASIOVADPRO Driver)", L"AnyMix B1 (ASIOVADPRO Driver)", L"AnyMix B5 (ASIOVADPRO Driver)", L"Casque (Realtek(R) Audio)"};
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
      bool hit = false;
      for (auto* w : want) if (wcscmp(t, w) == 0) hit = true;
      if (hit) {
        IAudioClient* c = nullptr;
        d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&c);
        WAVEFORMATEX* mix = nullptr; c->GetMixFormat(&mix);
        DWORD flags = flow == eRender ? 0x00020000 : 0;
        HRESULT hr = c->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 10000000, 0, mix, nullptr);
        wprintf(L"%s init %08X %uHz %ls\n", flow == eRender ? L"play" : L"cap", (unsigned)hr, mix->nSamplesPerSec, t);
        CoTaskMemFree(mix); c->Release();
      }
      PropVariantClear(&name); s->Release(); d->Release();
    }
    col->Release();
  }
  return 0;
}
