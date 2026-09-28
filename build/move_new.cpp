#include "anymix/Devices.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <cstdio>

static const wchar_t* NameOf(anymix::DeviceInfo* devices, int count, const wchar_t* id) {
  for (int i = 0; i < count; ++i) {
    if (wcscmp(devices[i].id, id) == 0) {
      return devices[i].name;
    }
  }
  return L"?";
}

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
  const wchar_t* music = nullptr;
  const wchar_t* chat = nullptr;
  for (int i = 0; i < count; ++i) {
    if (wcscmp(devices[i].name, L"AnyMix Music") == 0) {
      music = devices[i].id;
    }
    if (wcscmp(devices[i].name, L"AnyMix Chat") == 0) {
      chat = devices[i].id;
    }
  }
  const unsigned long pid = GetCurrentProcessId();
  std::wstring error;
  wprintf(L"set music %d\n", anymix::MoveSession(pid, music, error) ? 1 : 0);
  IMMDeviceEnumerator* enumerator = nullptr;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                   reinterpret_cast<void**>(&enumerator));
  IMMDevice* firstDevice = nullptr;
  enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &firstDevice);
  LPWSTR firstId = nullptr;
  firstDevice->GetId(&firstId);
  wprintf(L"first default %ls\n", NameOf(devices, count, firstId));
  IAudioClient* first = nullptr;
  firstDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&first));
  WAVEFORMATEX* format = nullptr;
  first->GetMixFormat(&format);
  first->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, format, nullptr);
  first->Start();
  Sleep(300);
  wprintf(L"first session music %d chat %d\n", On(music, pid), On(chat, pid));
  error.clear();
  wprintf(L"set chat %d\n", anymix::MoveSession(pid, chat, error) ? 1 : 0);
  IMMDevice* secondDevice = nullptr;
  enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &secondDevice);
  LPWSTR secondId = nullptr;
  secondDevice->GetId(&secondId);
  wprintf(L"second default %ls\n", NameOf(devices, count, secondId));
  IAudioClient* second = nullptr;
  secondDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&second));
  WAVEFORMATEX* format2 = nullptr;
  second->GetMixFormat(&format2);
  second->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, format2, nullptr);
  second->Start();
  Sleep(400);
  wprintf(L"after second session music %d chat %d\n", On(music, pid), On(chat, pid));
  first->Stop();
  second->Stop();
  CoTaskMemFree(format);
  CoTaskMemFree(format2);
  CoTaskMemFree(firstId);
  CoTaskMemFree(secondId);
  second->Release();
  first->Release();
  secondDevice->Release();
  firstDevice->Release();
  enumerator->Release();
  return 0;
}
