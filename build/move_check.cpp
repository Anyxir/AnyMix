#include "anymix/Devices.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <cstdio>

static int HasPid(const wchar_t* label, const wchar_t* endpoint, unsigned long pid) {
  anymix::SessionInfo infos[32]{};
  std::wstring error;
  const int count = anymix::EnumSessions(endpoint, infos, 32, error);
  int hit = 0;
  wprintf(L"%ls sessions %d %ls\n", label, count, error.c_str());
  for (int i = 0; i < count; ++i) {
    wprintf(L"  %lu %ls\n", infos[i].sessionId, infos[i].name);
    if (infos[i].sessionId == pid) {
      hit = 1;
    }
  }
  return hit;
}

int wmain() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  anymix::DeviceInfo devices[32]{};
  const int count = anymix::EnumRenderDevices(devices, 32);
  if (wcscmp(GetCommandLineW(), L"") == 0) {
    return 0;
  }
  const wchar_t* music = nullptr;
  const wchar_t* game = nullptr;
  const wchar_t* chat = nullptr;
  for (int i = 0; i < count; ++i) {
    if (wcscmp(devices[i].name, L"AnyMix Music") == 0) {
      music = devices[i].id;
    }
    if (wcscmp(devices[i].name, L"AnyMix Game") == 0) {
      game = devices[i].id;
    }
    if (wcscmp(devices[i].name, L"AnyMix Chat") == 0) {
      chat = devices[i].id;
    }
  }
  if (music == nullptr || game == nullptr || chat == nullptr) {
    wprintf(L"missing endpoints\n");
    return 1;
  }
  unsigned long spotify = 0;
  {
    anymix::SessionInfo infos[32]{};
    std::wstring sessionError;
    const int sessions = anymix::EnumSessions(music, infos, 32, sessionError);
    for (int i = 0; i < sessions; ++i) {
      if (wcsstr(infos[i].name, L"Spotify") != nullptr) {
        spotify = infos[i].sessionId;
      }
    }
  }
  if (spotify != 0) {
    std::wstring moveError;
    const bool toChat = anymix::MoveSession(spotify, chat, moveError);
    wprintf(L"spotify %lu to chat %d %ls\n", spotify, toChat ? 1 : 0, moveError.c_str());
    Sleep(1000);
    const int onChat = HasPid(L"spotify chat", chat, spotify);
    const int onMusic = HasPid(L"spotify music", music, spotify);
    wprintf(L"spotify onChat %d onMusic %d\n", onChat, onMusic);
    moveError.clear();
    const bool back = anymix::MoveSession(spotify, music, moveError);
    wprintf(L"spotify back %d %ls\n", back ? 1 : 0, moveError.c_str());
    Sleep(1000);
    wprintf(L"spotify restored onMusic %d onChat %d\n", HasPid(L"back music", music, spotify),
            HasPid(L"back chat", chat, spotify));
  } else {
    wprintf(L"spotify not on music\n");
  }
  const unsigned long pid = GetCurrentProcessId();
  std::wstring error;
  const bool first = anymix::MoveSession(pid, game, error);
  wprintf(L"preset game %d %ls\n", first ? 1 : 0, error.c_str());
  IMMDeviceEnumerator* enumerator = nullptr;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                   reinterpret_cast<void**>(&enumerator));
  IMMDevice* device = nullptr;
  enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
  wchar_t opened[256] = {};
  for (int i = 0; i < count; ++i) {
    IMMDevice* candidate = nullptr;
    if (SUCCEEDED(enumerator->GetDevice(devices[i].id, &candidate)) && candidate != nullptr) {
      if (candidate == device) {
        wcsncpy(opened, devices[i].name, 255);
      }
      candidate->Release();
    }
  }
  LPWSTR openedId = nullptr;
  device->GetId(&openedId);
  wprintf(L"opened id %ls\n", openedId != nullptr ? openedId : L"");
  for (int i = 0; i < count; ++i) {
    if (openedId != nullptr && wcscmp(devices[i].id, openedId) == 0) {
      wprintf(L"opened name %ls\n", devices[i].name);
    }
  }
  IAudioClient* client = nullptr;
  device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
  WAVEFORMATEX* format = nullptr;
  client->GetMixFormat(&format);
  const HRESULT init = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, format, nullptr);
  wprintf(L"audio init 0x%08lx\n", static_cast<unsigned long>(init));
  client->Start();
  Sleep(400);
  const int bornMusic = HasPid(L"born music", music, pid);
  const int bornGame = HasPid(L"born game", game, pid);
  wprintf(L"born onMusic %d onGame %d\n", bornMusic, bornGame);
  error.clear();
  const bool toMusic = anymix::MoveSession(pid, music, error);
  wprintf(L"MoveSession music %d %ls\n", toMusic ? 1 : 0, error.c_str());
  Sleep(800);
  const int movedMusic = HasPid(L"after music", music, pid);
  const int movedGame = HasPid(L"after game", game, pid);
  wprintf(L"after music onMusic %d onGame %d\n", movedMusic, movedGame);
  if (openedId != nullptr) {
    CoTaskMemFree(openedId);
  }
  client->Stop();
  CoTaskMemFree(format);
  client->Release();
  device->Release();
  enumerator->Release();
  return (first && toMusic && bornGame == 1 && movedMusic == 1 && movedGame == 0) ? 0 : 2;
}
