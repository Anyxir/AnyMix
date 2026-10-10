// AnyMix Insert ASIO. Interface layout matches ASIO SDK 2.3 (NATIVE_INT64 = 0).
// Cantabile loads this DLL and exchanges 5 stereo pairs with the engine.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>

#include <timeapi.h>

#include "anymix/Types.h"
#include "anymix/Bridge.h"

namespace {

constexpr long kAseOk = 0;
constexpr long kAseNotPresent = -1000;
constexpr long kAseInvalidParameter = -998;
constexpr long kAseInvalidMode = -997;
constexpr long kAseNoClock = -995;
constexpr long kAseNoMemory = -994;
constexpr long kAsioFloat32Lsb = 19;
constexpr long kAsioSupportsTimeInfo = 7;

// Private bridge for SelfCheck(): the production bridge is shared with a live
// engine that publishes blocks continuously and would overwrite the sentinels
// before the pump copies them. This mapping exercises the exact same pump code
// with no interference from a running AnyMix instance.
constexpr wchar_t kSelfCheckMappingName[] = L"Local\\AnyMix.AsioInsert3.SelfCheck";

using AsioBool = long;
using AsioError = long;
using AsioSampleRate = double;
using AsioSampleType = long;

struct AsioSamples {
  unsigned long hi;
  unsigned long lo;
};
struct AsioTimeStamp {
  unsigned long hi;
  unsigned long lo;
};
struct AsioClockSource {
  long index;
  long associatedChannel;
  long associatedGroup;
  AsioBool isCurrentSource;
  char name[32];
};
struct AsioChannelInfo {
  long channel;
  AsioBool isInput;
  AsioBool isActive;
  long channelGroup;
  AsioSampleType type;
  char name[32];
};
struct AsioBufferInfo {
  AsioBool isInput;
  long channelNum;
  void* buffers[2];
};
struct AsioTimeCode {
  double speed;
  AsioSamples timeCodeSamples;
  unsigned long flags;
  char future[64];
};
struct AsioTimeInfo {
  double speed;
  AsioTimeStamp systemTime;
  AsioSamples samplePosition;
  double sampleRate;
  unsigned long flags;
  char reserved[12];
};
struct AsioTime {
  long reserved[4];
  AsioTimeInfo timeInfo;
  AsioTimeCode timeCode;
};
struct AsioCallbacks {
  void (*bufferSwitch)(long doubleBufferIndex, AsioBool directProcess);
  void (*sampleRateDidChange)(AsioSampleRate rate);
  long (*asioMessage)(long selector, long value, void* message, double* opt);
  AsioTime* (*bufferSwitchTimeInfo)(AsioTime* params, long doubleBufferIndex, AsioBool directProcess);
};

const char* kChannelNames[anymix::kAsioChannels] = {
    "Game L", "Game R", "Chat L", "Chat R", "Music L", "Music R", "Aux L",
    "Aux R",  "Add L",  "Add R",  "Mic L",  "Mic R",   "Mic2 L",  "Mic2 R"};

const CLSID kClsid = {0x8f3a6c21, 0x4b7e, 0x4d15, {0x9a, 0x62, 0x1c, 0x0e, 0x5b, 0x7d, 0x4a, 0x90}};

void LaunchAnyMixEngine() {
  HANDLE held = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\AnyMix.SingleInstance");
  if (held != nullptr) {
    CloseHandle(held);
    return;
  }
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&LaunchAnyMixEngine), &module)) {
    return;
  }
  wchar_t folder[MAX_PATH]{};
  GetModuleFileNameW(module, folder, MAX_PATH);
  wchar_t* slash = wcsrchr(folder, L'\\');
  if (slash == nullptr) {
    return;
  }
  *slash = 0;
  wchar_t exe[MAX_PATH]{};
  wsprintfW(exe, L"%s\\AnyMix.exe", folder);
  if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
    return;
  }
  wchar_t command[MAX_PATH + 32]{};
  wsprintfW(command, L"\"%s\" --startup", exe);
  STARTUPINFOW info{};
  info.cb = sizeof(info);
  PROCESS_INFORMATION process{};
  if (CreateProcessW(exe, command, nullptr, nullptr, FALSE, 0, nullptr, folder, &info, &process)) {
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  }
}

