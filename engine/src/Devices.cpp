#include "anymix/Devices.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cstring>

namespace anymix {
namespace {

struct MeterVtbl {
  HRESULT(STDMETHODCALLTYPE* QueryInterface)(void* self, REFIID riid, void** object);
  ULONG(STDMETHODCALLTYPE* AddRef)(void* self);
  ULONG(STDMETHODCALLTYPE* Release)(void* self);
  HRESULT(STDMETHODCALLTYPE* GetPeakValue)(void* self, float* peak);
  HRESULT(STDMETHODCALLTYPE* GetMeteringChannelCount)(void* self, UINT* count);
  HRESULT(STDMETHODCALLTYPE* GetChannelsPeakValues)(void* self, UINT32 count, float* peaks);
  HRESULT(STDMETHODCALLTYPE* QueryHardwareSupport)(void* self, DWORD* mask);
};

struct Meter {
  MeterVtbl* vtbl;
};

const GUID kMeterIid = {0xC02216F6, 0x8C67, 0x4B5B, {0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64}};

float SessionPeak(IUnknown* unknown) {
  Meter* meter = nullptr;
  if (unknown == nullptr || FAILED(unknown->QueryInterface(kMeterIid, reinterpret_cast<void**>(&meter))) ||
      meter == nullptr || meter->vtbl == nullptr || meter->vtbl->GetPeakValue == nullptr) {
    return 0.f;
  }
  float peak = 0.f;
  const HRESULT hr = meter->vtbl->GetPeakValue(meter, &peak);
  if (meter->vtbl->Release != nullptr) {
    meter->vtbl->Release(meter);
  }
  if (FAILED(hr) || peak < 0.f) {
    return 0.f;
  }
  return peak > 1.f ? 1.f : peak;
}

void CopyText(wchar_t* dst, int capacity, const wchar_t* src) {
  if (capacity <= 0) {
    return;
  }
  if (src == nullptr) {
    dst[0] = 0;
    return;
  }
  wcsncpy(dst, src, static_cast<size_t>(capacity - 1));
  dst[capacity - 1] = 0;
}

class ComInit {
 public:
  ComInit() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    owned_ = SUCCEEDED(hr);
    ok_ = owned_ || hr == RPC_E_CHANGED_MODE;
  }
  ~ComInit() {
    if (owned_) {
      CoUninitialize();
    }
  }
  bool ok_ = false;
  bool owned_ = false;
};

bool EnumEndpoints(EDataFlow flow, DeviceInfo* out, int capacity, int& count) {
  count = 0;
  IMMDeviceEnumerator* enumerator = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator));
  if (FAILED(hr) || enumerator == nullptr) {
    return false;
  }
  IMMDeviceCollection* collection = nullptr;
  hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
  if (FAILED(hr) || collection == nullptr) {
    enumerator->Release();
    return false;
  }
  UINT total = 0;
  collection->GetCount(&total);
  for (UINT i = 0; i < total && count < capacity; ++i) {
    IMMDevice* device = nullptr;
    if (FAILED(collection->Item(i, &device)) || device == nullptr) {
      continue;
    }
    LPWSTR id = nullptr;
    if (SUCCEEDED(device->GetId(&id)) && id != nullptr) {
      CopyText(out[count].id, 512, id);
      CoTaskMemFree(id);
    }
    IPropertyStore* store = nullptr;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store != nullptr) {
      PROPVARIANT name;
      PropVariantInit(&name);
      if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR) {
        CopyText(out[count].name, 256, name.pwszVal);
      }
      PropVariantClear(&name);
      // Windows left these endpoints named "AnyMix". The KS pin path still
      // identifies Game, Chat, Music, Aux, Add, and B1–B5.
      static const PROPERTYKEY kPinPath = {
          {0x233164c8, 0x1b2c, 0x4c7d, {0xbc, 0x68, 0xb6, 0x71, 0x68, 0x7a, 0x25, 0x67}}, 1};
      PROPVARIANT path;
      PropVariantInit(&path);
      if (SUCCEEDED(store->GetValue(kPinPath, &path)) && path.vt == VT_LPWSTR && path.pwszVal != nullptr) {
        const wchar_t* pin = wcsrchr(path.pwszVal, L'\\');
        pin = pin == nullptr ? path.pwszVal : pin + 1;
        struct PinName {
          const wchar_t* pin;
          const wchar_t* name;
        };
        static const PinName pins[] = {
            {L"wavegame", L"AnyMix Game"},   {L"wavechat", L"AnyMix Chat"},
            {L"wavemusic", L"AnyMix Music"}, {L"waveaux", L"AnyMix Aux"},
            {L"waveadd", L"AnyMix Add"},     {L"waveb1", L"AnyMix B1"},
            {L"waveb2", L"AnyMix B2"},       {L"waveb3", L"AnyMix B3"},
            {L"waveb4", L"AnyMix B4"},       {L"waveb5", L"AnyMix B5"},
        };
        for (const PinName& item : pins) {
          if (_wcsicmp(pin, item.pin) == 0) {
            CopyText(out[count].name, 256, item.name);
            break;
          }
        }
      }
      PropVariantClear(&path);
      store->Release();
    }
    device->Release();
    if (out[count].name[0] == 0 || out[count].id[0] == 0) {
      out[count].name[0] = 0;
      out[count].id[0] = 0;
      continue;
    }
    ++count;
  }
  collection->Release();
  enumerator->Release();
  return true;
}

