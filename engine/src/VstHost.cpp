#include "anymix/VstHost.h"

#include "anymix/Types.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace anymix {
namespace {

constexpr int32_t kOk = 0;
constexpr int32_t kFalse = 1;
constexpr int32_t kNoInterface = static_cast<int32_t>(0x80004002);
constexpr int kBlock = kBlockFrames;
constexpr int kMaxCh = 8;

void MakeUid(char out[16], uint32_t l1, uint32_t l2, uint32_t l3, uint32_t l4) {
  const uint32_t part[4] = {l1, l2, l3, l4};
  int n = 0;
  out[n++] = static_cast<char>(part[0] & 0xFF);
  out[n++] = static_cast<char>((part[0] >> 8) & 0xFF);
  out[n++] = static_cast<char>((part[0] >> 16) & 0xFF);
  out[n++] = static_cast<char>((part[0] >> 24) & 0xFF);
  out[n++] = static_cast<char>((part[1] >> 16) & 0xFF);
  out[n++] = static_cast<char>((part[1] >> 24) & 0xFF);
  out[n++] = static_cast<char>(part[1] & 0xFF);
  out[n++] = static_cast<char>((part[1] >> 8) & 0xFF);
  out[n++] = static_cast<char>((part[2] >> 24) & 0xFF);
  out[n++] = static_cast<char>((part[2] >> 16) & 0xFF);
  out[n++] = static_cast<char>((part[2] >> 8) & 0xFF);
  out[n++] = static_cast<char>(part[2] & 0xFF);
  out[n++] = static_cast<char>((part[3] >> 24) & 0xFF);
  out[n++] = static_cast<char>((part[3] >> 16) & 0xFF);
  out[n++] = static_cast<char>((part[3] >> 8) & 0xFF);
  out[n++] = static_cast<char>(part[3] & 0xFF);
}

bool SameUid(const char* a, const char* b) {
  return a != nullptr && b != nullptr && std::memcmp(a, b, 16) == 0;
}

char kComponent[16];
char kProcessor[16];
char kEdit[16];
char kView[16];
char kHost[16];
char kUnknown[16];
char kStream[16];
bool g_uids = false;

void EnsureUids() {
  if (g_uids) {
    return;
  }
  MakeUid(kComponent, 0xE831FF31, 0xF2D54301, 0x928EBBEE, 0x25697802);
  MakeUid(kProcessor, 0x42043F99, 0xB7DA453C, 0xA569E79D, 0x9AAEC33D);
  MakeUid(kEdit, 0xDCD7BBE3, 0x7742448D, 0xA874AACC, 0x979C759E);
  MakeUid(kView, 0x5BC32507, 0xD06049EA, 0xA6151B52, 0x2B755B29);
  MakeUid(kHost, 0x58E595CC, 0xDB2D4960, 0x8B6AA72E, 0x1E13F6F4);
  MakeUid(kUnknown, 0x00000000, 0x00000000, 0xC0000000, 0x00000046);
  MakeUid(kStream, 0xC3BF6EA2, 0x30994752, 0x9B6BF990, 0x1EE33E9B);
  g_uids = true;
}

struct FUnknown {
  virtual int32_t __stdcall queryInterface(const char* iid, void** obj) = 0;
  virtual uint32_t __stdcall addRef() = 0;
  virtual uint32_t __stdcall release() = 0;
};

struct IPluginBase : FUnknown {
  virtual int32_t __stdcall initialize(FUnknown* context) = 0;
  virtual int32_t __stdcall terminate() = 0;
};

struct BusInfo {
  int32_t mediaType;
  int32_t direction;
  int32_t channelCount;
  char16_t name[128];
  int32_t busType;
  uint32_t flags;
};

struct RoutingInfo {
  int32_t mediaType;
  int32_t busIndex;
  int32_t channel;
};

struct IComponent : IPluginBase {
  virtual int32_t __stdcall getControllerClassId(char* classId) = 0;
  virtual int32_t __stdcall setIoMode(int32_t mode) = 0;
  virtual int32_t __stdcall getBusCount(int32_t type, int32_t dir) = 0;
  virtual int32_t __stdcall getBusInfo(int32_t type, int32_t dir, int32_t index, BusInfo& bus) = 0;
  virtual int32_t __stdcall getRoutingInfo(RoutingInfo& inInfo, RoutingInfo& outInfo) = 0;
  virtual int32_t __stdcall activateBus(int32_t type, int32_t dir, int32_t index, uint8_t state) = 0;
  virtual int32_t __stdcall setActive(uint8_t state) = 0;
  virtual int32_t __stdcall setState(void* stream) = 0;
  virtual int32_t __stdcall getState(void* stream) = 0;
};

struct ProcessSetup {
  int32_t processMode;
  int32_t symbolicSampleSize;
  int32_t maxSamplesPerBlock;
  double sampleRate;
};

struct AudioBusBuffers {
  int32_t numChannels;
  uint64_t silenceFlags;
  float** channelBuffers32;
};

static_assert(sizeof(ProcessSetup) == 24, "VST3 ProcessSetup layout");
static_assert(offsetof(ProcessSetup, sampleRate) == 16, "VST3 ProcessSetup rate");
static_assert(offsetof(AudioBusBuffers, silenceFlags) == 8, "VST3 bus silence");
static_assert(sizeof(AudioBusBuffers) == 24, "VST3 AudioBusBuffers layout");

struct ProcessData {
  int32_t processMode;
  int32_t symbolicSampleSize;
  int32_t numSamples;
  int32_t numInputs;
  int32_t numOutputs;
  AudioBusBuffers* inputs;
  AudioBusBuffers* outputs;
  void* inputParameterChanges;
  void* outputParameterChanges;
  void* inputEvents;
  void* outputEvents;
  void* processContext;
};

struct IAudioProcessor : FUnknown {
  virtual int32_t __stdcall setBusArrangements(uint64_t* inputs, int32_t numIns, uint64_t* outputs,
                                              int32_t numOuts) = 0;
  virtual int32_t __stdcall getBusArrangement(int32_t dir, int32_t index, uint64_t& arr) = 0;
  virtual int32_t __stdcall canProcessSampleSize(int32_t symbolicSampleSize) = 0;
  virtual uint32_t __stdcall getLatencySamples() = 0;
  virtual int32_t __stdcall setupProcessing(ProcessSetup& setup) = 0;
  virtual int32_t __stdcall setProcessing(uint8_t state) = 0;
  virtual int32_t __stdcall process(ProcessData& data) = 0;
  virtual uint32_t __stdcall getTailSamples() = 0;
};

struct ViewRect {
  int32_t left;
  int32_t top;
  int32_t right;
  int32_t bottom;
};

struct IPlugView : FUnknown {
  virtual int32_t __stdcall isPlatformTypeSupported(const char* type) = 0;
  virtual int32_t __stdcall attached(void* parent, const char* type) = 0;
  virtual int32_t __stdcall removed() = 0;
  virtual int32_t __stdcall onWheel(float distance) = 0;
  virtual int32_t __stdcall onKeyDown(char16_t key, int16_t keyCode, int16_t modifiers) = 0;
  virtual int32_t __stdcall onKeyUp(char16_t key, int16_t keyCode, int16_t modifiers) = 0;
  virtual int32_t __stdcall getSize(ViewRect* size) = 0;
  virtual int32_t __stdcall onSize(ViewRect* newSize) = 0;
  virtual int32_t __stdcall onFocus(uint8_t state) = 0;
  virtual int32_t __stdcall setFrame(void* frame) = 0;
  virtual int32_t __stdcall canResize() = 0;
  virtual int32_t __stdcall checkSizeConstraint(ViewRect* rect) = 0;
};

struct IConnectionPoint : FUnknown {
  virtual int32_t __stdcall connect(IConnectionPoint* other) = 0;
  virtual int32_t __stdcall disconnect(IConnectionPoint* other) = 0;
  virtual int32_t __stdcall notify(void* message) = 0;
};

struct IEditController : IPluginBase {
  virtual int32_t __stdcall setComponentState(void* stream) = 0;
  virtual int32_t __stdcall setState(void* stream) = 0;
  virtual int32_t __stdcall getState(void* stream) = 0;
  virtual int32_t __stdcall getParameterCount() = 0;
  virtual int32_t __stdcall getParameterInfo(int32_t index, void* info) = 0;
  virtual int32_t __stdcall getParamStringByValue(uint32_t id, double value, char16_t* text) = 0;
  virtual int32_t __stdcall getParamValueByString(uint32_t id, char16_t* text, double& value) = 0;
  virtual double __stdcall normalizedParamToPlain(uint32_t id, double value) = 0;
  virtual double __stdcall plainParamToNormalized(uint32_t id, double value) = 0;
  virtual double __stdcall getParamNormalized(uint32_t id) = 0;
  virtual int32_t __stdcall setParamNormalized(uint32_t id, double value) = 0;
  virtual int32_t __stdcall setComponentHandler(void* handler) = 0;
  virtual IPlugView* __stdcall createView(const char* name) = 0;
};

struct IBStream : FUnknown {
  virtual int32_t __stdcall read(void* buffer, int32_t numBytes, int32_t* numBytesRead) = 0;
  virtual int32_t __stdcall write(void* buffer, int32_t numBytes, int32_t* numBytesWritten) = 0;
  virtual int32_t __stdcall seek(int64_t pos, int32_t mode, int64_t* result) = 0;
  virtual int32_t __stdcall tell(int64_t* pos) = 0;
};

struct MemStream : IBStream {
  std::vector<uint8_t> bytes;
  int64_t pos = 0;
  uint32_t refs = 1;