class InsertDriver {
 public:
  friend int AnyMix_CheckInsert();
  InsertDriver() = default;
  ~InsertDriver() { StopThread(); }

  HRESULT QueryInterface(REFIID riid, void** out) {
    if (out == nullptr) {
      return E_POINTER;
    }
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualGUID(riid, kClsid)) {
      *out = this;
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG AddRef() { return ++refs_; }
  ULONG Release() {
    const ULONG left = --refs_;
    if (left == 0) {
      delete this;
    }
    return left;
  }

  AsioBool Init(void*) {
    if (launchMixer_) {
      LaunchAnyMixEngine();
    }
    return BindBridge(anymix::kAsioMappingName) ? 1 : 0;
  }
  void GetDriverName(char* name) {
    std::strncpy(name, "AnyMix Insert ASIO", 31);
    name[31] = 0;
  }
  long GetDriverVersion() { return 1; }
  void GetErrorMessage(char* text) { std::strncpy(text, lastError_, 123); }
  AsioError Start() {
    if (run_) {
      return kAseOk;
    }
    if (callbacks_ == nullptr || channelCount_ == 0) {
      return kAseInvalidMode;
    }
    if (callbacks_->asioMessage != nullptr) {
      callbacks_->asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr);
    }
    timeInfo_ = callbacks_->bufferSwitchTimeInfo != nullptr;
    if (bridge_ != nullptr) {
      bridge_->hostRunning = 1;
    }
    run_ = true;
    worker_ = std::thread([this] { ThreadMain(); });
    return kAseOk;
  }
  AsioError Stop() {
    StopThread();
    if (bridge_ != nullptr) {
      bridge_->hostRunning = 0;
    }
    return kAseOk;
  }
  AsioError GetChannels(long* inputs, long* outputs) {
    *inputs = anymix::kAsioChannels;
    *outputs = anymix::kAsioChannels;
    return kAseOk;
  }
  AsioError GetLatencies(long* input, long* output) {
    *input = anymix::kBlockFrames;
    *output = anymix::kBlockFrames;
    return kAseOk;
  }
  AsioError GetBufferSize(long* minSize, long* maxSize, long* preferred, long* granularity) {
    *minSize = anymix::kBlockFrames;
    *maxSize = anymix::kBlockFrames;
    *preferred = anymix::kBlockFrames;
    *granularity = 0;
    return kAseOk;
  }
  AsioError CanSampleRate(AsioSampleRate rate) { return rate == 48000.0 ? kAseOk : kAseNoClock; }
  AsioError GetSampleRate(AsioSampleRate* rate) {
    *rate = 48000.0;
    return kAseOk;
  }
  AsioError SetSampleRate(AsioSampleRate rate) { return rate == 48000.0 ? kAseOk : kAseInvalidParameter; }
  AsioError GetClockSources(AsioClockSource* clocks, long* count) {
    clocks[0].index = 0;
    clocks[0].associatedChannel = 0;
    clocks[0].associatedGroup = 0;
    clocks[0].isCurrentSource = 1;
    std::strncpy(clocks[0].name, "AnyMix", 31);
    *count = 1;
    return kAseOk;
  }
  AsioError SetClockSource(long reference) { return reference == 0 ? kAseOk : kAseInvalidParameter; }
  AsioError GetSamplePosition(AsioSamples* pos, AsioTimeStamp* stamp) {
    const unsigned long long samples = samplePos_;
    pos->hi = static_cast<unsigned long>(samples >> 32);
    pos->lo = static_cast<unsigned long>(samples);
    const unsigned long long now = SystemTimeNs();
    stamp->hi = static_cast<unsigned long>(now >> 32);
    stamp->lo = static_cast<unsigned long>(now);
    return kAseOk;
  }
  AsioError GetChannelInfo(AsioChannelInfo* info) {
    if (info->channel < 0 || info->channel >= anymix::kAsioChannels) {
      return kAseInvalidParameter;
    }
    info->isActive = channelCount_ != 0;
    info->channelGroup = info->channel / 2;
    info->type = kAsioFloat32Lsb;
    std::strncpy(info->name, kChannelNames[info->channel], 31);
    return kAseOk;
  }
  AsioError CreateBuffers(AsioBufferInfo* infos, long channels, long bufferSize, AsioCallbacks* callbacks) {
    if (bufferSize != anymix::kBlockFrames || channels <= 0 || channels > 28 || callbacks == nullptr) {
      return kAseInvalidParameter;
    }
    DisposeBuffers();
    owned_ = new (std::nothrow) float[static_cast<size_t>(channels) * 2 * anymix::kBlockFrames];
    if (owned_ == nullptr) {
      return kAseNoMemory;
    }
    std::memset(owned_, 0, sizeof(float) * static_cast<size_t>(channels) * 2 * anymix::kBlockFrames);
    for (long i = 0; i < channels; ++i) {
      infos[i].buffers[0] = owned_ + (static_cast<size_t>(i) * 2) * anymix::kBlockFrames;
      infos[i].buffers[1] = owned_ + (static_cast<size_t>(i) * 2 + 1) * anymix::kBlockFrames;
      held_[i].isInput = infos[i].isInput;
      held_[i].channelNum = infos[i].channelNum;
      held_[i].samples[0] = static_cast<float*>(infos[i].buffers[0]);
      held_[i].samples[1] = static_cast<float*>(infos[i].buffers[1]);
    }
    channelCount_ = channels;
    callbacks_ = callbacks;
    PublishMask();
    return kAseOk;
  }
  AsioError DisposeBuffers() {
    StopThread();
    delete[] owned_;
    owned_ = nullptr;
    callbacks_ = nullptr;
    channelCount_ = 0;
    if (bridge_ != nullptr) {
      bridge_->outputMask = 0;
    }
    return kAseOk;
  }
  AsioError ControlPanel() { return kAseNotPresent; }
  AsioError Future(long, void*) { return kAseNotPresent; }
  AsioError OutputReady() { return kAseNotPresent; }
  int SelfCheck();

