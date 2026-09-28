#include "anymix/Devices.h"

#include <windows.h>

#include <cstdio>

static void PrintPolicy(unsigned long pid) {
  HMODULE combase = LoadLibraryW(L"combase.dll");
  using CreateStringFn = HRESULT(WINAPI*)(const wchar_t*, UINT32, void**);
  using DeleteStringFn = HRESULT(WINAPI*)(void*);
  using ActivateFn = HRESULT(WINAPI*)(void*, const GUID&, void**);
  using GetStringFn = const wchar_t*(WINAPI*)(void*, UINT32*);
  auto createString = reinterpret_cast<CreateStringFn>(GetProcAddress(combase, "WindowsCreateString"));
  auto deleteString = reinterpret_cast<DeleteStringFn>(GetProcAddress(combase, "WindowsDeleteString"));
  auto activate = reinterpret_cast<ActivateFn>(GetProcAddress(combase, "RoGetActivationFactory"));
  auto getRaw = reinterpret_cast<GetStringFn>(GetProcAddress(combase, "WindowsGetStringRawBuffer"));
  const wchar_t* className = L"Windows.Media.Internal.AudioPolicyConfig";
  void* classId = nullptr;
  createString(className, static_cast<UINT32>(wcslen(className)), &classId);
  const GUID iid = {0xab3d4648, 0xe242, 0x459f, {0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24}};
  void* factory = nullptr;
  activate(classId, iid, &factory);
  deleteString(classId);
  using GetFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT32, int, int, void**);
  auto getFn = reinterpret_cast<GetFn>((*reinterpret_cast<void***>(factory))[26]);
  void* stored = nullptr;
  const HRESULT hr = getFn(factory, pid, 0, 1, &stored);
  const wchar_t* raw = L"";
  if (SUCCEEDED(hr) && stored != nullptr) {
    UINT32 len = 0;
    raw = getRaw(stored, &len);
  }
  wprintf(L"pid %lu policy 0x%08lx %ls\n", pid, static_cast<unsigned long>(hr), raw != nullptr ? raw : L"");
  if (stored != nullptr) {
    deleteString(stored);
  }
  auto release = reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(void*)>((*reinterpret_cast<void***>(factory))[2]);
  release(factory);
}

int wmain() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  anymix::DeviceInfo devices[32]{};
  const int count = anymix::EnumRenderDevices(devices, 32);
  for (int i = 0; i < count; ++i) {
    if (wcsstr(devices[i].name, L"AnyMix") == nullptr) {
      continue;
    }
    anymix::SessionInfo infos[16]{};
    std::wstring error;
    const int sessions = anymix::EnumSessions(devices[i].id, infos, 16, error);
    for (int s = 0; s < sessions; ++s) {
      wprintf(L"%ls %ls\n", devices[i].name, infos[s].name);
      PrintPolicy(infos[s].sessionId);
    }
  }
  return 0;
}