  int32_t __stdcall queryInterface(const char* iid, void** obj) override {
    if (obj == nullptr) {
      return kNoInterface;
    }
    *obj = nullptr;
    EnsureUids();
    if (SameUid(iid, kStream) || SameUid(iid, kUnknown)) {
      *obj = static_cast<IBStream*>(this);
      addRef();
      return kOk;
    }
    return kNoInterface;
  }
  uint32_t __stdcall addRef() override { return ++refs; }
  uint32_t __stdcall release() override { return refs > 0 ? --refs : 0; }
  int32_t __stdcall read(void* buffer, int32_t numBytes, int32_t* numBytesRead) override {
    if (buffer == nullptr || numBytes < 0) {
      return kFalse;
    }
    const int64_t left = static_cast<int64_t>(bytes.size()) - pos;
    int32_t count = numBytes;
    if (left <= 0) {
      count = 0;
    } else if (static_cast<int64_t>(count) > left) {
      count = static_cast<int32_t>(left);
    }
    if (count > 0) {
      std::memcpy(buffer, bytes.data() + pos, static_cast<size_t>(count));
      pos += count;
    }
    if (numBytesRead != nullptr) {
      *numBytesRead = count;
    }
    return count == numBytes ? kOk : kFalse;
  }
  int32_t __stdcall write(void* buffer, int32_t numBytes, int32_t* numBytesWritten) override {
    if (buffer == nullptr || numBytes < 0 || pos < 0) {
      return kFalse;
    }
    const size_t end = static_cast<size_t>(pos) + static_cast<size_t>(numBytes);
    if (end > bytes.size()) {
      bytes.resize(end);
    }
    if (numBytes > 0) {
      std::memcpy(bytes.data() + pos, buffer, static_cast<size_t>(numBytes));
      pos += numBytes;
    }
    if (numBytesWritten != nullptr) {
      *numBytesWritten = numBytes;
    }
    return kOk;
  }
  int32_t __stdcall seek(int64_t where, int32_t mode, int64_t* result) override {
    int64_t next = pos;
    if (mode == 0) {
      next = where;
    } else if (mode == 1) {
      next = pos + where;
    } else {
      next = static_cast<int64_t>(bytes.size()) + where;
    }
    if (next < 0) {
      next = 0;
    }
    pos = next;
    if (result != nullptr) {
      *result = pos;
    }
    return kOk;
  }
  int32_t __stdcall tell(int64_t* where) override {
    if (where != nullptr) {
      *where = pos;
    }
    return kOk;
  }
  void Rewind() { pos = 0; }
};

struct PClassInfo {
  char cid[16];
  int32_t cardinality;
  char category[32];
  char name[64];
};

struct IPluginFactory : FUnknown {
  virtual int32_t __stdcall getFactoryInfo(void* info) = 0;
  virtual int32_t __stdcall countClasses() = 0;
  virtual int32_t __stdcall getClassInfo(int32_t index, PClassInfo* info) = 0;
  virtual int32_t __stdcall createInstance(const char* cid, const char* iid, void** obj) = 0;
};

struct IPluginFactory3 : IPluginFactory {
  virtual int32_t __stdcall getClassInfo2(int32_t index, void* info) = 0;
  virtual int32_t __stdcall getClassInfoUnicode(int32_t index, void* info) = 0;
  virtual int32_t __stdcall setHostContext(FUnknown* context) = 0;
};

struct HostApp : FUnknown {
  int32_t __stdcall queryInterface(const char* iid, void** obj) override {
    if (obj == nullptr) {
      return kNoInterface;
    }
    *obj = nullptr;
    EnsureUids();
    if (SameUid(iid, kHost) || SameUid(iid, kUnknown)) {
      *obj = static_cast<FUnknown*>(this);
      addRef();
      return kOk;
    }
    return kNoInterface;
  }
  uint32_t __stdcall addRef() override { return 1; }
  uint32_t __stdcall release() override { return 1; }
  virtual int32_t __stdcall getName(char16_t* name) {
    if (name == nullptr) {
      return kFalse;
    }
    const char16_t text[] = u"AnyMix";
    for (int i = 0; i < 7; ++i) {
      name[i] = text[i];
    }
    return kOk;
  }
  virtual int32_t __stdcall createInstance(const char*, const char*, void** obj) {
    if (obj != nullptr) {
      *obj = nullptr;
    }
    return kNoInterface;
  }
};

HostApp g_host;

struct AEffect;

using VstDispatch = intptr_t(__cdecl*)(AEffect*, int32_t, int32_t, intptr_t, void*, float);
using VstProcess = void(__cdecl*)(AEffect*, float**, float**, int32_t);
using VstSetParam = void(__cdecl*)(AEffect*, int32_t, float);
using VstGetParam = float(__cdecl*)(AEffect*, int32_t);
using AudioMaster = intptr_t(__cdecl*)(AEffect*, int32_t, int32_t, intptr_t, void*, float);

struct AEffect {
  int32_t magic;
  VstDispatch dispatcher;
  VstProcess process;
  VstSetParam setParameter;
  VstGetParam getParameter;
  int32_t numPrograms;
  int32_t numParams;
  int32_t numInputs;
  int32_t numOutputs;
  int32_t flags;
  intptr_t resvd1;
  intptr_t resvd2;
  int32_t initialDelay;
  int32_t realQualities;
  int32_t offQualities;
  float ioRatio;
  void* object;
  void* user;
  int32_t uniqueID;
  int32_t version;
  VstProcess processReplacing;
  VstProcess processDoubleReplacing;
  char future[56];
};

struct ERect {
  int16_t top;
  int16_t left;
  int16_t bottom;
  int16_t right;
};

thread_local bool g_inProcess = false;

intptr_t __cdecl MasterCallback(AEffect*, int32_t opcode, int32_t, intptr_t, void* ptr, float) {
  if (opcode == 1) {
    return 2400;
  }
  if (opcode == 15) {
    return 1;
  }
  if (opcode == 16) {
    return kSampleRate;
  }
  if (opcode == 17) {
    return kBlock;
  }
  if (opcode == 23) {
    return g_inProcess ? 2 : 1;
  }
  if (opcode == 32 || opcode == 33) {
    if (ptr != nullptr) {
      std::strncpy(static_cast<char*>(ptr), "AnyMix", 63);
    }
    return 1;
  }
  if (opcode == 34) {
    return 1000;
  }
  if (opcode == 37 && ptr != nullptr) {
    const char* can = static_cast<const char*>(ptr);
    if (std::strcmp(can, "sizeWindow") == 0 || std::strcmp(can, "supplyIdle") == 0) {
      return 1;
    }
    return 0;
  }
  return 0;
}

std::wstring ResolveBinary(const wchar_t* path) {
  if (path == nullptr || path[0] == 0) {
    return {};
  }
  const DWORD attr = GetFileAttributesW(path);
  if (attr == INVALID_FILE_ATTRIBUTES) {
    return {};
  }
  if ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    return path;
  }
  const std::wstring pattern = std::wstring(path) + L"\\Contents\\x86_64-win\\*.vst3";
  WIN32_FIND_DATAW data{};
  const HANDLE found = FindFirstFileW(pattern.c_str(), &data);
  if (found == INVALID_HANDLE_VALUE) {
    return {};
  }
  std::wstring binary = std::wstring(path) + L"\\Contents\\x86_64-win\\" + data.cFileName;
  FindClose(found);
  return binary;
}