 private:
  bool BindBridge(const wchar_t* name) {
    map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                              static_cast<DWORD>(sizeof(anymix::AsioBridge)), name);
    if (map_ == nullptr) {
      return false;
    }
    bridge_ = static_cast<anymix::AsioBridge*>(
        MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(anymix::AsioBridge)));
    if (bridge_ == nullptr) {
      CloseHandle(map_);
      map_ = nullptr;
      return false;
    }
    if (bridge_->magic != anymix::kAsioMagic || bridge_->version != anymix::kAsioVersion ||
        bridge_->channels != anymix::kAsioChannels) {
      std::memset(bridge_, 0, sizeof(anymix::AsioBridge));
      bridge_->magic = anymix::kAsioMagic;
      bridge_->version = anymix::kAsioVersion;
      bridge_->bufferFrames = anymix::kBlockFrames;
      bridge_->channels = anymix::kAsioChannels;
    }
    return true;
  }

  void StopThread() {
    run_ = false;
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  void PublishMask() {
    if (bridge_ == nullptr) {
      return;
    }
    uint32_t mask = 0;
    for (long channel = 0; channel < channelCount_; ++channel) {
      if (held_[channel].isInput != 0) {
        continue;
      }
      const long number = held_[channel].channelNum;
      if (number >= 0 && number < anymix::kAsioChannels) {
        mask |= 1u << number;
      }
    }
    bridge_->outputMask = mask;
  }

  static unsigned long long SystemTimeNs() {
    static const LARGE_INTEGER freq = [] {
      LARGE_INTEGER value{};
      QueryPerformanceFrequency(&value);
      return value;
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (freq.QuadPart <= 0) {
      return 0;
    }
    return static_cast<unsigned long long>(now.QuadPart) * 1000000000ull /
           static_cast<unsigned long long>(freq.QuadPart);
  }

  void ThreadMain() {
    long index = 0;
    uint32_t seen = 0;
    DWORD mmcssTask = 0;
    HANDLE mmcss = nullptr;
    const HMODULE avrt = LoadLibraryW(L"avrt.dll");
    if (avrt != nullptr) {
      using SetCharacteristics = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
      const auto set =
          reinterpret_cast<SetCharacteristics>(GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW"));
      if (set != nullptr) {
        mmcss = set(L"Pro Audio", &mmcssTask);
        if (mmcss == nullptr) {
          mmcss = set(L"Audio", &mmcssTask);
        }
      }
    }
    if (mmcss == nullptr) {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }
    timeBeginPeriod(1);
    HANDLE tick = CreateEventW(nullptr, FALSE, FALSE, anymix::kAsioTickName);
    constexpr uint32_t slotMask = static_cast<uint32_t>(anymix::kAsioBufferSlots - 1);
    while (run_) {
      if (bridge_ == nullptr) {
        Sleep(2);
        continue;
      }
      const uint32_t seq = bridge_->sendSeq;
      if (seq == seen || seq == 0) {
        if (tick != nullptr) {
          WaitForSingleObject(tick, 20);
        } else {
          Sleep(2);
        }
        continue;
      }
      uint32_t next = seen == 0 ? seq : seen + 1;
      if (seen != 0 && seq - next >= static_cast<uint32_t>(anymix::kAsioBufferSlots)) {
        next = seq - (static_cast<uint32_t>(anymix::kAsioBufferSlots) - 1);
      }
      while (next <= seq && run_) {
        CopyToInputs(index, bridge_->send[(next - 1) & slotMask]);
        SwitchBuffer(index);
        CopyFromOutputs(index);
        index ^= 1;
        samplePos_ += anymix::kBlockFrames;
        ++next;
      }
      seen = seq;
    }
    if (tick != nullptr) {
      CloseHandle(tick);
    }
    timeEndPeriod(1);
    if (mmcss != nullptr && avrt != nullptr) {
      using Revert = BOOL(WINAPI*)(HANDLE);
      const auto revert = reinterpret_cast<Revert>(GetProcAddress(avrt, "AvRevertMmThreadCharacteristics"));
      if (revert != nullptr) {
        revert(mmcss);
      }
    }
    if (avrt != nullptr) {
      FreeLibrary(avrt);
    }
  }

  void SwitchBuffer(long index) {
    if (callbacks_ == nullptr) {
      return;
    }
    if (timeInfo_ && callbacks_->bufferSwitchTimeInfo != nullptr) {
      AsioTime time{};
      const unsigned long long position = samplePos_;
      const unsigned long long now = SystemTimeNs();
      time.timeInfo.speed = 1.0;
      time.timeInfo.sampleRate = 48000.0;
      time.timeInfo.flags = 1u | 2u | 4u | 8u;
      time.timeInfo.samplePosition.hi = static_cast<unsigned long>(position >> 32);
      time.timeInfo.samplePosition.lo = static_cast<unsigned long>(position);
      time.timeInfo.systemTime.hi = static_cast<unsigned long>(now >> 32);
      time.timeInfo.systemTime.lo = static_cast<unsigned long>(now);
      callbacks_->bufferSwitchTimeInfo(&time, index, 1);
      return;
    }
    if (callbacks_->bufferSwitch != nullptr) {
      callbacks_->bufferSwitch(index, 1);
    }
  }

  void CopyToInputs(long index, const float* interleaved) {
    const int side = index & 1;
    for (long channel = 0; channel < channelCount_; ++channel) {
      if (held_[channel].isInput == 0) {
        continue;
      }
      float* dst = held_[channel].samples[side];
      const long srcChannel = held_[channel].channelNum;
      if (dst == nullptr || srcChannel < 0 || srcChannel >= anymix::kAsioChannels) {
        continue;
      }
      for (int frame = 0; frame < anymix::kBlockFrames; ++frame) {
        dst[frame] = interleaved[frame * anymix::kAsioChannels + srcChannel];
      }
    }
  }

  void CopyFromOutputs(long index) {
    if (bridge_ == nullptr) {
      return;
    }
    const int side = index & 1;
    const uint32_t slot = bridge_->returnSeq & static_cast<uint32_t>(anymix::kAsioBufferSlots - 1);
    float* dst = bridge_->ret[slot];
    std::memset(dst, 0, sizeof(float) * anymix::kBlockFrames * anymix::kAsioChannels);
    for (long channel = 0; channel < channelCount_; ++channel) {
      if (held_[channel].isInput != 0) {
        continue;
      }
      float* src = held_[channel].samples[side];
      const long dstChannel = held_[channel].channelNum;
      if (src == nullptr || dstChannel < 0 || dstChannel >= anymix::kAsioChannels) {
        continue;
      }
      for (int frame = 0; frame < anymix::kBlockFrames; ++frame) {
        dst[frame * anymix::kAsioChannels + dstChannel] = src[frame];
      }
    }
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&bridge_->returnSeq));
  }

  std::atomic<ULONG> refs_{1};
  HANDLE map_ = nullptr;
  anymix::AsioBridge* bridge_ = nullptr;
  struct HeldChannel {
    long isInput = 0;
    long channelNum = 0;
    float* samples[2] = {};
  };
  float* owned_ = nullptr;
  HeldChannel held_[28]{};
  AsioCallbacks* callbacks_ = nullptr;
  long channelCount_ = 0;
  std::atomic<bool> run_{false};
  bool timeInfo_ = false;
  std::thread worker_;
  unsigned long long samplePos_ = 0;
  bool launchMixer_ = true;
  char lastError_[128] = {};
};

