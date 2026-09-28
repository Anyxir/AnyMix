#include "anymix/Devices.h"

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <cstdio>

static HRESULT SetRaw(unsigned long pid, const wchar_t* text, bool asString) {
  HMODULE combase = LoadLibraryW(L"combase.dll");
  using CreateStringFn = HRESULT(WINAPI*)(const wchar_t*, UINT32, void**);
  using DeleteStringFn = HRESULT(WINAPI*)(void*);
  using ActivateFn = HRESULT(WINAPI*)(void*, const GUID&, void**);
  auto createString = reinterpret_cast<CreateStringFn>(GetProcAddress(combase, "WindowsCreateString"));
  auto deleteString = reinterpret_cast<DeleteStringFn>(GetProcAddress(combase, "WindowsDeleteString"));
  auto activate = reinterpret_cast<ActivateFn>(GetProcAddress(combase, "RoGetActivationFactory"));
  const wchar_t* className = L"Windows.Media.Internal.AudioPolicyConfig";
  void* classId = nullptr;
  createString(className, static_cast<UINT32>(wcslen(className)), &classId);
  const GUID iid = {0xab3d4648, 0xe242, 0x459f, {0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24}};
  void* factory = nullptr;
  const HRESULT hr = activate(classId, iid, &factory);
  deleteString(classId);
  if (FAILED(hr) || factory == nullptr) {
    return hr;
  }
  using SetFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT32, int, int, void*);
  auto setFn = reinterpret_cast<SetFn>((*reinterpret_cast<void***>(factory))[25]);
  void* device = nullptr;
  void* arg = nullptr;
  if (asString) {
    createString(text, static_cast<UINT32>(wcslen(text)), &device);
    arg = device;
  } else {
    arg = const_cast<wchar_t*>(text);
  }
  const HRESULT setHr = setFn(factory, pid, 0, 1, arg);
  if (device != nullptr) {
    deleteString(device);
  }
  auto release = reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(void*)>((*reinterpret_cast<void***>(factory))[2]);
  release(factory);
  return setHr;
}

static void Dump(const wchar_t* label) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  anymix::DeviceInfo devices[32]{};
  const int count = anymix::EnumRenderDevices(devices, 32);
  wprintf(L"-- %ls\n", label);
  for (int i = 0; i < count; ++i) {
    if (wcsstr(devices[i].name, L"AnyMix") == nullptr) {
      continue;
    }
    anymix::SessionInfo infos[16]{};
    std::wstring error;
    const int sessions = anymix::EnumSessions(devices[i].id, infos, 16, error);
    for (int s = 0; s < sessions; ++s) {
      if (wcsstr(infos[s].name, L"Spotify") != nullptr) {
        wprintf(L"%ls %ls %lu\n", devices[i].name, infos[s].name, infos[s].sessionId);
      }
    }
  }
}

int wmain() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  HMODULE combase = LoadLibraryW(L"combase.dll");
  reinterpret_cast<HRESULT(WINAPI*)(int)>(GetProcAddress(combase, "RoInitialize"))(1);
  anymix::DeviceInfo devices[32]{};
  const int count = anymix::EnumRenderDevices(devices, 32);
  const wchar_t* chat = nullptr;
  const wchar_t* music = nullptr;
  for (int i = 0; i < count; ++i) {
    if (wcscmp(devices[i].name, L"AnyMix Chat") == 0) {
      chat = devices[i].id;
    }
    if (wcscmp(devices[i].name, L"AnyMix Music") == 0) {
      music = devices[i].id;
    }
  }
  unsigned long spotify = 0;
  anymix::SessionInfo infos[16]{};
  std::wstring error;
  const int sessions = anymix::EnumSessions(music, infos, 16, error);
  for (int i = 0; i < sessions; ++i) {
    if (wcsstr(infos[i].name, L"Spotify") != nullptr) {
      spotify = infos[i].sessionId;
    }
  }
  if (spotify == 0 || chat == nullptr) {
    wprintf(L"no spotify\n");
    return 1;
  }
  wchar_t wrapped[640] = {};
  wsprintfW(wrapped, L"\\\\?\\SWD#MMDEVAPI#%ls#{e6327cad-dcec-4949-ae8a-991e976a79d2}", chat);
  Dump(L"before");
  const HRESULT raw = SetRaw(spotify, wrapped, false);
  wprintf(L"raw set 0x%08lx\n", static_cast<unsigned long>(raw));
  Sleep(1200);
  Dump(L"after raw");
  std::wstring back;
  const bool restored = anymix::MoveSession(spotify, music, back);
  wprintf(L"restore %d\n", restored ? 1 : 0);
  Sleep(800);
  Dump(L"restored");
  return 0;
}
