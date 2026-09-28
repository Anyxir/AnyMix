#include "anymix/Devices.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <cstdio>

static int On(const wchar_t* endpoint, unsigned long pid) {
  anymix::SessionInfo infos[16]{};
  std::wstring error;
  const int count = anymix::EnumSessions(endpoint, infos, 16, error);
  for (int i = 0; i < count; ++i) {
    if (infos[i].sessionId == pid) {
      return 1;
    }
  }
  return 0;
}

int wmain() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
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
  IMMDeviceEnumerator* enumerator = nullptr;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                   reinterpret_cast<void**>(&enumerator));
  IMMDevice* device = nullptr;
  enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
  LPWSTR opened = nullptr;
  device->GetId(&opened);
  const wchar_t* originName = L"?";
  for (int i = 0; i < count; ++i) {
    if (wcscmp(devices[i].id, opened) == 0) {
      originName = devices[i].name;
    }
  }
  IAudioClient* client = nullptr;
  device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
  WAVEFORMATEX* format = nullptr;
  client->GetMixFormat(&format);
  client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, format, nullptr);
  client->Start();
  Sleep(300);
  const unsigned long pid = GetCurrentProcessId();
  const wchar_t* dest = chat;
  const wchar_t* destName = L"AnyMix Chat";
  if (opened != nullptr && chat != nullptr && wcscmp(opened, chat) == 0) {
    dest = music;
    destName = L"AnyMix Music";
  }
  std::wstring error;
  const bool moved = anymix::MoveSession(pid, dest, error);
  wprintf(L"from %ls to %ls set %d %ls\n", originName, destName, moved ? 1 : 0, error.c_str());
  Sleep(1000);
  wprintf(L"still on origin %d on dest %d\n", On(opened, pid), On(dest, pid));
  IMMDevice* again = nullptr;
  enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &again);
  LPWSTR againId = nullptr;
  again->GetId(&againId);
  const wchar_t* againName = L"?";
  for (int i = 0; i < count; ++i) {
    if (againId != nullptr && wcscmp(devices[i].id, againId) == 0) {
      againName = devices[i].name;
    }
  }
  wprintf(L"new default %ls\n", againName);
  client->Stop();
  CoTaskMemFree(format);
  CoTaskMemFree(opened);
  CoTaskMemFree(againId);
  client->Release();
  again->Release();
  device->Release();
  enumerator->Release();
  return 0;
}