bool DefaultId(EDataFlow flow, wchar_t* id, int capacity, wchar_t* name, int nameCapacity) {
  IMMDeviceEnumerator* enumerator = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator));
  if (FAILED(hr) || enumerator == nullptr) {
    return false;
  }
  IMMDevice* device = nullptr;
  hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device);
  enumerator->Release();
  if (FAILED(hr) || device == nullptr) {
    return false;
  }
  LPWSTR endpoint = nullptr;
  if (SUCCEEDED(device->GetId(&endpoint)) && endpoint != nullptr) {
    CopyText(id, capacity, endpoint);
    CoTaskMemFree(endpoint);
  }
  if (name != nullptr) {
    IPropertyStore* store = nullptr;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store != nullptr) {
      PROPVARIANT value;
      PropVariantInit(&value);
      if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) {
        CopyText(name, nameCapacity, value.pwszVal);
      }
      PropVariantClear(&value);
      store->Release();
    }
  }
  device->Release();
  return id[0] != 0;
}

IMMDevice* DeviceById(const wchar_t* endpointId) {
  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&enumerator))) ||
      enumerator == nullptr) {
    return nullptr;
  }
  IMMDevice* device = nullptr;
  enumerator->GetDevice(endpointId, &device);
  enumerator->Release();
  return device;
}

}  // namespace

int EnumRenderDevices(DeviceInfo* out, int capacity) {
  ComInit com;
  int count = 0;
  if (!com.ok_ || !EnumEndpoints(eRender, out, capacity, count)) {
    return 0;
  }
  return count;
}

int EnumCaptureDevices(DeviceInfo* out, int capacity) {
  ComInit com;
  int count = 0;
  if (!com.ok_ || !EnumEndpoints(eCapture, out, capacity, count)) {
    return 0;
  }
  return count;
}

bool DefaultRenderId(wchar_t* id, int capacity, wchar_t* name, int nameCapacity) {
  ComInit com;
  if (!com.ok_) {
    return false;
  }
  return DefaultId(eRender, id, capacity, name, nameCapacity);
}

bool DefaultCaptureId(wchar_t* id, int capacity, wchar_t* name, int nameCapacity) {
  ComInit com;
  if (!com.ok_) {
    return false;
  }
  return DefaultId(eCapture, id, capacity, name, nameCapacity);
}

