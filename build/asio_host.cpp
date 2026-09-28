#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef long ASIOBool;
typedef long ASIOError;
typedef double ASIOSampleRate;
typedef long ASIOSampleType;

struct ASIOSamples { unsigned long hi; unsigned long lo; };
struct ASIOTimeStamp { unsigned long hi; unsigned long lo; };
struct ASIOClockSource {
  long index, associatedChannel, associatedGroup;
  ASIOBool isCurrentSource;
  char name[32];
};
struct ASIOChannelInfo {
  long channel;
  ASIOBool isInput;
  ASIOBool isActive;
  long channelGroup;
  ASIOSampleType type;
  char name[32];
};
struct ASIOBufferInfo {
  ASIOBool isInput;
  long channelNum;
  void* buffers[2];
};
struct ASIOTime { long reserved[4]; char rest[256]; };
struct ASIOCallbacks {
  void (*bufferSwitch)(long index, ASIOBool directProcess);
  void (*sampleRateDidChange)(ASIOSampleRate rate);
  long (*asioMessage)(long selector, long value, void* message, double* opt);
  ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, long index, ASIOBool directProcess);
};

struct IASIO : IUnknown {
  virtual ASIOBool init(void* sysHandle) = 0;
  virtual void getDriverName(char* name) = 0;
  virtual long getDriverVersion() = 0;
  virtual void getErrorMessage(char* text) = 0;
  virtual ASIOError start() = 0;
  virtual ASIOError stop() = 0;
  virtual ASIOError getChannels(long* inputs, long* outputs) = 0;
  virtual ASIOError getLatencies(long* input, long* output) = 0;
  virtual ASIOError getBufferSize(long* a, long* b, long* c, long* d) = 0;
  virtual ASIOError canSampleRate(ASIOSampleRate rate) = 0;
  virtual ASIOError getSampleRate(ASIOSampleRate* rate) = 0;
  virtual ASIOError setSampleRate(ASIOSampleRate rate) = 0;
  virtual ASIOError getClockSources(ASIOClockSource* clocks, long* count) = 0;
  virtual ASIOError setClockSource(long reference) = 0;
  virtual ASIOError getSamplePosition(ASIOSamples* pos, ASIOTimeStamp* stamp) = 0;
  virtual ASIOError getChannelInfo(ASIOChannelInfo* info) = 0;
  virtual ASIOError createBuffers(ASIOBufferInfo* infos, long channels, long bufferSize, ASIOCallbacks* callbacks) = 0;
  virtual ASIOError disposeBuffers() = 0;
  virtual ASIOError controlPanel() = 0;
  virtual ASIOError future(long selector, void* opt) = 0;
  virtual ASIOError outputReady() = 0;
};

static volatile long g_calls = 0;
static void bufferSwitch(long index, ASIOBool directProcess) {
  InterlockedIncrement(&g_calls);
  (void)index;
  (void)directProcess;
}

int main() {
  printf("ASIOBufferInfo size=%zu buffers0=%zu\n", sizeof(ASIOBufferInfo), offsetof(ASIOBufferInfo, buffers));
  printf("ASIOChannelInfo size=%zu name=%zu\n", sizeof(ASIOChannelInfo), offsetof(ASIOChannelInfo, name));
  printf("ASIOCallbacks size=%zu\n", sizeof(ASIOCallbacks));

  HMODULE dll = LoadLibraryW(L"C:\\Users\\akram\\Downloads\\vb\\build\\AnyMixInsertASIO.dll");
  if (!dll) { printf("load fail %lu\n", GetLastError()); return 1; }
  auto getClass = (HRESULT (STDMETHODCALLTYPE*)(REFCLSID, REFIID, void**))GetProcAddress(dll, "DllGetClassObject");
  if (!getClass) { printf("no DllGetClassObject\n"); return 1; }
  const GUID clsid = {0x8f3a6c21, 0x4b7e, 0x4d15, {0x9a, 0x62, 0x1c, 0x0e, 0x5b, 0x7d, 0x4a, 0x90}};
  IClassFactory* factory = nullptr;
  HRESULT hr = getClass(clsid, IID_IClassFactory, (void**)&factory);
  printf("factory hr=%08lx\n", (unsigned long)hr);
  if (FAILED(hr) || !factory) return 1;
  IASIO* asio = nullptr;
  hr = factory->CreateInstance(nullptr, clsid, (void**)&asio);
  printf("create hr=%08lx asio=%p\n", (unsigned long)hr, (void*)asio);
  factory->Release();
  if (FAILED(hr) || !asio) return 1;
  long inited = asio->init(GetDesktopWindow());
  printf("init=%ld\n", inited);
  char name[64] = {};
  asio->getDriverName(name);
  printf("name=%s\n", name);
  long inputs = 0, outputs = 0;
  printf("channels err=%ld\n", asio->getChannels(&inputs, &outputs));
  printf("in=%ld out=%ld\n", inputs, outputs);
  long mn=0,mx=0,pref=0,gran=0;
  printf("buf err=%ld\n", asio->getBufferSize(&mn,&mx,&pref,&gran));
  printf("buf %ld %ld %ld %ld\n", mn, mx, pref, gran);
  ASIOSampleRate rate = 0;
  printf("getrate err=%ld\n", asio->getSampleRate(&rate));
  printf("rate=%f set=%ld can44=%ld\n", rate, asio->setSampleRate(48000.0), asio->canSampleRate(44100.0));
  ASIOChannelInfo info{};
  info.channel = 0;
  info.isInput = 1;
  printf("info err=%ld\n", asio->getChannelInfo(&info));
  printf("ch name=%s type=%ld\n", info.name, info.type);

  const long n = inputs + outputs;
  ASIOBufferInfo* infos = new ASIOBufferInfo[n];
  memset(infos, 0, sizeof(ASIOBufferInfo) * n);
  for (long i = 0; i < inputs; ++i) {
    infos[i].isInput = 1;
    infos[i].channelNum = i;
  }
  for (long i = 0; i < outputs; ++i) {
    infos[inputs + i].isInput = 0;
    infos[inputs + i].channelNum = i;
  }
  ASIOCallbacks cb{};
  cb.bufferSwitch = bufferSwitch;
  long err = asio->createBuffers(infos, n, pref, &cb);
  printf("createBuffers=%ld\n", err);
  printf("ptr0=%p ptr1=%p\n", infos[0].buffers[0], infos[0].buffers[1]);
  if (infos[0].buffers[0]) {
    float* s = (float*)infos[0].buffers[0];
    printf("sample0=%f\n", s[0]);
  }
  err = asio->start();
  printf("start=%ld\n", err);
  Sleep(300);
  printf("callbacks=%ld\n", g_calls);
  ASIOSamples pos{};
  ASIOTimeStamp stamp{};
  printf("pos err=%ld\n", asio->getSamplePosition(&pos, &stamp));
  printf("pos hi=%lu lo=%lu\n", pos.hi, pos.lo);
  asio->stop();
  asio->disposeBuffers();
  asio->Release();
  delete[] infos;
  printf("done\n");
  return 0;
}
