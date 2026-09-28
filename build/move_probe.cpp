#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <objbase.h>

#include "anymix/Devices.h"

#include <cstdio>
#include <string>

static void PrintHr(const wchar_t* label, HRESULT hr) {
  std::wprintf(L"%ls 0x%08lX\n", label, static_cast<unsigned long>(hr));
}

static HRESULT Policy(bool set, unsigned long pid, const wchar_t* endpoint, wchar_t* got, int gotCap, int* slotUsed) {
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
  const GUID iid21 = {0xab3d4648, 0xe242, 0x459f, {0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24}};
  const GUID iidOld = {0x2a59116d, 0x6c4f, 0x45e0, {0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58}};
  void* factory = nullptr;
  HRESULT hr = activate(classId, iid21, &factory);
  std::wprintf(L"activate 21H2 ");
  PrintHr(L"", hr);
  if (FAILED(hr) || factory == nullptr) {
    hr = activate(classId, iidOld, &factory);
    std::wprintf(L"activate older ");
    PrintHr(L"", hr);
  }
  deleteString(classId);
  if (FAILED(hr) || factory == nullptr) {
    return hr;
  }
  void** vt = *reinterpret_cast<void***>(factory);
  using SetFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT32, int, int, void*);
  using GetFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT32, int, int, void**);
  auto setFn = reinterpret_cast<SetFn>(vt[25]);
  auto getFn = reinterpret_cast<GetFn>(vt[26]);
  void* device = nullptr;
  if (set && endpoint != nullptr) {
    createString(endpoint, static_cast<UINT32>(wcslen(endpoint)), &device);
    hr = setFn(factory, pid, 0, 1, device);
    std::wprintf(L"set slot25 ");
    PrintHr(L"", hr);
    *slotUsed = 25;
    deleteString(device);
  }
  void* out = nullptr;
  const HRESULT getHr = getFn(factory, pid, 0, 1, &out);
  std::wprintf(L"get slot26 ");
  PrintHr(L"", getHr);
  if (SUCCEEDED(getHr) && out != nullptr && getRaw != nullptr && got != nullptr) {
    UINT32 rawLen = 0;
    const wchar_t* raw = getRaw(out, &rawLen);
    if (raw != nullptr) {
      wcsncpy(got, raw, static_cast<size_t>(gotCap - 1));
      got[gotCap - 1] = 0;
    }
    deleteString(out);
  }
  auto release = reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(void*)>(vt[2]);
  release(factory);
  return hr;
}

int wmain() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  HMODULE combase = LoadLibraryW(L"combase.dll");
  using RoInitFn = HRESULT(WINAPI*)(int);
  auto roInit = reinterpret_cast<RoInitFn>(GetProcAddress(combase, "RoInitialize"));
  if (roInit != nullptr) {
    const HRESULT ro = roInit(1);
    std::wprintf(L"RoInitialize ");
    PrintHr(L"", ro);
  }
  anymix::DeviceInfo devices[64]{};
  const int count = anymix::EnumRenderDevices(devices, 64);
  const wchar_t* game = nullptr;
  const wchar_t* chat = nullptr;
  for (int i = 0; i < count; ++i) {
    std::wprintf(L"DEV %ls | %ls\n", devices[i].name, devices[i].id);
    if (wcsstr(devices[i].name, L"AnyMix Game") != nullptr) {
      game = devices[i].id;
    }
    if (wcsstr(devices[i].name, L"AnyMix Chat") != nullptr) {
      chat = devices[i].id;
    }
  }
  if (game == nullptr || chat == nullptr) {
    std::wprintf(L"missing endpoints\n");
    return 1;
  }
  const unsigned long pid = GetCurrentProcessId();
  IMMDeviceEnumerator* enumerator = nullptr;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                   reinterpret_cast<void**>(&enumerator));
  IMMDevice* device = nullptr;
  enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
  IAudioClient* client = nullptr;
  device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
  WAVEFORMATEX* mix = nullptr;
  client->GetMixFormat(&mix);
  const HRESULT init = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, mix, nullptr);
  PrintHr(L"audio init", init);
  client->Start();
  CoTaskMemFree(mix);
  wchar_t wrapped[1024] = {};
  wsprintfW(wrapped, L"\\\\?\\SWD#MMDEVAPI#%ls#{e6327cad-dcec-4949-ae8a-991e976a79d2}", chat);
  std::wprintf(L"wrapped %ls\n", wrapped);
  wchar_t got[1024] = {};
  int slot = 0;
  Policy(true, pid, chat, got, 1024, &slot);
  std::wprintf(L"stored raw: %ls\n", got);
  Policy(true, pid, wrapped, got, 1024, &slot);
  std::wprintf(L"stored wrapped: %ls\n", got);
  Sleep(400);
  anymix::SessionInfo infos[16]{};
  std::wstring sessionError;
  const int onGame = anymix::EnumSessions(game, infos, 16, sessionError);
  int gameHit = 0;
  for (int i = 0; i < onGame; ++i) {
    if (infos[i].sessionId == pid) {
      gameHit = 1;
    }
    std::wprintf(L"GAME session %lu %ls\n", infos[i].sessionId, infos[i].name);
  }
  const int onChat = anymix::EnumSessions(chat, infos, 16, sessionError);
  int chatHit = 0;
  for (int i = 0; i < onChat; ++i) {
    if (infos[i].sessionId == pid) {
      chatHit = 1;
    }
    std::wprintf(L"CHAT session %lu %ls\n", infos[i].sessionId, infos[i].name);
  }
  std::wprintf(L"pid %lu onGame %d onChat %d\n", pid, gameHit, chatHit);
  client->Stop();
  client->Release();
  device->Release();
  enumerator->Release();
  std::wprintf(L"stored: %ls\n", got);
  std::wstring error;
  const bool moved = anymix::MoveSession(pid, game, error);
  std::wprintf(L"MoveSession %d %ls\n", moved ? 1 : 0, error.c_str());
  got[0] = 0;
  Policy(false, pid, nullptr, got, 1024, &slot);
  std::wprintf(L"after move stored: %ls\n", got);
  return 0;
}