// The host sees a COM vtable. QueryInterface through OutputReady must stay in this order.
struct ComDriver {
  virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) = 0;
  virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
  virtual ULONG STDMETHODCALLTYPE Release() = 0;
  virtual AsioBool Init(void* sysHandle) = 0;
  virtual void GetDriverName(char* name) = 0;
  virtual long GetDriverVersion() = 0;
  virtual void GetErrorMessage(char* text) = 0;
  virtual AsioError Start() = 0;
  virtual AsioError Stop() = 0;
  virtual AsioError GetChannels(long* inputs, long* outputs) = 0;
  virtual AsioError GetLatencies(long* input, long* output) = 0;
  virtual AsioError GetBufferSize(long* minSize, long* maxSize, long* preferred, long* granularity) = 0;
  virtual AsioError CanSampleRate(AsioSampleRate rate) = 0;
  virtual AsioError GetSampleRate(AsioSampleRate* rate) = 0;
  virtual AsioError SetSampleRate(AsioSampleRate rate) = 0;
  virtual AsioError GetClockSources(AsioClockSource* clocks, long* count) = 0;
  virtual AsioError SetClockSource(long reference) = 0;
  virtual AsioError GetSamplePosition(AsioSamples* pos, AsioTimeStamp* stamp) = 0;
  virtual AsioError GetChannelInfo(AsioChannelInfo* info) = 0;
  virtual AsioError CreateBuffers(AsioBufferInfo* infos, long channels, long bufferSize, AsioCallbacks* callbacks) = 0;
  virtual AsioError DisposeBuffers() = 0;
  virtual AsioError ControlPanel() = 0;
  virtual AsioError Future(long selector, void* opt) = 0;
  virtual AsioError OutputReady() = 0;
};

