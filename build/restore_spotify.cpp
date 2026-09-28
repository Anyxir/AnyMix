#include "anymix/Devices.h"

#include <windows.h>

#include <cstdio>

int wmain() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  anymix::DeviceInfo devices[32]{};
  const int count = anymix::EnumRenderDevices(devices, 32);
  const wchar_t* music = nullptr;
  for (int i = 0; i < count; ++i) {
    if (wcscmp(devices[i].name, L"AnyMix Music") == 0) {
      music = devices[i].id;
    }
  }
  std::wstring error;
  const bool ok = anymix::MoveSession(17348, music, error);
  wprintf(L"restore %d %ls\n", ok ? 1 : 0, error.c_str());
  return ok ? 0 : 1;
}