int EnumSessions(const wchar_t* endpointId, SessionInfo* out, int capacity, std::wstring& error) {
  error.clear();
  if (endpointId == nullptr || endpointId[0] == 0) {
    error = L"No Windows endpoint for this strip.";
    return 0;
  }
  ComInit com;
  if (!com.ok_) {
    error = L"COM init failed.";
    return 0;
  }
  IMMDevice* device = DeviceById(endpointId);
  if (device == nullptr) {
    error = L"Windows endpoint not found.";
    return 0;
  }
  IAudioSessionManager2* manager = nullptr;
  HRESULT hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&manager));
  device->Release();
  if (FAILED(hr) || manager == nullptr) {
    error = L"This endpoint does not expose audio sessions.";
    return 0;
  }
  IAudioSessionEnumerator* enumerator = nullptr;
  hr = manager->GetSessionEnumerator(&enumerator);
  if (FAILED(hr) || enumerator == nullptr) {
    manager->Release();
    error = L"Session enumerator failed.";
    return 0;
  }
  int count = 0;
  int total = 0;
  enumerator->GetCount(&total);
  for (int i = 0; i < total && count < capacity; ++i) {
    IAudioSessionControl* control = nullptr;
    if (FAILED(enumerator->GetSession(i, &control)) || control == nullptr) {
      continue;
    }
    IAudioSessionControl2* control2 = nullptr;
    control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&control2));
    DWORD pid = 0;
    if (control2 != nullptr) {
      control2->GetProcessId(&pid);
      if (control2->IsSystemSoundsSession() == S_OK) {
        control2->Release();
        control->Release();
        continue;
      }
      control2->Release();
    }
    if (pid == 0 || pid == GetCurrentProcessId()) {
      control->Release();
      continue;
    }
    LPWSTR display = nullptr;
    if (FAILED(control->GetDisplayName(&display)) || display == nullptr || display[0] == 0) {
      HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      wchar_t path[MAX_PATH] = {};
      DWORD size = MAX_PATH;
      if (process != nullptr && QueryFullProcessImageNameW(process, 0, path, &size)) {
        const wchar_t* base = wcsrchr(path, L'\\');
        CopyText(out[count].name, 256, base != nullptr ? base + 1 : path);
      } else {
        CopyText(out[count].name, 256, L"Application");
      }
      if (process != nullptr) {
        CloseHandle(process);
      }
    } else {
      CopyText(out[count].name, 256, display);
    }
    if (display != nullptr) {
      CoTaskMemFree(display);
    }
    if (_wcsicmp(out[count].name, L"LEDKeeper2.exe") == 0 || _wcsicmp(out[count].name, L"LEDKeeper2") == 0) {
      control->Release();
      continue;
    }
    out[count].sessionId = pid;
    ISimpleAudioVolume* volume = nullptr;
    if (SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume),
                                          reinterpret_cast<void**>(&volume))) &&
        volume != nullptr) {
      BOOL muted = FALSE;
      volume->GetMute(&muted);
      out[count].muted = muted == TRUE;
      float level = 1.f;
      if (SUCCEEDED(volume->GetMasterVolume(&level))) {
        out[count].volume = level < 0.f ? 0.f : (level > 1.f ? 1.f : level);
      }
      volume->Release();
    }
    out[count].peak = SessionPeak(control);
    control->Release();
    ++count;
  }
  enumerator->Release();
  manager->Release();
  return count;
}

void SampleSessionPeaks(const wchar_t* endpointId, SessionInfo* sessions, int count) {
  if (endpointId == nullptr || sessions == nullptr || count <= 0) {
    return;
  }
  if (count > 32) {
    count = 32;
  }
  ComInit com;
  if (!com.ok_) {
    return;
  }
  IMMDevice* device = DeviceById(endpointId);
  if (device == nullptr) {
    return;
  }
  IAudioSessionManager2* manager = nullptr;
  HRESULT hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&manager));
  device->Release();
  if (FAILED(hr) || manager == nullptr) {
    return;
  }
  IAudioSessionEnumerator* enumerator = nullptr;
  hr = manager->GetSessionEnumerator(&enumerator);
  if (FAILED(hr) || enumerator == nullptr) {
    manager->Release();
    return;
  }
  bool seen[32]{};
  float found[32]{};
  int total = 0;
  enumerator->GetCount(&total);
  for (int i = 0; i < total; ++i) {
    IAudioSessionControl* control = nullptr;
    if (FAILED(enumerator->GetSession(i, &control)) || control == nullptr) {
      continue;
    }
    IAudioSessionControl2* control2 = nullptr;
    DWORD pid = 0;
    if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                                          reinterpret_cast<void**>(&control2))) &&
        control2 != nullptr) {
      control2->GetProcessId(&pid);
      control2->Release();
    }
    const float peak = SessionPeak(control);
    control->Release();
    if (pid == 0) {
      continue;
    }
    for (int row = 0; row < count; ++row) {
      if (sessions[row].sessionId == pid) {
        seen[row] = true;
        if (peak > found[row]) {
          found[row] = peak;
        }
      }
    }
  }
  enumerator->Release();
  manager->Release();
  for (int row = 0; row < count; ++row) {
    sessions[row].peak = seen[row] ? found[row] : 0.f;
  }
}

