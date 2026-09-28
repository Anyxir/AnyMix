#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <cstdio>

int wmain() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* enumerator = nullptr;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                   reinterpret_cast<void**>(&enumerator));
  IMMDeviceCollection* collection = nullptr;
  enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
  UINT total = 0;
  collection->GetCount(&total);
  for (UINT i = 0; i < total; ++i) {
    IMMDevice* device = nullptr;
    collection->Item(i, &device);
    IPropertyStore* store = nullptr;
    device->OpenPropertyStore(STGM_READ, &store);
    PROPVARIANT name;
    PropVariantInit(&name);
    store->GetValue(PKEY_Device_FriendlyName, &name);
    if (name.vt != VT_LPWSTR || wcsstr(name.pwszVal, L"AnyMix") == nullptr) {
      PropVariantClear(&name);
      store->Release();
      device->Release();
      continue;
    }
    wprintf(L"%ls\n", name.pwszVal);
    PropVariantClear(&name);
    store->Release();
    IAudioSessionManager2* manager = nullptr;
    device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&manager));
    IAudioSessionEnumerator* sessions = nullptr;
    manager->GetSessionEnumerator(&sessions);
    int count = 0;
    sessions->GetCount(&count);
    for (int s = 0; s < count; ++s) {
      IAudioSessionControl* control = nullptr;
      sessions->GetSession(s, &control);
      IAudioSessionControl2* control2 = nullptr;
      control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&control2));
      DWORD pid = 0;
      control2->GetProcessId(&pid);
      AudioSessionState state = AudioSessionStateInactive;
      control->GetState(&state);
      LPWSTR display = nullptr;
      control->GetDisplayName(&display);
      wprintf(L"  pid %lu state %d %ls\n", pid, static_cast<int>(state), display != nullptr ? display : L"");
      if (display != nullptr) {
        CoTaskMemFree(display);
      }
      control2->Release();
      control->Release();
    }
    sessions->Release();
    manager->Release();
    device->Release();
  }
  collection->Release();
  enumerator->Release();
  return 0;
}