bool EndsWith(const wchar_t* path, const wchar_t* ext) {
  const size_t n = wcslen(path);
  const size_t m = wcslen(ext);
  return n >= m && _wcsicmp(path + n - m, ext) == 0;
}

int BusChannels(IAudioProcessor* processor, int direction, int buses) {
  if (processor == nullptr || buses <= 0) {
    return direction == 1 ? 2 : 0;
  }
  uint64_t arrangement = 0;
  if (processor->getBusArrangement(direction, 0, arrangement) != kOk || arrangement == 0) {
    return 2;
  }
  int count = 0;
  while (arrangement != 0) {
    count += static_cast<int>(arrangement & 1ull);
    arrangement >>= 1;
  }
  return count < 1 ? 1 : count;
}

#pragma pack(push, 1)
struct VstEditHead {
  uint32_t magic = 0;
  uint32_t version = 0;
  uint32_t kind = 0;
  int32_t program = 0;
  uint32_t compSize = 0;
  uint32_t ctrlSize = 0;
  wchar_t path[520]{};
};
#pragma pack(pop)

constexpr uint32_t kVstEditMagic = 0x42455356u;

}  // namespace

class VstPlugin {
 public:
  std::atomic<int> inUse{0};
  std::atomic<bool> alive{false};
  wchar_t name[128]{};
  wchar_t path[520]{};
  HWND editor = nullptr;
  bool editorOpen = false;

  ~VstPlugin() { Shutdown(); }

  void OnEditorSize(int width, int height) {
    if (view_ == nullptr || width < 1 || height < 1) {
      return;
    }
    ViewRect rect{0, 0, width, height};
    view_->onSize(&rect);
  }