class AsioCom final : public ComDriver {
 public:
  explicit AsioCom(InsertDriver* inner) : inner_(inner) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (out == nullptr) {
      return E_POINTER;
    }
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualGUID(riid, kClsid)) {
      *out = static_cast<ComDriver*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = --refs_;
    if (left == 0) {
      delete inner_;
      delete this;
    }
    return left;
  }
  AsioBool Init(void* sysHandle) override { return inner_->Init(sysHandle); }
  void GetDriverName(char* name) override { inner_->GetDriverName(name); }
  long GetDriverVersion() override { return inner_->GetDriverVersion(); }
  void GetErrorMessage(char* text) override { inner_->GetErrorMessage(text); }
  AsioError Start() override { return inner_->Start(); }
  AsioError Stop() override { return inner_->Stop(); }
  AsioError GetChannels(long* inputs, long* outputs) override { return inner_->GetChannels(inputs, outputs); }
  AsioError GetLatencies(long* input, long* output) override { return inner_->GetLatencies(input, output); }
  AsioError GetBufferSize(long* a, long* b, long* c, long* d) override { return inner_->GetBufferSize(a, b, c, d); }
  AsioError CanSampleRate(AsioSampleRate rate) override { return inner_->CanSampleRate(rate); }
  AsioError GetSampleRate(AsioSampleRate* rate) override { return inner_->GetSampleRate(rate); }
  AsioError SetSampleRate(AsioSampleRate rate) override { return inner_->SetSampleRate(rate); }
  AsioError GetClockSources(AsioClockSource* clocks, long* count) override {
    return inner_->GetClockSources(clocks, count);
  }
  AsioError SetClockSource(long reference) override { return inner_->SetClockSource(reference); }
  AsioError GetSamplePosition(AsioSamples* pos, AsioTimeStamp* stamp) override {
    return inner_->GetSamplePosition(pos, stamp);
  }
  AsioError GetChannelInfo(AsioChannelInfo* info) override { return inner_->GetChannelInfo(info); }
  AsioError CreateBuffers(AsioBufferInfo* infos, long channels, long bufferSize, AsioCallbacks* callbacks) override {
    return inner_->CreateBuffers(infos, channels, bufferSize, callbacks);
  }
  AsioError DisposeBuffers() override { return inner_->DisposeBuffers(); }
  AsioError ControlPanel() override { return inner_->ControlPanel(); }
  AsioError Future(long selector, void* opt) override { return inner_->Future(selector, opt); }
  AsioError OutputReady() override { return inner_->OutputReady(); }

 private:
  InsertDriver* inner_;
  std::atomic<ULONG> refs_{1};
};