bool SetSessionMute(const wchar_t* endpointId, unsigned long sessionId, bool mute, std::wstring& error) {
  error.clear();
  ComInit com;
  if (!com.ok_) {
    error = L"COM init failed.";
    return false;
  }
  IMMDevice* device = DeviceById(endpointId);
  if (device == nullptr) {
    error = L"Windows endpoint not found.";
    return false;
  }
  IAudioSessionManager2* manager = nullptr;
  HRESULT hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&manager));
  device->Release();
  if (FAILED(hr) || manager == nullptr) {
    error = L"Cannot control sessions on this endpoint.";
    return false;
  }
  IAudioSessionEnumerator* enumerator = nullptr;
  hr = manager->GetSessionEnumerator(&enumerator);
  if (FAILED(hr) || enumerator == nullptr) {
    manager->Release();
    error = L"Session enumerator failed.";
    return false;
  }
  bool found = false;
  int total = 0;
  enumerator->GetCount(&total);
  for (int i = 0; i < total && !found; ++i) {
    IAudioSessionControl* control = nullptr;
    if (FAILED(enumerator->GetSession(i, &control)) || control == nullptr) {
      continue;
    }
    IAudioSessionControl2* control2 = nullptr;
    DWORD pid = 0;
    if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                                          reinterpret_cast<void**>(&control2))) &&
        control2 != nullptr) {
      control2->GetProcessId(&pid);
      control2->Release();
    }
    if (pid == sessionId) {
      ISimpleAudioVolume* volume = nullptr;
      if (SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume),
                                            reinterpret_cast<void**>(&volume))) &&
          volume != nullptr) {
        hr = volume->SetMute(mute ? TRUE : FALSE, nullptr);
        volume->Release();
        found = SUCCEEDED(hr);
        if (!found) {
          error = L"SetMute failed.";
        }
      }
    }
    control->Release();
  }
  enumerator->Release();
  manager->Release();
  if (!found && error.empty()) {
    error = L"Application session is gone.";
  }
  return found;
}

bool SetSessionVolume(const wchar_t* endpointId, unsigned long sessionId, float level, std::wstring& error) {
  error.clear();
  if (level < 0.f) {
    level = 0.f;
  } else if (level > 1.f) {
    level = 1.f;
  }
  ComInit com;
  if (!com.ok_) {
    error = L"COM init failed.";
    return false;
  }
  IMMDevice* device = DeviceById(endpointId);
  if (device == nullptr) {
    error = L"Windows endpoint not found.";
    return false;
  }
  IAudioSessionManager2* manager = nullptr;
  HRESULT hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&manager));
  device->Release();
  if (FAILED(hr) || manager == nullptr) {
    error = L"Cannot control sessions on this endpoint.";
    return false;
  }
  IAudioSessionEnumerator* enumerator = nullptr;
  hr = manager->GetSessionEnumerator(&enumerator);
  if (FAILED(hr) || enumerator == nullptr) {
    manager->Release();
    error = L"Session enumerator failed.";
    return false;
  }
  bool found = false;
  int total = 0;
  enumerator->GetCount(&total);
  for (int i = 0; i < total && !found; ++i) {
    IAudioSessionControl* control = nullptr;
    if (FAILED(enumerator->GetSession(i, &control)) || control == nullptr) {
      continue;
    }
    IAudioSessionControl2* control2 = nullptr;
    DWORD pid = 0;
    if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                                          reinterpret_cast<void**>(&control2))) &&
        control2 != nullptr) {
      control2->GetProcessId(&pid);
      control2->Release();
    }
    if (pid == sessionId) {
      ISimpleAudioVolume* volume = nullptr;
      if (SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume),
                                            reinterpret_cast<void**>(&volume))) &&
          volume != nullptr) {
        hr = volume->SetMasterVolume(level, nullptr);
        volume->Release();
        found = SUCCEEDED(hr);
        if (!found) {
          error = L"SetMasterVolume failed.";
        }
      }
    }
    control->Release();
  }
  enumerator->Release();
  manager->Release();
  if (!found && error.empty()) {
    error = L"Application session is gone.";
  }
  return found;
}