  bool Load(const wchar_t* file, std::wstring& error) {
    EnsureUids();
    const std::wstring binary = ResolveBinary(file);
    if (binary.empty()) {
      error = L"Plug-in file was not found.";
      return false;
    }
    wcsncpy(path, file, 519);
    path[519] = 0;
    module_ = LoadLibraryW(binary.c_str());
    if (module_ == nullptr) {
      error = L"Windows could not load the plug-in.";
      return false;
    }
    const bool vst3 = EndsWith(binary.c_str(), L".vst3") || EndsWith(file, L".vst3");
    if (vst3) {
      using ModuleFn = bool(__stdcall*)();
      auto init = reinterpret_cast<ModuleFn>(GetProcAddress(module_, "InitDll"));
      if (init != nullptr) {
        if (!init()) {
          error = L"The plug-in module did not initialize.";
          FreeLibrary(module_);
          module_ = nullptr;
          return false;
        }
        moduleInit_ = true;
      }
    }
    if (vst3) {
      if (!LoadVst3(error)) {
        FreeLibrary(module_);
        module_ = nullptr;
        return false;
      }
    } else if (!LoadVst2(error)) {
      FreeLibrary(module_);
      module_ = nullptr;
      return false;
    }
    alive.store(true, std::memory_order_release);
    return true;
  }

  void Shutdown() {
    alive.store(false, std::memory_order_release);
    CloseEditor();
    if (processor_ != nullptr) {
      processor_->setProcessing(0);
    }
    if (component_ != nullptr) {
      component_->setActive(0);
    }
    if (otherPoint_ != nullptr && point_ != nullptr) {
      point_->disconnect(otherPoint_);
      otherPoint_->disconnect(point_);
    }
    if (view_ != nullptr) {
      view_->release();
      view_ = nullptr;
    }
    if (controller_ != nullptr && controller_ != static_cast<IPluginBase*>(component_)) {
      controller_->terminate();
    }
    if (controller_ != nullptr) {
      controller_->release();
      controller_ = nullptr;
    }
    if (point_ != nullptr) {
      point_->release();
      point_ = nullptr;
    }
    if (otherPoint_ != nullptr) {
      otherPoint_->release();
      otherPoint_ = nullptr;
    }
    if (processor_ != nullptr) {
      processor_->release();
      processor_ = nullptr;
    }
    if (component_ != nullptr) {
      component_->terminate();
      component_->release();
      component_ = nullptr;
    }
    if (effect_ != nullptr && effect_->dispatcher != nullptr) {
      effect_->dispatcher(effect_, 72, 0, 0, nullptr, 0.f);
      effect_->dispatcher(effect_, 12, 0, 0, nullptr, 0.f);
      effect_->dispatcher(effect_, 1, 0, 0, nullptr, 0.f);
      effect_ = nullptr;
    }
    if (module_ != nullptr) {
      if (moduleInit_) {
        using ModuleFn = bool(__stdcall*)();
        auto exitDll = reinterpret_cast<ModuleFn>(GetProcAddress(module_, "ExitDll"));
        if (exitDll != nullptr) {
          exitDll();
        }
        moduleInit_ = false;
      }
      FreeLibrary(module_);
      module_ = nullptr;
    }
    for (int i = 0; i < kMaxCh; ++i) {
      delete[] inSamples_[i];
      delete[] outSamples_[i];
      delete[] silence_[i];
      inSamples_[i] = nullptr;
      outSamples_[i] = nullptr;
      silence_[i] = nullptr;
      inPtr_[i] = nullptr;
      outPtr_[i] = nullptr;
      silencePtr_[i] = nullptr;
    }
  }

  void Process(float* interleaved, int frames) {
    if (frames <= 0 || interleaved == nullptr) {
      return;
    }
    int done = 0;
    while (done < frames) {
      const int count = std::min(kBlock, frames - done);
      float* at = interleaved + done * 2;
      float dry[kBlock * 2];
      std::memcpy(dry, at, sizeof(float) * static_cast<size_t>(count) * 2);
      try {
        if (processor_ != nullptr) {
          ProcessVst3(at, count);
        } else if (effect_ != nullptr && effect_->processReplacing != nullptr) {
          ProcessVst2(at, count);
        }
      } catch (...) {
        std::memcpy(at, dry, sizeof(float) * static_cast<size_t>(count) * 2);
      }
      done += count;
    }
  }

  void ToggleEditor() {
    if (editorOpen) {
      CloseEditor();
      return;
    }
    if (processor_ == nullptr && effect_ == nullptr) {
      return;
    }
    if (view_ == nullptr && controller_ != nullptr) {
      view_ = controller_->createView("editor");
    }
    int width = 480;
    int height = 320;
    if (view_ != nullptr) {
      ViewRect rect{};
      if (view_->getSize(&rect) == kOk && rect.right > rect.left && rect.bottom > rect.top) {
        width = rect.right - rect.left;
        height = rect.bottom - rect.top;
      }
    } else if (effect_ != nullptr && effect_->dispatcher != nullptr) {
      ERect* rect = nullptr;
      effect_->dispatcher(effect_, 13, 0, 0, &rect, 0.f);
      if (rect != nullptr && rect->right > rect->left && rect->bottom > rect->top) {
        width = rect->right - rect->left;
        height = rect->bottom - rect->top;
      }
    } else {
      return;
    }
    EnsureEditorClass();
    HWND owner = FindWindowW(L"AnyMixWindow", nullptr);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
    RECT bounds{0, 0, width, height};
    AdjustWindowRect(&bounds, style, FALSE);
    editor = CreateWindowExW(0, L"AnyMixVstEditor", name, style, CW_USEDEFAULT, CW_USEDEFAULT,
                             bounds.right - bounds.left, bounds.bottom - bounds.top, owner, nullptr,
                             GetModuleHandleW(nullptr), this);
    if (editor == nullptr) {
      return;
    }
    editorOpen = true;
    ShowWindow(editor, SW_SHOW);
    if (view_ != nullptr) {
      view_->attached(editor, "HWND");
      ViewRect rect{0, 0, width, height};
      view_->onSize(&rect);
    } else if (effect_ != nullptr && effect_->dispatcher != nullptr) {
      effect_->dispatcher(effect_, 14, 0, 0, editor, 0.f);
    }
  }

