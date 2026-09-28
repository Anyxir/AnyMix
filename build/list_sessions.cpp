#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "anymix/Devices.h"
#include <cstdio>
int wmain(int argc, wchar_t** argv) {
  const bool mute = argc > 1 && wcscmp(argv[1], L"mute") == 0;
  const bool unmute = argc > 1 && wcscmp(argv[1], L"unmute") == 0;
  anymix::DeviceInfo devices[64]{};
  const int n = anymix::EnumRenderDevices(devices, 64);
  int sessions = 0;
  for (int i = 0; i < n; ++i) {
    std::wstring error;
    anymix::SessionInfo list[32]{};
    const int count = anymix::EnumSessions(devices[i].id, list, 32, error);
    for (int s = 0; s < count; ++s) {
      if (mute || unmute) {
        std::wstring muteError;
        anymix::SetSessionMute(devices[i].id, list[s].sessionId, mute, muteError);
      }
      wprintf(L"SESSION %ls | %ls | muted=%d\n", devices[i].name, list[s].name, list[s].muted ? 1 : 0);
      ++sessions;
    }
  }
  wprintf(L"SESSION_COUNT %d\n", sessions);
  return 0;
}
