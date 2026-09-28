#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <cstdio>

struct AsioChannelInfo {
  long channel;
  long isInput;
  long isActive;
  long channelGroup;
  long type;
  char name[32];
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
  long(__stdcall* createBuffers)(void*, void*, long, long, void*);
  long(__stdcall* disposeBuffers)(void*);
  long(__stdcall* controlPanel)(void*);
  long(__stdcall* future)(void*, long, void*);
  long(__stdcall* outputReady)(void*);
};
struct AsioDriver {
  AsioVtbl* lpVtbl;
};

int main() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  std::printf("com %08lx\n", static_cast<unsigned long>(com));
  const GUID id = {0xF1442B0B, 0x6489, 0x449B, {0x9F, 0x32, 0xD2, 0x6C, 0x52, 0xF0, 0xC4, 0x91}};
  AsioDriver* drv = nullptr;
  const HRESULT hr = CoCreateInstance(id, nullptr, CLSCTX_INPROC_SERVER, id, reinterpret_cast<void**>(&drv));
  std::printf("create %08lx ptr %p\n", static_cast<unsigned long>(hr), static_cast<void*>(drv));
  if (FAILED(hr) || drv == nullptr) {
    return 1;
  }
  const long init = drv->lpVtbl->init(drv, GetDesktopWindow());
  char name[128] = {};
  char err[256] = {};
  drv->lpVtbl->getDriverName(drv, name);
  drv->lpVtbl->getErrorMessage(drv, err);
  std::printf("init %ld name %s err %s\n", init, name, err);
  long ins = 0;
  long outs = 0;
  std::printf("channels %ld\n", drv->lpVtbl->getChannels(drv, &ins, &outs));
  std::printf("in %ld out %ld\n", ins, outs);
  long mn = 0, mx = 0, pref = 0, gran = 0;
  std::printf("buf %ld\n", drv->lpVtbl->getBufferSize(drv, &mn, &mx, &pref, &gran));
  std::printf("min %ld max %ld pref %ld gran %ld\n", mn, mx, pref, gran);
  double rate = 0;
  std::printf("rate hr %ld\n", drv->lpVtbl->getSampleRate(drv, &rate));
  std::printf("rate %f can48 %ld\n", rate, drv->lpVtbl->canSampleRate(drv, 48000.0));
  const long shown = outs < 8 ? outs : 8;
  for (long i = 0; i < shown; ++i) {
    AsioChannelInfo info{};
    info.channel = i;
    info.isInput = 0;
    const long ch = drv->lpVtbl->getChannelInfo(drv, &info);
    std::printf("out %ld hr %ld type %ld group %ld name %s\n", i, ch, info.type, info.channelGroup, info.name);
  }
  drv->lpVtbl->Release(drv);
  CoUninitialize();
  return 0;
}
