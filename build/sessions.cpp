#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <windows.h>
#include <cstdio>
int wmain() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* e = nullptr;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&e);
  IMMDeviceCollection* col = nullptr;
  e->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
  UINT n = 0; col->GetCount(&n);
  for (UINT i = 0; i < n; ++i) {
    IMMDevice* d = nullptr; col->Item(i, &d);
    IPropertyStore* s = nullptr; d->OpenPropertyStore(STGM_READ, &s);
    PROPVARIANT name; PropVariantInit(&name);
    s->GetValue(PKEY_Device_FriendlyName, &name);
    const wchar_t* t = name.vt == VT_LPWSTR ? name.pwszVal : L"";
    if (wcsstr(t, L"AnyMix") == nullptr) { PropVariantClear(&name); s->Release(); d->Release(); continue; }
    IAudioSessionManager2* m = nullptr;
    d->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&m);
    IAudioSessionEnumerator* se = nullptr; m->GetSessionEnumerator(&se);
    int c = 0; se->GetCount(&c);
    wprintf(L"%ls sessions %d\n", t, c);
    for (int k = 0; k < c; ++k) {
      IAudioSessionControl* sc = nullptr; se->GetSession(k, &sc);
      IAudioSessionControl2* sc2 = nullptr;
      sc->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&sc2);
      DWORD pid = 0; sc2->GetProcessId(&pid);
      AudioSessionState st; sc->GetState(&st);
      wprintf(L"  pid %lu state %d\n", pid, (int)st);
      sc2->Release(); sc->Release();
    }
    se->Release(); m->Release();
    PropVariantClear(&name); s->Release(); d->Release();
  }
  return 0;
}