class Factory : public IClassFactory {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
      *out = this;
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = --refs_;
    if (left == 0) {
      delete this;
    }
    return left;
  }
  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID, void** out) override {
    if (outer != nullptr) {
      return CLASS_E_NOAGGREGATION;
    }
    auto* inner = new (std::nothrow) InsertDriver();
    if (inner == nullptr) {
      return E_OUTOFMEMORY;
    }
    auto* com = new (std::nothrow) AsioCom(inner);
    if (com == nullptr) {
      delete inner;
      return E_OUTOFMEMORY;
    }
    *out = com;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }

 private:
  std::atomic<ULONG> refs_{1};
};

LONG WriteKey(HKEY root, const wchar_t* path, const wchar_t* name, const wchar_t* value) {
  HKEY key = nullptr;
  LONG status = RegCreateKeyExW(root, path, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
  if (status != ERROR_SUCCESS) {
    return status;
  }
  status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
                          static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  return status;
}

}  // namespace

extern "C" HRESULT STDMETHODCALLTYPE DllGetClassObject(REFCLSID clsid, REFIID riid, void** out) {
  if (!IsEqualGUID(clsid, kClsid)) {
    return CLASS_E_CLASSNOTAVAILABLE;
  }
  Factory* factory = new (std::nothrow) Factory();
  if (factory == nullptr) {
    return E_OUTOFMEMORY;
  }
  const HRESULT hr = factory->QueryInterface(riid, out);
  factory->Release();
  return hr;
}

extern "C" HRESULT STDMETHODCALLTYPE DllCanUnloadNow() { return S_FALSE; }

