#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <cstdio>

static void Dump(IMMDeviceEnumerator* enumerator, EDataFlow flow, const wchar_t* label) {
  IMMDeviceCollection* collection = nullptr;
  enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
  UINT total = 0;
  collection->GetCount(&total);
  wprintf(L"== %ls %u\n", label, total);
  for (UINT i = 0; i < total; ++i) {
    IMMDevice* device = nullptr;
    collection->Item(i, &device);
    IPropertyStore* store = nullptr;
    device->OpenPropertyStore(STGM_READ, &store);
    PROPVARIANT name;
    PropVariantInit(&name);
    store->GetValue(PKEY_Device_FriendlyName, &name);
    const wchar_t* text = name.vt == VT_LPWSTR ? name.pwszVal : L"";
    if (wcsstr(text, L"AnyMix B") == nullptr && wcsstr(text, L"Mix 01") == nullptr &&
        wcsstr(text, L"CABLE") == nullptr) {
      PropVariantClear(&name);
      store->Release();
      device->Release();
      continue;
    }
    wprintf(L"%ls\n", text);
    IAudioSessionManager2* manager = nullptr;
    if (SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(&manager))) &&
        manager != nullptr) {
      IAudioSessionEnumerator* sessions = nullptr;
      manager->GetSessionEnumerator(&sessions);
      int count = 0;
      sessions->GetCount(&count);
      wprintf(L"  sessions %d\n", count);
      for (int s = 0; s < count; ++s) {
        IAudioSessionControl* control = nullptr;
        sessions->GetSession(s, &control);
        IAudioSessionControl2* control2 = nullptr;
        control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&control2));
        DWORD pid = 0;
        control2->GetProcessId(&pid);
        AudioSessionState state = AudioSessionStateInactive;
        control->GetState(&state);
        wchar_t image[520] = L"?";
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (proc != nullptr) {
          DWORD size = 520;
          QueryFullProcessImageNameW(proc, 0, image, &size);
          CloseHandle(proc);
        }
        wprintf(L"  pid=%lu state=%d %ls\n", pid, static_cast<int>(state), image);
        control2->Release();
        control->Release();
      }
      sessions->Release();
      manager->Release();
    }
    PropVariantClear(&name);
    store->Release();
    device->Release();
  }
  collection->Release();
}

int wmain() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* enumerator = nullptr;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                   reinterpret_cast<void**>(&enumerator));
  Dump(enumerator, eCapture, L"capture");
  enumerator->Release();
  return 0;
}