  void CloseEditor() {
    if (view_ != nullptr && editorOpen) {
      view_->removed();
    } else if (effect_ != nullptr && effect_->dispatcher != nullptr && editorOpen) {
      effect_->dispatcher(effect_, 15, 0, 0, nullptr, 0.f);
    }
    editorOpen = false;
    if (editor != nullptr) {
      HWND window = editor;
      editor = nullptr;
      DestroyWindow(window);
    }
  }

  static LRESULT CALLBACK EditorProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* plugin = reinterpret_cast<VstPlugin*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      plugin = static_cast<VstPlugin*>(create->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(plugin));
    }
    if (message == WM_CLOSE && plugin != nullptr) {
      plugin->CloseEditor();
      HWND owner = FindWindowW(L"AnyMixWindow", nullptr);
      if (owner != nullptr) {
        PostMessageW(owner, WM_APP + 41, 0, 0);
      }
      return 0;
    }
    if (message == WM_SIZE && plugin != nullptr) {
      plugin->OnEditorSize(static_cast<int>(LOWORD(lparam)), static_cast<int>(HIWORD(lparam)));
    }
    return DefWindowProcW(window, message, wparam, lparam);
  }

  void PauseAudio() {
    alive.store(false, std::memory_order_release);
    while (inUse.load(std::memory_order_acquire) != 0) {
      Sleep(1);
    }
  }

  bool Capture(std::vector<uint8_t>& out) {
    out.clear();
    if (!alive.load(std::memory_order_acquire) && component_ == nullptr && effect_ == nullptr) {
      return false;
    }
    PauseAudio();
    const bool ok = WriteState(out);
    alive.store(true, std::memory_order_release);
    return ok;
  }

  void Restore(const uint8_t* data, size_t size) {
    if (data == nullptr || size < sizeof(VstEditHead)) {
      return;
    }
    VstEditHead head{};
    std::memcpy(&head, data, sizeof(head));
    if (head.magic != kVstEditMagic || head.version != 1 || _wcsicmp(head.path, path) != 0) {
      return;
    }
    const size_t body = static_cast<size_t>(head.compSize) + static_cast<size_t>(head.ctrlSize);
    if (sizeof(VstEditHead) + body != size || body > (1u << 20)) {
      return;
    }
    const uint8_t* bytes = data + sizeof(VstEditHead);
    PauseAudio();
    ReadState(head, bytes);
    alive.store(true, std::memory_order_release);
  }

  bool WriteState(std::vector<uint8_t>& out) {
    VstEditHead head{};
    head.magic = kVstEditMagic;
    head.version = 1;
    wcsncpy(head.path, path, 519);
    std::vector<uint8_t> comp;
    std::vector<uint8_t> ctrl;
    if (component_ != nullptr) {
      MemStream stream;
      if (component_->getState(&stream) == kOk && !stream.bytes.empty()) {
        comp.swap(stream.bytes);
      }
      if (controller_ != nullptr) {
        MemStream edit;
        if (controller_->getState(&edit) == kOk && !edit.bytes.empty()) {
          ctrl.swap(edit.bytes);
        }
      }
      head.kind = 3;
    } else if (effect_ != nullptr && effect_->dispatcher != nullptr) {
      head.program = static_cast<int32_t>(effect_->dispatcher(effect_, 3, 0, 0, nullptr, 0.f));
      void* chunk = nullptr;
      const intptr_t bytes = effect_->dispatcher(effect_, 23, 0, 0, &chunk, 0.f);
      if ((effect_->flags & 32) != 0 && bytes > 0 && chunk != nullptr &&
          static_cast<size_t>(bytes) <= (1u << 20)) {
        head.kind = 1;
        comp.assign(static_cast<uint8_t*>(chunk), static_cast<uint8_t*>(chunk) + bytes);
      } else if (effect_->getParameter != nullptr && effect_->numParams > 0) {
        const int count = std::min(effect_->numParams, 4096);
        head.kind = 2;
        comp.resize(static_cast<size_t>(count) * sizeof(float));
        for (int i = 0; i < count; ++i) {
          const float value = effect_->getParameter(effect_, i);
          std::memcpy(comp.data() + static_cast<size_t>(i) * sizeof(float), &value, sizeof(float));
        }
      }
    }
    if (head.kind == 0 || comp.size() + ctrl.size() > (1u << 20)) {
      return false;
    }
    head.compSize = static_cast<uint32_t>(comp.size());
    head.ctrlSize = static_cast<uint32_t>(ctrl.size());
    out.resize(sizeof(head) + comp.size() + ctrl.size());
    std::memcpy(out.data(), &head, sizeof(head));
    if (!comp.empty()) {
      std::memcpy(out.data() + sizeof(head), comp.data(), comp.size());
    }
    if (!ctrl.empty()) {
      std::memcpy(out.data() + sizeof(head) + comp.size(), ctrl.data(), ctrl.size());
    }
    return true;
  }

  void ReadState(const VstEditHead& head, const uint8_t* bytes) {
    if (head.kind == 3 && component_ != nullptr) {
      if (processor_ != nullptr) {
        processor_->setProcessing(0);
      }
      component_->setActive(0);
      if (head.compSize > 0) {
        MemStream stream;
        stream.bytes.assign(bytes, bytes + head.compSize);
        component_->setState(&stream);
        stream.Rewind();
        if (controller_ != nullptr) {
          controller_->setComponentState(&stream);
        }
      }
      if (controller_ != nullptr && head.ctrlSize > 0) {
        MemStream edit;
        edit.bytes.assign(bytes + head.compSize, bytes + head.compSize + head.ctrlSize);
        controller_->setState(&edit);
      }
      component_->setActive(1);
      if (processor_ != nullptr) {
        processor_->setProcessing(1);
      }
      return;
    }
    if (effect_ == nullptr || effect_->dispatcher == nullptr) {
      return;
    }
    effect_->dispatcher(effect_, 2, 0, head.program, nullptr, 0.f);
    if (head.kind == 1 && head.compSize > 0) {
      effect_->dispatcher(effect_, 24, 0, static_cast<intptr_t>(head.compSize),
                          const_cast<uint8_t*>(bytes), 0.f);
      return;
    }
    if (head.kind == 2 && effect_->setParameter != nullptr) {
      const int count = static_cast<int>(head.compSize / sizeof(float));
      for (int i = 0; i < count; ++i) {
        float value = 0.f;
        std::memcpy(&value, bytes + static_cast<size_t>(i) * sizeof(float), sizeof(float));
        effect_->setParameter(effect_, i, value);
      }
    }
  }

 private:
  static void EnsureEditorClass() {
    static bool ready = false;
    if (ready) {
      return;
    }
    WNDCLASSW klass{};
    klass.lpfnWndProc = EditorProc;
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpszClassName = L"AnyMixVstEditor";
    klass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&klass);
    ready = true;
  }

  bool LoadVst3(std::wstring& error) {
    using GetFactory = IPluginFactory*(__stdcall*)();
    auto getFactory = reinterpret_cast<GetFactory>(GetProcAddress(module_, "GetPluginFactory"));
    if (getFactory == nullptr) {
      error = L"This file is not a VST3 plug-in.";
      return false;
    }
    IPluginFactory* factory = getFactory();
    if (factory == nullptr) {
      error = L"The plug-in factory did not start.";
      return false;
    }
    char factory3[16];
    MakeUid(factory3, 0x4555A2AB, 0xC1234E57, 0x9B122910, 0x36878931);
    void* factory3Raw = nullptr;
    if (factory->queryInterface(factory3, &factory3Raw) == kOk && factory3Raw != nullptr) {
      auto* newer = static_cast<IPluginFactory3*>(factory3Raw);
      newer->setHostContext(&g_host);
      newer->release();
    }
    char cid[16]{};
    bool found = false;
    const int32_t count = factory->countClasses();
    for (int32_t i = 0; i < count; ++i) {
      PClassInfo info{};
      if (factory->getClassInfo(i, &info) != kOk) {
        continue;
      }
      if (std::strcmp(info.category, "Audio Module Class") == 0) {
        std::memcpy(cid, info.cid, 16);
        char narrow[65]{};
        std::memcpy(narrow, info.name, 64);
        MultiByteToWideChar(CP_UTF8, 0, narrow, -1, name, 128);
        found = true;
        break;
      }
    }
    if (!found) {
      factory->release();
      error = L"No audio effect was found in this plug-in.";
      return false;
    }
    void* raw = nullptr;
    if (factory->createInstance(cid, kComponent, &raw) != kOk || raw == nullptr) {
      factory->release();
      error = L"The plug-in did not create an effect.";
      return false;
    }
    component_ = static_cast<IComponent*>(raw);
    if (component_->initialize(&g_host) != kOk) {
      component_->release();
      component_ = nullptr;
      factory->release();
      error = L"The plug-in refused to initialize.";
      return false;
    }
    void* proc = nullptr;
    if (component_->queryInterface(kProcessor, &proc) != kOk || proc == nullptr) {
      component_->terminate();
      component_->release();
      component_ = nullptr;
      factory->release();
      error = L"The plug-in has no audio processor.";
      return false;
    }
    processor_ = static_cast<IAudioProcessor*>(proc);
    const int32_t inputs = component_->getBusCount(0, 0);
    const int32_t outputs = component_->getBusCount(0, 1);
    inBuses_ = inputs < 0 ? 0 : (inputs > 8 ? 8 : inputs);
    outBuses_ = outputs < 0 ? 0 : (outputs > 8 ? 8 : outputs);
    uint64_t inArr[8];
    uint64_t outArr[8];
    for (int i = 0; i < 8; ++i) {
      inArr[i] = 3;
      outArr[i] = 3;
    }
    if (inBuses_ > 0 || outBuses_ > 0) {
      processor_->setBusArrangements(inBuses_ > 0 ? inArr : nullptr, inBuses_, outBuses_ > 0 ? outArr : nullptr,
                                    outBuses_);
    }
    for (int i = 0; i < inBuses_; ++i) {
      BusInfo info{};
      const bool main = i == 0 || (component_->getBusInfo(0, 0, i, info) == kOk && (info.flags & 1u) != 0);
      component_->activateBus(0, 0, i, main ? 1 : 0);
    }
    for (int i = 0; i < outBuses_; ++i) {
      BusInfo info{};
      const bool main = i == 0 || (component_->getBusInfo(0, 1, i, info) == kOk && (info.flags & 1u) != 0);
      component_->activateBus(0, 1, i, main ? 1 : 0);
    }
    inCh_ = BusChannels(processor_, 0, inBuses_);
    outCh_ = BusChannels(processor_, 1, outBuses_);
    if (inCh_ > kMaxCh || outCh_ > kMaxCh) {
      processor_->release();
      processor_ = nullptr;
      component_->terminate();
      component_->release();
      component_ = nullptr;
      factory->release();
      error = L"This plug-in needs more than 8 channels.";
      return false;
    }
    ProcessSetup setup{};
    setup.processMode = 0;
    setup.symbolicSampleSize = 0;
    setup.maxSamplesPerBlock = kBlock;
    setup.sampleRate = static_cast<double>(kSampleRate);
    processor_->setupProcessing(setup);
    component_->setActive(1);
    processor_->setProcessing(1);
    void* edit = nullptr;
    if (component_->queryInterface(kEdit, &edit) == kOk && edit != nullptr) {
      controller_ = static_cast<IEditController*>(edit);
    } else {
      char editId[16]{};
      if (component_->getControllerClassId(editId) == kOk) {
        void* created = nullptr;
        if (factory->createInstance(editId, kEdit, &created) == kOk && created != nullptr) {
          controller_ = static_cast<IEditController*>(created);
          controller_->initialize(&g_host);
        }
      }
    }
    if (controller_ != nullptr) {
      void* mine = nullptr;
      void* theirs = nullptr;
      if (component_->queryInterface(MakeConnection(), &mine) == kOk) {
        point_ = static_cast<IConnectionPoint*>(mine);
      }
      if (controller_->queryInterface(MakeConnection(), &theirs) == kOk) {
        otherPoint_ = static_cast<IConnectionPoint*>(theirs);
      }
      if (point_ != nullptr && otherPoint_ != nullptr && point_ != otherPoint_) {
        point_->connect(otherPoint_);
        otherPoint_->connect(point_);
      }
    }
    factory->release();
    if (name[0] == 0) {
      wcsncpy(name, L"VST3", 127);
    }
    Allocate();
    return true;
  }

  static const char* MakeConnection() {
    static char iid[16];
    static bool ready = false;
    if (!ready) {
      MakeUid(iid, 0x70A4156F, 0x6E6E4026, 0x989148BF, 0xAA60D8D1);
      ready = true;
    }
    return iid;
  }

  bool LoadVst2(std::wstring& error) {
    using Entry = AEffect*(__cdecl*)(AudioMaster);
    auto entry = reinterpret_cast<Entry>(GetProcAddress(module_, "VSTPluginMain"));
    if (entry == nullptr) {
      entry = reinterpret_cast<Entry>(GetProcAddress(module_, "main"));
    }
    if (entry == nullptr) {
      error = L"This file is not a VST plug-in.";
      return false;
    }
    effect_ = entry(MasterCallback);
    if (effect_ == nullptr || effect_->magic != 0x56737450 || effect_->dispatcher == nullptr) {
      effect_ = nullptr;
      error = L"The VST plug-in did not open.";
      return false;
    }
    effect_->user = this;
    effect_->dispatcher(effect_, 0, 0, 0, nullptr, 0.f);
    effect_->dispatcher(effect_, 10, 0, 0, nullptr, static_cast<float>(kSampleRate));
    effect_->dispatcher(effect_, 11, 0, kBlock, nullptr, 0.f);
    effect_->dispatcher(effect_, 12, 0, 1, nullptr, 0.f);
    effect_->dispatcher(effect_, 71, 0, 0, nullptr, 0.f);
    char text[64]{};
    effect_->dispatcher(effect_, 45, 0, 0, text, 0.f);
    if (text[0] == 0) {
      effect_->dispatcher(effect_, 48, 0, 0, text, 0.f);
    }
    if (text[0] != 0) {
      MultiByteToWideChar(CP_UTF8, 0, text, -1, name, 128);
    } else {
      wcsncpy(name, L"VST", 127);
    }
    if (effect_->numInputs > kMaxCh || effect_->numOutputs > kMaxCh) {
      effect_->dispatcher(effect_, 1, 0, 0, nullptr, 0.f);
      effect_ = nullptr;
      error = L"This plug-in needs more than 8 channels.";
      return false;
    }
    inCh_ = effect_->numInputs > 0 ? effect_->numInputs : 0;
    outCh_ = effect_->numOutputs > 0 ? effect_->numOutputs : 2;
    Allocate();
    return true;
  }

  void Allocate() {
    for (int i = 0; i < kMaxCh; ++i) {
      inSamples_[i] = new float[kBlock]();
      outSamples_[i] = new float[kBlock]();
      silence_[i] = new float[kBlock]();
      inPtr_[i] = inSamples_[i];
      outPtr_[i] = outSamples_[i];
      silencePtr_[i] = silence_[i];
    }
  }

  void ProcessVst3(float* interleaved, int frames) {
    for (int ch = 0; ch < kMaxCh; ++ch) {
      std::memset(inSamples_[ch], 0, sizeof(float) * static_cast<size_t>(frames));
      std::memset(outSamples_[ch], 0, sizeof(float) * static_cast<size_t>(frames));
    }
    for (int i = 0; i < frames; ++i) {
      if (inCh_ > 0) {
        inSamples_[0][i] = interleaved[i * 2];
      }
      if (inCh_ > 1) {
        inSamples_[1][i] = interleaved[i * 2 + 1];
      }
    }
    AudioBusBuffers inputs[8]{};
    AudioBusBuffers outputs[8]{};
    for (int i = 0; i < inBuses_; ++i) {
      inputs[i].numChannels = i == 0 ? inCh_ : 2;
      inputs[i].silenceFlags = i == 0 ? 0 : ~0ull;
      inputs[i].channelBuffers32 = i == 0 ? inPtr_ : silencePtr_;
    }
    for (int i = 0; i < outBuses_; ++i) {
      outputs[i].numChannels = i == 0 ? outCh_ : 2;
      outputs[i].channelBuffers32 = i == 0 ? outPtr_ : silencePtr_;
    }
    ProcessData data{};
    data.processMode = 0;
    data.symbolicSampleSize = 0;
    data.numSamples = frames;
    data.numInputs = inBuses_;
    data.numOutputs = outBuses_;
    data.inputs = inBuses_ > 0 ? inputs : nullptr;
    data.outputs = outBuses_ > 0 ? outputs : nullptr;
    g_inProcess = true;
    const int32_t result = processor_->process(data);
    g_inProcess = false;
    if (result < 0) {
      return;
    }
    for (int i = 0; i < frames; ++i) {
      interleaved[i * 2] = outCh_ > 0 ? outSamples_[0][i] : interleaved[i * 2];
      interleaved[i * 2 + 1] = outCh_ > 1 ? outSamples_[1][i] : outSamples_[0][i];
    }
  }

  void ProcessVst2(float* interleaved, int frames) {
    for (int ch = 0; ch < kMaxCh; ++ch) {
      std::memset(inSamples_[ch], 0, sizeof(float) * static_cast<size_t>(frames));
      std::memset(outSamples_[ch], 0, sizeof(float) * static_cast<size_t>(frames));
    }
    for (int i = 0; i < frames; ++i) {
      if (inCh_ > 0) {
        inSamples_[0][i] = interleaved[i * 2];
      }
      if (inCh_ > 1) {
        inSamples_[1][i] = interleaved[i * 2 + 1];
      }
    }
    g_inProcess = true;
    effect_->processReplacing(effect_, inCh_ > 0 ? inPtr_ : nullptr, outPtr_, frames);
    g_inProcess = false;
    for (int i = 0; i < frames; ++i) {
      interleaved[i * 2] = outSamples_[0][i];
      interleaved[i * 2 + 1] = outCh_ > 1 ? outSamples_[1][i] : outSamples_[0][i];
    }
  }

  HMODULE module_ = nullptr;
  bool moduleInit_ = false;
  IComponent* component_ = nullptr;
  IAudioProcessor* processor_ = nullptr;
  IEditController* controller_ = nullptr;
  IPlugView* view_ = nullptr;
  IConnectionPoint* point_ = nullptr;
  IConnectionPoint* otherPoint_ = nullptr;
  AEffect* effect_ = nullptr;
  int inBuses_ = 0;
  int outBuses_ = 0;
  int inCh_ = 2;
  int outCh_ = 2;
  float* inSamples_[kMaxCh]{};
  float* outSamples_[kMaxCh]{};
  float* silence_[kMaxCh]{};
  float* inPtr_[kMaxCh]{};
  float* outPtr_[kMaxCh]{};
  float* silencePtr_[kMaxCh]{};
};

