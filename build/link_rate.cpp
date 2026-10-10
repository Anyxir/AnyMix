// Read-only probe: what rate does ASIO Link Pro actually report?
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <cstring>

namespace {

struct AsioChannelInfo {
  long channel;
  long isInput;
  long isActive;
  long channelGroup;
  long type;
  char name[32];
};
struct AsioBufferInfo {
  long isInput;
  long channelNum;
  void* buffers[2];
};
struct AsioTime;
struct AsioCallbacks {
  void (*bufferSwitch)(long, long);
  void (*sampleRateDidChange)(double);
  long (*asioMessage)(long, long, void*, double*);
  AsioTime* (*bufferSwitchTimeInfo)(AsioTime*, long, long);
};
struct AsioVtbl {
  HRESULT(__stdcall* QueryInterface)(void*, const GUID&, void**);
  unsigned long(__stdcall* AddRef)(void*);
  unsigned long(__stdcall* Release)(void*);
  long(__stdcall* init)(void*, void*);
  void(__stdcall* getDriverName)(void*, char*);
  long(__stdcall* getDriverVersion)(void*);
  void(__stdcall* getErrorMessage)(void*, char*);
  long(__stdcall* start)(void*);
  long(__stdcall* stop)(void*);
  long(__stdcall* getChannels)(void*, long*, long*);
  long(__stdcall* getLatencies)(void*, long*, long*);
  long(__stdcall* getBufferSize)(void*, long*, long*, long*, long*);
  long(__stdcall* canSampleRate)(void*, double);
  long(__stdcall* getSampleRate)(void*, double*);
  long(__stdcall* setSampleRate)(void*, double);
  long(__stdcall* getClockSources)(void*, void*, long*);
  long(__stdcall* setClockSource)(void*, long);
  long(__stdcall* getSamplePosition)(void*, void*, void*);
  long(__stdcall* getChannelInfo)(void*, AsioChannelInfo*);
  long(__stdcall* createBuffers)(void*, AsioBufferInfo*, long, long, AsioCallbacks*);
  long(__stdcall* disposeBuffers)(void*);
  long(__stdcall* controlPanel)(void*);
  long(__stdcall* future)(void*, long, void*);
  long(__stdcall* outputReady)(void*);
};
struct AsioDriver {
  AsioVtbl* lpVtbl;
};

constexpr char kApplyFlag[] = "--apply";

}  // namespace

int main(int argc, char** argv) {
  const bool apply = argc > 1 && std::strcmp(argv[1], kApplyFlag) == 0;
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const GUID id = {0xF1442B0B, 0x6489, 0x449B, {0x9F, 0x32, 0xD2, 0x6C, 0x52, 0xF0, 0xC4, 0x91}};
  AsioDriver* driver = nullptr;
  HRESULT hr = CoCreateInstance(id, nullptr, CLSCTX_INPROC_SERVER, id, reinterpret_cast<void**>(&driver));
  std::printf("CoCreateInstance hr=0x%08lX driver=%p\n", static_cast<unsigned long>(hr),
              static_cast<void*>(driver));
  if (FAILED(hr) || driver == nullptr) {
    if (SUCCEEDED(com)) {
      CoUninitialize();
    }
    return 1;
  }
  char name[32] = {};
  driver->lpVtbl->getDriverName(driver, name);
  if (driver->lpVtbl->init(driver, GetDesktopWindow()) == 0) {
    char error[128] = {};
    driver->lpVtbl->getErrorMessage(driver, error);
    std::printf("driver='%s' init FAILED: %s\n", name, error);
    driver->lpVtbl->Release(driver);
    if (SUCCEEDED(com)) {
      CoUninitialize();
    }
    return 2;
  }
  long inputs = 0;
  long outputs = 0;
  driver->lpVtbl->getChannels(driver, &inputs, &outputs);
  double rate = 0.0;
  driver->lpVtbl->getSampleRate(driver, &rate);
  const long can48 = driver->lpVtbl->canSampleRate(driver, 48000.0);
  std::printf("driver='%s' inputs=%ld outputs=%ld rate=%.0f can48k=%ld\n", name, inputs, outputs, rate,
              can48);
  if (apply) {
    const long set = driver->lpVtbl->setSampleRate(driver, 48000.0);
    double after = 0.0;
    driver->lpVtbl->getSampleRate(driver, &after);
    std::printf("setSampleRate(48000) -> %ld ; rate now %.0f\n", set, after);
  }
  driver->lpVtbl->Release(driver);
  if (SUCCEEDED(com)) {
    CoUninitialize();
  }
  return 0;
}