extern "C" HRESULT STDMETHODCALLTYPE DllRegisterServer() {
  wchar_t path[MAX_PATH] = {};
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&DllRegisterServer), &module)) {
    return E_FAIL;
  }
  GetModuleFileNameW(module, path, MAX_PATH);
  const wchar_t* clsid = L"{8F3A6C21-4B7E-4D15-9A62-1C0E5B7D4A90}";
  if (WriteKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO\\AnyMix Insert ASIO", L"CLSID", clsid) != ERROR_SUCCESS ||
      WriteKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO\\AnyMix Insert ASIO", L"Description", L"AnyMix Insert ASIO") !=
          ERROR_SUCCESS) {
    return E_ACCESSDENIED;
  }
  wchar_t clsidKey[128] = {};
  wsprintfW(clsidKey, L"SOFTWARE\\Classes\\CLSID\\%s", clsid);
  WriteKey(HKEY_LOCAL_MACHINE, clsidKey, nullptr, L"AnyMix Insert ASIO");
  wchar_t inproc[160] = {};
  wsprintfW(inproc, L"SOFTWARE\\Classes\\CLSID\\%s\\InprocServer32", clsid);
  if (WriteKey(HKEY_LOCAL_MACHINE, inproc, nullptr, path) != ERROR_SUCCESS) {
    return E_ACCESSDENIED;
  }
  WriteKey(HKEY_LOCAL_MACHINE, inproc, L"ThreadingModel", L"Both");
  return S_OK;
}

extern "C" HRESULT STDMETHODCALLTYPE DllUnregisterServer() {
  RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO\\AnyMix Insert ASIO");
  RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\{8F3A6C21-4B7E-4D15-9A62-1C0E5B7D4A90}");
  return S_OK;
}

int InsertDriver::SelfCheck() {
  launchMixer_ = false;
  // The production bridge (Local\AnyMix.AsioInsert3) is shared: a running
  // AnyMix.exe publishes audio blocks on it continuously and overwrites the
  // sentinels before the pump can copy them. Bind a private mapping with the
  // same layout instead: ThreadMain/CopyToInputs are identical, only the
  // mapping name differs, so there is no interference from a live engine.
  if (!BindBridge(kSelfCheckMappingName) || bridge_ == nullptr) {
    return 1;
  }
  long inputs = 0;
  long outputs = 0;
  if (GetChannels(&inputs, &outputs) != kAseOk || inputs != anymix::kAsioChannels ||
      outputs != anymix::kAsioChannels) {
    return 2;
  }
  for (int frame = 0; frame < anymix::kBlockFrames; ++frame) {
    bridge_->send[0][frame * anymix::kAsioChannels] = 0.25f;
    bridge_->send[0][frame * anymix::kAsioChannels + 1] = -0.25f;
    bridge_->send[0][frame * anymix::kAsioChannels + 10] = 0.5f;
  }
  bridge_->sendSeq = 0;
  int switched = 0;
  AsioCallbacks callbacks{};
  callbacks.bufferSwitch = [](long, AsioBool) {};
  // The static callback above cannot touch `switched`. Count through asioMessage instead.
  callbacks.asioMessage = [](long, long, void* message, double*) -> long {
    if (message != nullptr) {
      *static_cast<int*>(message) += 1;
    }
    return 1;
  };
  AsioBufferInfo infos[28]{};
  for (int i = 0; i < anymix::kAsioChannels; ++i) {
    infos[i].isInput = 1;
    infos[i].channelNum = i;
    infos[anymix::kAsioChannels + i].isInput = 0;
    infos[anymix::kAsioChannels + i].channelNum = i;
  }
  if (CreateBuffers(infos, anymix::kAsioChannels * 2, anymix::kBlockFrames, &callbacks) != kAseOk) {
    return 3;
  }
  if (Start() != kAseOk) {
    return 4;
  }
  bridge_->sendSeq = 1;
  Sleep(80);
  Stop();
  const float* input = static_cast<float*>(infos[0].buffers[0]);
  const float* inputR = static_cast<float*>(infos[1].buffers[0]);
  const float* mic = static_cast<float*>(infos[10].buffers[0]);
  if (input == nullptr || inputR == nullptr || mic == nullptr || std::fabs(input[0] - 0.25f) > 0.001f ||
      std::fabs(inputR[0] + 0.25f) > 0.001f || std::fabs(mic[0] - 0.5f) > 0.001f) {
    return 5;
  }
  (void)switched;
  return 0;
}

extern "C" __declspec(dllexport) int AnyMix_CheckInsert() {
  InsertDriver driver;
  return driver.SelfCheck();
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