VstRack::VstRack() {
  for (int strip = 0; strip < kStrips; ++strip) {
    localOn_[strip].store(true, std::memory_order_relaxed);
  }
}

VstRack::~VstRack() {
  for (int i = 0; i < kVstSlots; ++i) {
    Remove(i);
  }
}

int VstRack::Add(int strip, const wchar_t* path, std::wstring& error) {
  if (path == nullptr || path[0] == 0) {
    error = L"Choose a plug-in.";
    return -1;
  }
  int slot = -1;
  for (int i = 0; i < kVstSlots; ++i) {
    if (slots_[i].plugin.load(std::memory_order_acquire) == nullptr) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    error = L"The rack is full (8 plug-ins).";
    return -1;
  }
  auto* plugin = new VstPlugin();
  if (!plugin->Load(path, error)) {
    delete plugin;
    return -1;
  }
  slots_[slot].strip.store(strip, std::memory_order_relaxed);
  slots_[slot].group.store(-1, std::memory_order_relaxed);
  slots_[slot].bypass.store(false, std::memory_order_relaxed);
  slots_[slot].plugin.store(plugin, std::memory_order_release);
  return slot;
}

void VstRack::Remove(int slot) {
  if (slot < 0 || slot >= kVstSlots) {
    return;
  }
  slots_[slot].group.store(-1, std::memory_order_relaxed);
  VstPlugin* plugin = slots_[slot].plugin.exchange(nullptr, std::memory_order_acq_rel);
  if (plugin == nullptr) {
    return;
  }
  plugin->alive.store(false, std::memory_order_release);
  while (plugin->inUse.load(std::memory_order_acquire) != 0) {
    Sleep(1);
  }
  delete plugin;
}

