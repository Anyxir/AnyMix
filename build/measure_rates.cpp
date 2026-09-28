#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <cmath>
#include <cstdint>
#include <cstdio>

#pragma pack(push, 4)
struct Req {
  uint32_t magic;
  uint32_t stream;
  uint32_t frames;
  uint32_t reserved;
  float samples[512 * 2];
};
#pragma pack(pop)

static double QpcMs(LARGE_INTEGER freq, LARGE_INTEGER t0, LARGE_INTEGER t1) {
  return (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
}

static void DriverRate(bool readDriver, double seconds) {
  IMMDeviceEnumerator* enumerator = nullptr;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                   reinterpret_cast<void**>(&enumerator));
  IMMDevice* device = nullptr;
  enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
  enumerator->Release();
  IAudioClient* client = nullptr;
  device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
  device->Release();
  WAVEFORMATEX* mix = nullptr;
  client->GetMixFormat(&mix);
  client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, mix, nullptr);
  UINT32 buffer = 0;
  client->GetBufferSize(&buffer);
  REFERENCE_TIME defPeriod = 0;
  REFERENCE_TIME minPeriod = 0;
  client->GetDevicePeriod(&defPeriod, &minPeriod);
  IAudioRenderClient* render = nullptr;
  client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render));
  client->Start();
  HANDLE driver = INVALID_HANDLE_VALUE;
  if (readDriver) {
    driver = CreateFileW(L"\\\\.\\AnyMix", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  const unsigned long ioctl = (0x0000001Du << 16) | (1u << 14) | (0x800u << 2);
  LARGE_INTEGER freq{}, t0{}, t1{};
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);
  uint64_t reported[5] = {};
  uint32_t reads[5] = {};
  uint32_t minF[5] = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
  uint32_t maxF[5] = {};
  uint64_t tailZeros[5] = {};
  uint32_t exact512[5] = {};
  float phase = 0.f;
  uint64_t played = 0;
  while (true) {
    QueryPerformanceCounter(&t1);
    if (QpcMs(freq, t0, t1) >= seconds * 1000.0) {
      break;
    }
    UINT32 padding = 0;
    client->GetCurrentPadding(&padding);
    UINT32 need = buffer > padding ? buffer - padding : 0;
    if (need > 0) {
      BYTE* data = nullptr;
      if (SUCCEEDED(render->GetBuffer(need, &data)) && data != nullptr) {
        float* out = reinterpret_cast<float*>(data);
        for (UINT32 i = 0; i < need; ++i) {
          const float s = 0.125f * std::sin(phase);
          phase += 2.f * 3.14159265f * 1000.f / static_cast<float>(mix->nSamplesPerSec);
          if (mix->nChannels >= 2 && mix->wBitsPerSample == 32) {
            out[i * mix->nChannels] = s;
            out[i * mix->nChannels + 1] = s;
          }
        }
        render->ReleaseBuffer(need, 0);
        played += need;
      }
    }
    if (readDriver) {
    for (uint32_t stream = 0; stream < 5; ++stream) {
      Req request{};
      request.magic = 0x414D5831u;
      request.stream = stream;
      request.frames = 512;
      DWORD got = 0;
      if (!DeviceIoControl(driver, ioctl, &request, sizeof(request), &request, sizeof(request), &got,
                           nullptr)) {
        continue;
      }
      ++reads[stream];
      const uint32_t frames = request.frames > 512 ? 512 : request.frames;
      reported[stream] += frames;
      if (frames < minF[stream]) minF[stream] = frames;
      if (frames > maxF[stream]) maxF[stream] = frames;
      if (frames == 512) ++exact512[stream];
      uint32_t end = frames;
      while (end > 0 && request.samples[(end - 1) * 2] == 0.f && request.samples[(end - 1) * 2 + 1] == 0.f) {
        --end;
      }
      tailZeros[stream] += frames - end;
    }
    }
    Sleep(1);
  }
  QueryPerformanceCounter(&t1);
  const double sec = QpcMs(freq, t0, t1) / 1000.0;
  wprintf(L"DRIVER_TEST sec=%.3f playedFrames=%llu playedPerSec=%.1f mix=%lu buffer=%u period100ns=%lld\n",
          sec, played, played / sec, mix->nSamplesPerSec, buffer, defPeriod);
  if (readDriver) {
  for (int s = 0; s < 5; ++s) {
    if (reads[s] == 0) continue;
    wprintf(L"stream %d reads=%u reportedPerSec=%.1f min=%u max=%u avg=%.1f exact512=%u tailZeroPerSec=%.1f\n",
            s, reads[s], reported[s] / sec, minF[s], maxF[s], reported[s] / static_cast<double>(reads[s]),
            exact512[s], tailZeros[s] / sec);
  }
  }
  client->Stop();
  render->Release();
  client->Release();
  CoTaskMemFree(mix);
  if (driver != INVALID_HANDLE_VALUE) {
    CloseHandle(driver);
  }
}

static void RingTest() {
  const int sizes[] = {480, 960, 2048, 4096, 8192};
  for (int size : sizes) {
    float* data = new float[static_cast<size_t>(size) * 2]();
    uint32_t read = 0;
    uint32_t write = 0;
    auto avail = [&] { return write - read; };
    uint32_t minFill = 0xffffffff;
    uint32_t maxFill = 0;
    uint64_t fillSum = 0;
    uint32_t samples = 0;
    uint32_t underrun = 0;
    uint32_t overrun = 0;
    const int chunk = 48;
    for (int step = 0; step < 48000 / chunk; ++step) {
      if (static_cast<int>(avail()) + chunk > size) overrun += chunk;
      else {
        write += chunk;
      }
      if (avail() < static_cast<uint32_t>(chunk)) underrun += chunk;
      else read += chunk;
      const uint32_t fill = avail();
      if (fill < minFill) minFill = fill;
      if (fill > maxFill) maxFill = fill;
      fillSum += fill;
      ++samples;
    }
    wprintf(L"RING size=%d chunk=%d underrun=%u overrun=%u fillMin=%u fillMax=%u fillAvg=%llu\n", size, chunk,
            underrun, overrun, minFill, maxFill, samples ? fillSum / samples : 0);
    delete[] data;
  }
}

int wmain(int argc, wchar_t** argv) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool engineOnly = argc > 1 && wcscmp(argv[1], L"play") == 0;
  double seconds = 30.0;
  if (argc > 2) {
    seconds = wcstod(argv[2], nullptr);
  }
  if (seconds < 1.0) {
    seconds = 30.0;
  }
  if (!engineOnly) {
    wprintf(L"--- driver while 1 kHz plays into default output ---\n");
    DriverRate(true, seconds);
    wprintf(L"--- ring equal rate ---\n");
    RingTest();
  } else {
    wprintf(L"--- play only, engine is the reader ---\n");
    DriverRate(false, seconds);
  }
  CoUninitialize();
  return 0;
}