bool MoveSession(unsigned long processId, const wchar_t* endpointId, std::wstring& error) {
  error.clear();
  if (processId == 0 || endpointId == nullptr || endpointId[0] == 0) {
    error = L"This application cannot be moved.";
    return false;
  }
  ComInit com;
  if (!com.ok_) {
    error = L"Windows cannot move application audio.";
    return false;
  }
  HMODULE combase = GetModuleHandleW(L"combase.dll");
  if (combase == nullptr) {
    combase = LoadLibraryW(L"combase.dll");
  }
  if (combase == nullptr) {
    error = L"Windows cannot move application audio.";
    return false;
  }
  using RoInitFn = HRESULT(WINAPI*)(int);
  auto roInit = reinterpret_cast<RoInitFn>(GetProcAddress(combase, "RoInitialize"));
  if (roInit != nullptr) {
    roInit(1);
  }
  using CreateStringFn = HRESULT(WINAPI*)(const wchar_t*, UINT32, void**);
  using DeleteStringFn = HRESULT(WINAPI*)(void*);
  using ActivateFn = HRESULT(WINAPI*)(void*, const GUID&, void**);
  auto createString = reinterpret_cast<CreateStringFn>(GetProcAddress(combase, "WindowsCreateString"));
  auto deleteString = reinterpret_cast<DeleteStringFn>(GetProcAddress(combase, "WindowsDeleteString"));
  auto activate = reinterpret_cast<ActivateFn>(GetProcAddress(combase, "RoGetActivationFactory"));
  if (createString == nullptr || deleteString == nullptr || activate == nullptr) {
    error = L"Windows cannot move application audio.";
    return false;
  }
  wchar_t wrapped[640] = {};
  const wchar_t* deviceText = endpointId;
  if (wcsstr(endpointId, L"SWD#MMDEVAPI#") == nullptr) {
    wsprintfW(wrapped, L"\\\\?\\SWD#MMDEVAPI#%ls#{e6327cad-dcec-4949-ae8a-991e976a79d2}", endpointId);
    deviceText = wrapped;
  }
  const wchar_t* className = L"Windows.Media.Internal.AudioPolicyConfig";
  void* classId = nullptr;
  if (FAILED(createString(className, static_cast<UINT32>(wcslen(className)), &classId)) || classId == nullptr) {
    error = L"Windows cannot move application audio.";
    return false;
  }
  const GUID iid21H2 = {0xab3d4648, 0xe242, 0x459f, {0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24}};
  const GUID iidOlder = {0x2a59116d, 0x6c4f, 0x45e0, {0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58}};
  void* factory = nullptr;
  HRESULT hr = activate(classId, iid21H2, &factory);
  if (FAILED(hr) || factory == nullptr) {
    hr = activate(classId, iidOlder, &factory);
  }
  deleteString(classId);
  if (FAILED(hr) || factory == nullptr) {
    error = L"Windows cannot move application audio.";
    return false;
  }
  void* deviceId = nullptr;
  if (FAILED(createString(deviceText, static_cast<UINT32>(wcslen(deviceText)), &deviceId)) || deviceId == nullptr) {
    auto release = reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(void*)>((*reinterpret_cast<void***>(factory))[2]);
    release(factory);
    error = L"That AnyMix channel is not available.";
    return false;
  }
  using SetEndpointFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT32, int, int, void*);
  auto setEndpoint = reinterpret_cast<SetEndpointFn>((*reinterpret_cast<void***>(factory))[25]);
  bool moved = false;
  for (int role = 0; role < 3; ++role) {
    if (SUCCEEDED(setEndpoint(factory, processId, 0, role, deviceId))) {
      moved = true;
    }
  }
  deleteString(deviceId);
  auto release = reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(void*)>((*reinterpret_cast<void***>(factory))[2]);
  release(factory);
  if (!moved) {
    error = L"This application does not allow its playback device to be changed.";
    return false;
  }
  return true;
}

}  // namespace anymix