void VstRack::SetBypass(int slot, bool on) {
  if (slot < 0 || slot >= kVstSlots) {
    return;
  }
  slots_[slot].bypass.store(on, std::memory_order_relaxed);
}

void VstRack::SetGroup(int slot, int group) {
  if (slot < 0 || slot >= kVstSlots || group < -1 || group >= kVstGroups) {
    return;
  }
  slots_[slot].group.store(group, std::memory_order_relaxed);
}

void VstRack::SetGroupBypass(int group, bool on) {
  if (group < 0 || group >= kVstGroups) {
    return;
  }
  groupBypass_[group].store(on, std::memory_order_relaxed);
}

void VstRack::SetLocal(int strip, bool on) {
  if (strip < 0 || strip >= kStrips) {
    return;
  }
  localOn_[strip].store(on, std::memory_order_relaxed);
}

void VstRack::ToggleEditor(int slot) {
  if (slot < 0 || slot >= kVstSlots) {
    return;
  }
  VstPlugin* plugin = slots_[slot].plugin.load(std::memory_order_acquire);
  if (plugin != nullptr) {
    plugin->ToggleEditor();
  }
}

bool VstRack::CaptureState(int slot, std::vector<uint8_t>& out) {
  out.clear();
  if (slot < 0 || slot >= kVstSlots) {
    return false;
  }
  VstPlugin* plugin = slots_[slot].plugin.load(std::memory_order_acquire);
  if (plugin == nullptr) {
    return false;
  }
  return plugin->Capture(out);
}

void VstRack::RestoreState(int slot, const uint8_t* data, size_t size) {
  if (slot < 0 || slot >= kVstSlots || data == nullptr || size == 0) {
    return;
  }
  VstPlugin* plugin = slots_[slot].plugin.load(std::memory_order_acquire);
  if (plugin == nullptr) {
    return;
  }
  plugin->Restore(data, size);
}

void VstRack::Slot(int slot, VstSlotView& out) const {
  out = {};
  if (slot < 0 || slot >= kVstSlots) {
    return;
  }
  VstPlugin* plugin = slots_[slot].plugin.load(std::memory_order_acquire);
  if (plugin == nullptr) {
    return;
  }
  out.used = true;
  out.strip = slots_[slot].strip.load(std::memory_order_relaxed);
  out.bypass = slots_[slot].bypass.load(std::memory_order_relaxed);
  out.group = slots_[slot].group.load(std::memory_order_relaxed);
  out.editor = plugin->editorOpen;
  wcsncpy(out.name, plugin->name, 127);
  wcsncpy(out.path, plugin->path, 519);
}

void VstRack::Process(float* const* shaped, int strips, int frames) {
  if (shaped == nullptr || frames <= 0) {
    return;
  }
  for (int slot = 0; slot < kVstSlots; ++slot) {
    VstPlugin* plugin = slots_[slot].plugin.load(std::memory_order_acquire);
    if (plugin == nullptr || slots_[slot].bypass.load(std::memory_order_relaxed)) {
      continue;
    }
    const int group = slots_[slot].group.load(std::memory_order_relaxed);
    if (group >= 0 && group < kVstGroups && groupBypass_[group].load(std::memory_order_relaxed)) {
      continue;
    }
    const int strip = slots_[slot].strip.load(std::memory_order_relaxed);
    if (strip < 0 || strip >= strips || shaped[strip] == nullptr ||
        !localOn_[strip].load(std::memory_order_relaxed)) {
      continue;
    }
    plugin->inUse.fetch_add(1, std::memory_order_acq_rel);
    if (slots_[slot].plugin.load(std::memory_order_acquire) == plugin &&
        plugin->alive.load(std::memory_order_acquire)) {
      try {
        plugin->Process(shaped[strip], frames);
      } catch (...) {
      }
    }
    plugin->inUse.fetch_sub(1, std::memory_order_acq_rel);
  }
}

}  // namespace anymix
