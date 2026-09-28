#include "anymix/Engine.h"

#include "anymix/Bridge.h"
#include "anymix/NoiseGate.h"
#include "anymix/Process.h"
#include "anymix/VstHost.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <functiondiscoverykeys_devpkey.h>
#include <objbase.h>
#include <ksmedia.h>
#include <avrt.h>
#include <xmmintrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#ifndef AUDCLNT_STREAMFLAGS_LOOPBACK
#define AUDCLNT_STREAMFLAGS_LOOPBACK 0x00020000
#endif
#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace anymix {
namespace {

constexpr int kRingFrames = 16384;
constexpr float kBassHz = 85.f;
constexpr float kTrebleHz = 3200.f;

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

float DbToGain(float db) {
  if (db <= -59.9f) {
    return 0.f;
  }
  return std::pow(10.f, db / 20.f);
}

float PeakOf(const float* samples, int count) {
  float peak = 0.f;
  for (int i = 0; i < count; ++i) {
    peak = std::max(peak, std::fabs(samples[i]));
  }
  return peak;
}

void StereoPeaks(const float* samples, int frames, float gain, bool mono, float& left, float& right,
                   float pan = 0.f) {
  left = 0.f;
  right = 0.f;
  if (samples == nullptr || frames <= 0) {
    return;
  }
  const float scale = std::fabs(gain);
  for (int i = 0; i < frames; ++i) {
    float l = samples[i * 2];
    float r = samples[i * 2 + 1];
    if (mono) {
      const float mid = 0.5f * (l + r);
      l = mid;
      r = mid;
    }
    ApplyBalance(l, r, pan);
    left = std::max(left, std::fabs(l) * scale);
    right = std::max(right, std::fabs(r) * scale);
  }
}

struct FloatRing {
  float data[kRingFrames * kChannels]{};
  uint32_t read = 0;
  uint32_t write = 0;

  uint32_t Available() const { return write - read; }

  uint32_t Push(const float* interleaved, uint32_t frames) {
    if (frames == 0) {
      return 0;
    }
    const uint32_t free = kRingFrames - Available();
    uint32_t dropped = 0;
    if (frames > free) {
      dropped = frames - free;
      read += dropped;
    }
    for (uint32_t i = 0; i < frames; ++i) {
      const uint32_t index = (write + i) % kRingFrames;
      data[index * 2] = interleaved[i * 2];
      data[index * 2 + 1] = interleaved[i * 2 + 1];
    }
    write += frames;
    return dropped;
  }

  uint32_t Pop(float* interleaved, uint32_t frames) {
    const uint32_t have = std::min(frames, Available());
    for (uint32_t i = 0; i < have; ++i) {
      const uint32_t index = (read + i) % kRingFrames;
      interleaved[i * 2] = data[index * 2];
      interleaved[i * 2 + 1] = data[index * 2 + 1];
    }
    read += have;
    if (have < frames) {
      std::memset(interleaved + have * 2, 0, sizeof(float) * (frames - have) * 2);
    }
    return have;
  }

  void Peek(uint32_t offset, float& left, float& right) const {
    const uint32_t index = (read + offset) % kRingFrames;
    left = data[index * 2];
    right = data[index * 2 + 1];
  }

  void Clear() {
    read = 0;
    write = 0;
  }
};

struct MicPull {
  double frac = 0;
  float holdL = 0.f;
  float holdR = 0.f;
  bool primed = false;

  void Reset() {
    frac = 0;
    holdL = 0.f;
    holdR = 0.f;
    primed = false;
  }
};

constexpr uint32_t kMicTargetFrames = 960;

void PullMic(FloatRing& ring, MicPull& pull, float* out, uint32_t frames, uint64_t& holds) {
  const uint32_t fill = ring.Available();
  if (!pull.primed) {
    if (fill < kMicTargetFrames) {
      std::memset(out, 0, sizeof(float) * frames * kChannels);
      return;
    }
    pull.primed = true;
  }
  double step = 1.0 + (static_cast<double>(fill) - static_cast<double>(kMicTargetFrames)) / 24000.0;
  if (step < 0.997) {
    step = 0.997;
  }
  if (step > 1.003) {
    step = 1.003;
  }
  const double maxConsume = fill > 2 ? static_cast<double>(fill - 2) : 0.0;
  if (frames > 0 && step * static_cast<double>(frames) > maxConsume) {
    step = maxConsume / static_cast<double>(frames);
  }
  float discarded[2];
  for (uint32_t i = 0; i < frames; ++i) {
    if (ring.Available() >= 2) {
      float l0 = 0.f;
      float r0 = 0.f;
      float l1 = 0.f;
      float r1 = 0.f;
      ring.Peek(0, l0, r0);
      ring.Peek(1, l1, r1);
      const float t = static_cast<float>(pull.frac);
      pull.holdL = l0 + (l1 - l0) * t;
      pull.holdR = r0 + (r1 - r0) * t;
    } else {
      holds += 1;
    }
    out[i * 2] = pull.holdL;
    out[i * 2 + 1] = pull.holdR;
    pull.frac += step;
    while (pull.frac >= 1.0 && ring.Available() > 2) {
      ring.Pop(discarded, 1);
      pull.frac -= 1.0;
    }
    if (pull.frac >= 1.0) {
      pull.frac = 0.0;
    }
  }
}

struct WasapiEnd {
  IMMDevice* device = nullptr;
  IAudioClient* client = nullptr;
  IAudioCaptureClient* capture = nullptr;
  IAudioRenderClient* render = nullptr;
  UINT32 bufferFrames = 0;
  int rate = kSampleRate;
  int channels = kChannels;
  int bits = 32;
  int blockAlign = 8;
  bool isFloat = true;
  HANDLE event = nullptr;
  bool open = false;

  void Close() {
    if (client != nullptr) {
      client->Stop();
    }
    if (event != nullptr) {
      CloseHandle(event);
      event = nullptr;
    }
    if (capture != nullptr) {
      capture->Release();
      capture = nullptr;
    }
    if (render != nullptr) {
      render->Release();
      render = nullptr;
    }
    if (client != nullptr) {
      client->Release();
      client = nullptr;
    }
    if (device != nullptr) {
      device->Release();
      device = nullptr;
    }
    open = false;
    bufferFrames = 0;
  }
};

bool OpenEndpoint(const wchar_t* id, bool capture, WasapiEnd& end, std::wstring& error) {
  end.Close();
  if (id == nullptr || id[0] == 0) {
    return false;
  }
  IMMDeviceEnumerator* enumerator = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator));
  if (FAILED(hr) || enumerator == nullptr) {
    error = L"Cannot create the Windows device enumerator.";
    return false;
  }
  hr = enumerator->GetDevice(id, &end.device);
  enumerator->Release();
  if (FAILED(hr) || end.device == nullptr) {
    error = L"Windows audio endpoint was not found.";
    return false;
  }
  hr = end.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                            reinterpret_cast<void**>(&end.client));
  if (FAILED(hr) || end.client == nullptr) {
    error = L"Cannot activate the audio client.";
    end.Close();
    return false;
  }

  const GUID ieeeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
  WAVEFORMATEXTENSIBLE format{};
  format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  format.Format.nChannels = kChannels;
  format.Format.nSamplesPerSec = kSampleRate;
  format.Format.wBitsPerSample = 32;
  format.Format.nBlockAlign = kChannels * 4;
  format.Format.nAvgBytesPerSec = kSampleRate * format.Format.nBlockAlign;
  format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  format.Samples.wValidBitsPerSample = 32;
  format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  format.SubFormat = ieeeFloat;
  end.rate = kSampleRate;
  end.channels = kChannels;
  end.bits = 32;
  end.blockAlign = kChannels * 4;
  end.isFloat = true;
  if (!capture) {
    end.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  }
  const DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                      (end.event != nullptr ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0);
  const REFERENCE_TIME duration =
      static_cast<REFERENCE_TIME>((static_cast<double>(kBlockFrames * 4) * 10000000.0) / kSampleRate);
  hr = end.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, duration, 0,
                              reinterpret_cast<WAVEFORMATEX*>(&format), nullptr);
  if (FAILED(hr)) {
    wchar_t text[160];
    wsprintfW(text, L"WASAPI Initialize failed (0x%08X).", static_cast<unsigned>(hr));
    error = text;
    end.Close();
    return false;
  }
  hr = end.client->GetBufferSize(&end.bufferFrames);
  if (FAILED(hr) || end.bufferFrames == 0) {
    error = L"The endpoint returned an empty buffer.";
    end.Close();
    return false;
  }
  if (capture) {
    hr = end.client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&end.capture));
  } else {
    hr = end.client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&end.render));
  }
  if (FAILED(hr)) {
    error = L"The endpoint did not provide a capture or render client.";
    end.Close();
    return false;
  }
  if (end.event != nullptr) {
    hr = end.client->SetEventHandle(end.event);
    if (FAILED(hr)) {
      error = L"The endpoint did not accept an event handle.";
      end.Close();
      return false;
    }
    BYTE* prime = nullptr;
    if (SUCCEEDED(end.render->GetBuffer(end.bufferFrames, &prime)) && prime != nullptr) {
      std::memset(prime, 0, static_cast<size_t>(end.bufferFrames) * end.blockAlign);
      end.render->ReleaseBuffer(end.bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
    }
  }
  hr = end.client->Start();
  if (FAILED(hr)) {
    error = L"The endpoint failed to start.";
    end.Close();
    return false;
  }
  end.open = true;
  return true;
}

bool FormatIsFloat(const WAVEFORMATEX* format) {
  if (format == nullptr) {
    return false;
  }
  if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
    return true;
  }
  if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE || format->cbSize < 22) {
    return false;
  }
  const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
  const GUID ieee = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
  return IsEqualGUID(ext->SubFormat, ieee) != FALSE;
}

void DescribeFormat(WasapiEnd& end, const WAVEFORMATEX* format) {
  end.rate = static_cast<int>(format->nSamplesPerSec);
  end.channels = format->nChannels;
  end.bits = format->wBitsPerSample;
  end.blockAlign = format->nBlockAlign;
  end.isFloat = FormatIsFloat(format);
}

bool FindVadRender(const wchar_t* label, wchar_t* id, int capacity) {
  if (id == nullptr || capacity <= 0) {
    return false;
  }
  id[0] = 0;
  wchar_t wanted[160];
  wsprintfW(wanted, L"%s (ASIOVADPRO Driver)", label);
  IMMDeviceEnumerator* enumerator = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator));
  if (FAILED(hr) || enumerator == nullptr) {
    return false;
  }
  IMMDeviceCollection* collection = nullptr;
  hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
  enumerator->Release();
  if (FAILED(hr) || collection == nullptr) {
    return false;
  }
  UINT total = 0;
  collection->GetCount(&total);
  bool found = false;
  for (UINT i = 0; i < total && !found; ++i) {
    IMMDevice* device = nullptr;
    if (FAILED(collection->Item(i, &device)) || device == nullptr) {
      continue;
    }
    IPropertyStore* store = nullptr;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store != nullptr) {
      PROPVARIANT name;
      PropVariantInit(&name);
      if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR &&
          name.pwszVal != nullptr && wcscmp(name.pwszVal, wanted) == 0) {
        LPWSTR endpoint = nullptr;
        if (SUCCEEDED(device->GetId(&endpoint)) && endpoint != nullptr) {
          CopyText(id, capacity, endpoint);
          CoTaskMemFree(endpoint);
          found = id[0] != 0;
        }
      }
      PropVariantClear(&name);
      store->Release();
    }
    device->Release();
  }
  collection->Release();
  return found;
}

bool OpenLoopback(const wchar_t* id, WasapiEnd& end, std::wstring& error) {
  end.Close();
  if (id == nullptr || id[0] == 0) {
    return false;
  }
  IMMDeviceEnumerator* enumerator = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator));
  if (FAILED(hr) || enumerator == nullptr) {
    error = L"Cannot create the Windows device enumerator.";
    return false;
  }
  hr = enumerator->GetDevice(id, &end.device);
  enumerator->Release();
  if (FAILED(hr) || end.device == nullptr) {
    error = L"The virtual playback device was not found.";
    return false;
  }
  hr = end.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                            reinterpret_cast<void**>(&end.client));
  if (FAILED(hr) || end.client == nullptr) {
    error = L"Cannot activate the virtual playback device.";
    end.Close();
    return false;
  }

  const GUID ieeeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
  WAVEFORMATEXTENSIBLE format{};
  format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  format.Format.nChannels = kChannels;
  format.Format.nSamplesPerSec = kSampleRate;
  format.Format.wBitsPerSample = 32;
  format.Format.nBlockAlign = kChannels * 4;
  format.Format.nAvgBytesPerSec = kSampleRate * format.Format.nBlockAlign;
  format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  format.Samples.wValidBitsPerSample = 32;
  format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  format.SubFormat = ieeeFloat;
  DescribeFormat(end, reinterpret_cast<WAVEFORMATEX*>(&format));
  const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
  const REFERENCE_TIME duration =
      static_cast<REFERENCE_TIME>((static_cast<double>(kBlockFrames * 4) * 10000000.0) / kSampleRate);
  hr = end.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, duration, 0,
                              reinterpret_cast<WAVEFORMATEX*>(&format), nullptr);
  if (FAILED(hr)) {
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(end.client->GetMixFormat(&mix)) || mix == nullptr) {
      error = L"The virtual playback device rejected loopback.";
      end.Close();
      return false;
    }
    DescribeFormat(end, mix);
    hr = end.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, duration, 0, mix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(hr)) {
      wchar_t text[160];
      wsprintfW(text, L"Loopback Initialize failed (0x%08X).", static_cast<unsigned>(hr));
      error = text;
      end.Close();
      return false;
    }
  }
  hr = end.client->GetBufferSize(&end.bufferFrames);
  if (FAILED(hr) || end.bufferFrames == 0) {
    error = L"The virtual playback device returned an empty buffer.";
    end.Close();
    return false;
  }
  hr = end.client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&end.capture));
  if (FAILED(hr) || end.capture == nullptr) {
    error = L"The virtual playback device did not provide a loopback client.";
    end.Close();
    return false;
  }
  hr = end.client->Start();
  if (FAILED(hr)) {
    error = L"The virtual playback device failed to start.";
    end.Close();
    return false;
  }
  end.open = true;
  return true;
}

void DecodeFrame(const WasapiEnd& end, const BYTE* frame, float& left, float& right) {
  auto sample = [&](int channel) -> float {
    if (channel >= end.channels) {
      channel = 0;
    }
    const BYTE* p = frame + channel * (end.bits / 8);
    if (end.isFloat) {
      float value = 0.f;
      std::memcpy(&value, p, sizeof(float));
      return value;
    }
    if (end.bits == 16) {
      int16_t value = 0;
      std::memcpy(&value, p, sizeof(int16_t));
      return static_cast<float>(value) / 32768.f;
    }
    int32_t value = 0;
    std::memcpy(&value, p, sizeof(int32_t));
    return static_cast<float>(value) / 2147483648.f;
  };
  left = sample(0);
  right = end.channels > 1 ? sample(1) : left;
}

uint32_t DrainLoopback(WasapiEnd& end, FloatRing& ring) {
  if (!end.open || end.capture == nullptr) {
    return 0;
  }
  uint32_t pushed = 0;
  UINT32 packet = 0;
  float block[1024 * 2];
  while (SUCCEEDED(end.capture->GetNextPacketSize(&packet)) && packet > 0) {
    BYTE* data = nullptr;
    UINT32 frames = 0;
    DWORD flags = 0;
    if (FAILED(end.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
      break;
    }
    UINT32 offset = 0;
    while (offset < frames) {
      const UINT32 count = std::min<UINT32>(1024, frames - offset);
      if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
        std::memset(block, 0, sizeof(float) * count * 2);
      } else {
        const BYTE* bytes = data + static_cast<size_t>(offset) * end.blockAlign;
        for (UINT32 i = 0; i < count; ++i) {
          DecodeFrame(end, bytes + static_cast<size_t>(i) * end.blockAlign, block[i * 2], block[i * 2 + 1]);
        }
      }
      ring.Push(block, count);
      pushed += count;
      offset += count;
    }
    end.capture->ReleaseBuffer(frames);
  }
  return pushed;
}

void EncodeFrame(const WasapiEnd& end, float left, float right, BYTE* frame) {
  std::memset(frame, 0, static_cast<size_t>(end.blockAlign));
  auto write = [&](int channel, float value) {
    if (channel >= end.channels) {
      return;
    }
    BYTE* p = frame + channel * (end.bits / 8);
    value = std::max(-1.f, std::min(1.f, value));
    if (end.isFloat) {
      std::memcpy(p, &value, sizeof(float));
    } else if (end.bits == 16) {
      const int16_t pcm = static_cast<int16_t>(value * 32767.f);
      std::memcpy(p, &pcm, sizeof(pcm));
    } else {
      const int32_t pcm = static_cast<int32_t>(value * 2147483647.f);
      std::memcpy(p, &pcm, sizeof(pcm));
    }
  };
  write(0, left);
  if (end.channels > 1) {
    write(1, right);
  }
}

void PublishEq(PublishedEq& dst, float bassDb, float trebleDb) {
  const uint32_t seq = dst.seq.load(std::memory_order_relaxed);
  dst.seq.store(seq + 1, std::memory_order_release);
  dst.bass = MakeLowShelf(bassDb, kBassHz, static_cast<float>(kSampleRate));
  dst.treble = MakeHighShelf(trebleDb, kTrebleHz, static_cast<float>(kSampleRate));
  dst.seq.store(seq + 2, std::memory_order_release);
}

struct AsioSamples {
  unsigned long hi;
  unsigned long lo;
};
struct AsioTimeStamp {
  unsigned long hi;
  unsigned long lo;
};
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
  void (*bufferSwitch)(long doubleBufferIndex, long directProcess);
  void (*sampleRateDidChange)(double rate);
  long (*asioMessage)(long selector, long value, void* message, double* opt);
  AsioTime* (*bufferSwitchTimeInfo)(AsioTime* params, long doubleBufferIndex, long directProcess);
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

class LinkClient;

LinkClient* gLink = nullptr;

void LinkBufferSwitch(long index, long);
void LinkRateChanged(double);
long LinkMessage(long selector, long value, void*, double*);
AsioTime* LinkBufferTime(AsioTime* params, long index, long);

void WriteAsioSample(void* buffer, long type, long index, float value) {
  if (buffer == nullptr) {
    return;
  }
  if (value > 1.f) {
    value = 1.f;
  }
  if (value < -1.f) {
    value = -1.f;
  }
  if (type == 19) {
    static_cast<float*>(buffer)[index] = value;
  } else if (type == 16) {
    static_cast<int16_t*>(buffer)[index] = static_cast<int16_t>(value * 32767.f);
  } else {
    static_cast<int32_t*>(buffer)[index] = static_cast<int32_t>(value * 2147483647.f);
  }
}

class LinkClient {
 public:
  LinkClient() { InitializeCriticalSection(&lock_); }
  ~LinkClient() {
    Stop();
    DeleteCriticalSection(&lock_);
  }

  void Start(std::atomic<bool>& running, wchar_t* note) {
    if (thread_.joinable()) {
      return;
    }
    stop_.store(false, std::memory_order_relaxed);
    thread_ = std::thread(&LinkClient::ThreadMain, this, &running, note);
  }

  void Stop() {
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) {
      thread_.join();
    }
    stop_.store(false, std::memory_order_relaxed);
  }

  bool Live() const { return live_.load(std::memory_order_acquire) != 0; }

  void CopyStatus(wchar_t* text, int capacity, int& fault) const {
    if (text != nullptr && capacity > 0) {
      const int slot = statusSlot_.load(std::memory_order_acquire);
      CopyText(text, capacity, statusText_[slot]);
    }
    fault = statusFault_.load(std::memory_order_relaxed);
  }

  void Push(int pair, const float* interleaved, uint32_t frames) {
    if (!Live() || interleaved == nullptr || frames == 0 || pair < 0 || pair >= kVirtualBuses) {
      return;
    }
    EnterCriticalSection(&lock_);
    rings_[pair].Push(interleaved, frames);
    LeaveCriticalSection(&lock_);
  }

  void OnBuffer(long index) {
    if (index != 0 && index != 1) {
      return;
    }
    EnterCriticalSection(&lock_);
    float dump[512 * 2];
    float left[kVirtualBuses]{};
    float right[kVirtualBuses]{};
    for (int pair = 0; pair < kVirtualBuses; ++pair) {
      while (rings_[pair].Available() > 2048) {
        uint32_t extra = rings_[pair].Available() - 1024;
        if (extra > 512) {
          extra = 512;
        }
        rings_[pair].Pop(dump, extra);
      }
    }
    for (long i = 0; i < bufferFrames_; ++i) {
      for (int pair = 0; pair < kVirtualBuses; ++pair) {
        left[pair] = 0.f;
        right[pair] = 0.f;
        if (rings_[pair].Available() > 0) {
          float sample[2];
          rings_[pair].Pop(sample, 1);
          left[pair] = sample[0];
          right[pair] = sample[1];
        }
      }
      for (int ch = 0; ch < outputCount_ && ch < 16; ++ch) {
        float value = 0.f;
        if (ch < kVirtualBuses * 2) {
          value = (ch % 2 == 0) ? left[ch / 2] : right[ch / 2];
        }
        WriteAsioSample(infos_[outBase_ + ch].buffers[index], outType_[ch], i, value);
      }
    }
    LeaveCriticalSection(&lock_);
    if (readyOk_ && driver_ != nullptr) {
      if (driver_->lpVtbl->outputReady(driver_) != 0) {
        readyOk_ = false;
      }
    }
  }

 private:
  void Publish(const wchar_t* text, int fault, int hz) {
    const int next = 1 - statusSlot_.load(std::memory_order_relaxed);
    CopyText(statusText_[next], 160, text);
    statusHz_.store(hz, std::memory_order_relaxed);
    statusFault_.store(fault, std::memory_order_relaxed);
    statusSlot_.store(next, std::memory_order_release);
  }

  void RefreshStatus() {
    double rate = 0.0;
    int hz = 0;
    if (driver_ != nullptr && driver_->lpVtbl->getSampleRate(driver_, &rate) == 0) {
      hz = static_cast<int>(rate + 0.5);
    }
    wchar_t text[160];
    int fault = 0;
    if (hz == kSampleRate) {
      CopyText(text, 160, L"ASIO Link Pro OK");
    } else if (hz > 0) {
      wsprintfW(text, L"ASIO Link Pro sample rate mismatch (%d Hz)", hz);
      fault = 1;
    } else {
      CopyText(text, 160, L"ASIO Link Pro offline");
      fault = 2;
    }
    Publish(text, fault, hz);
  }

  void Fail(wchar_t* note, const wchar_t* text) {
    if (note != nullptr) {
      CopyText(note, 160, text);
    }
    Publish(text != nullptr ? text : L"ASIO Link Pro offline", 2, 0);
  }

  void CloseDriver() {
    live_.store(0, std::memory_order_release);
    if (gLink == this) {
      gLink = nullptr;
    }
    if (driver_ != nullptr) {
      driver_->lpVtbl->stop(driver_);
      driver_->lpVtbl->disposeBuffers(driver_);
      driver_->lpVtbl->Release(driver_);
      driver_ = nullptr;
    }
    bufferFrames_ = 0;
    outputCount_ = 0;
  }

  bool Open(wchar_t* note) {
    const GUID id = {0xF1442B0B, 0x6489, 0x449B, {0x9F, 0x32, 0xD2, 0x6C, 0x52, 0xF0, 0xC4, 0x91}};
    const HRESULT hr =
        CoCreateInstance(id, nullptr, CLSCTX_INPROC_SERVER, id, reinterpret_cast<void**>(&driver_));
    if (FAILED(hr) || driver_ == nullptr) {
      driver_ = nullptr;
      Fail(note, L"ASIO Link Pro est introuvable.");
      return false;
    }
    if (driver_->lpVtbl->init(driver_, GetDesktopWindow()) == 0) {
      char error[128] = {};
      driver_->lpVtbl->getErrorMessage(driver_, error);
      Fail(note, L"ASIO Link Pro n'a pas démarré. Laisse Start ASIO allumé.");
      driver_->lpVtbl->Release(driver_);
      driver_ = nullptr;
      return false;
    }
    long inputs = 0;
    long outputs = 0;
    if (driver_->lpVtbl->getChannels(driver_, &inputs, &outputs) != 0 || outputs < 10) {
      Fail(note, L"ASIO Link Pro n'a pas 10 sorties. Laisse le câble VB en 16 canaux.");
      driver_->lpVtbl->Release(driver_);
      driver_ = nullptr;
      return false;
    }
    long minimum = 0;
    long maximum = 0;
    long preferred = 512;
    long granularity = 0;
    driver_->lpVtbl->getBufferSize(driver_, &minimum, &maximum, &preferred, &granularity);
    if (preferred < 32 || preferred > 4096) {
      preferred = 512;
    }
    callbacks_.bufferSwitch = &LinkBufferSwitch;
    callbacks_.sampleRateDidChange = &LinkRateChanged;
    callbacks_.asioMessage = &LinkMessage;
    callbacks_.bufferSwitchTimeInfo = &LinkBufferTime;
    gLink = this;
    const int wantOut = outputs >= 16 ? 16 : static_cast<int>(outputs);
    const int wantIn = inputs >= wantOut ? wantOut : static_cast<int>(inputs);
    long created = -1;
    AsioBufferInfo trial[32]{};
    const int plans[2][2] = {{0, wantOut}, {wantIn, wantOut}};
    for (int plan = 0; plan < 2 && created != 0; ++plan) {
      const int nIn = plans[plan][0];
      const int nOut = plans[plan][1];
      int count = 0;
      std::memset(trial, 0, sizeof(trial));
      for (int i = 0; i < nIn; ++i, ++count) {
        trial[count].isInput = 1;
        trial[count].channelNum = i;
      }
      for (int i = 0; i < nOut; ++i, ++count) {
        trial[count].isInput = 0;
        trial[count].channelNum = i;
      }
      created = driver_->lpVtbl->createBuffers(driver_, trial, count, preferred, &callbacks_);
      if (created == 0) {
        std::memcpy(infos_, trial, sizeof(AsioBufferInfo) * static_cast<size_t>(count));
        outBase_ = nIn;
        outputCount_ = nOut;
        break;
      }
    }
    if (created != 0) {
      wchar_t text[160];
      wsprintfW(text, L"ASIO Link Pro a refusé les buffers (%ld).", created);
      Fail(note, text);
      gLink = nullptr;
      driver_->lpVtbl->Release(driver_);
      driver_ = nullptr;
      return false;
    }
    bufferFrames_ = preferred;
    for (int i = 0; i < outputCount_ && i < 16; ++i) {
      AsioChannelInfo info{};
      info.channel = i;
      info.isInput = 0;
      outType_[i] = 18;
      if (driver_->lpVtbl->getChannelInfo(driver_, &info) == 0) {
        outType_[i] = info.type;
      }
    }
    readyOk_ = true;
    if (driver_->lpVtbl->start(driver_) != 0) {
      Fail(note, L"ASIO Link Pro n'a pas accepté B1 à B5.");
      driver_->lpVtbl->disposeBuffers(driver_);
      driver_->lpVtbl->Release(driver_);
      driver_ = nullptr;
      gLink = nullptr;
      return false;
    }
    if (note != nullptr) {
      note[0] = 0;
    }
    live_.store(1, std::memory_order_release);
    RefreshStatus();
    return true;
  }

  void ThreadMain(std::atomic<bool>* running, wchar_t* note) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool owned = SUCCEEDED(com);
    while (!stop_.load(std::memory_order_relaxed) && running->load(std::memory_order_relaxed)) {
      if (Open(note)) {
        int ticks = 0;
        while (!stop_.load(std::memory_order_relaxed) && running->load(std::memory_order_relaxed)) {
          MSG msg;
          while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
          }
          if (++ticks >= 400) {
            ticks = 0;
            RefreshStatus();
          }
          Sleep(5);
        }
        CloseDriver();
        break;
      }
      for (int i = 0; i < 40 && !stop_.load(std::memory_order_relaxed) &&
                      running->load(std::memory_order_relaxed);
           ++i) {
        Sleep(50);
      }
    }
    if (owned) {
      CoUninitialize();
    }
  }

  CRITICAL_SECTION lock_{};
  FloatRing rings_[kVirtualBuses]{};
  std::thread thread_{};
  std::atomic<bool> stop_{false};
  std::atomic<int> live_{0};
  wchar_t statusText_[2][160]{};
  std::atomic<int> statusSlot_{0};
  std::atomic<int> statusFault_{2};
  std::atomic<int> statusHz_{0};
  AsioDriver* driver_ = nullptr;
  AsioCallbacks callbacks_{};
  AsioBufferInfo infos_[32]{};
  long outType_[16]{};
  long bufferFrames_ = 0;
  int outputCount_ = 0;
  int outBase_ = 0;
  bool readyOk_ = false;
};

void LinkBufferSwitch(long index, long) {
  if (gLink != nullptr) {
    gLink->OnBuffer(index);
  }
}

void LinkRateChanged(double) {}

long LinkMessage(long selector, long value, void*, double*) {
  if (selector == 1) {
    return value == 2 || value == 3 || value == 4 || value == 5 || value == 6 || value == 7 ? 1 : 0;
  }
  if (selector == 2) {
    return 2;
  }
  if (selector == 7) {
    return 1;
  }
  return 0;
}

AsioTime* LinkBufferTime(AsioTime* params, long index, long) {
  LinkBufferSwitch(index, 1);
  return params;
}

}  // namespace

struct Engine::Impl {
  VstRack vst;
  std::atomic<bool> running{false};
  std::thread thread;
  std::mutex configLock;
  std::atomic<uint32_t> configSeq{1};
  uint32_t appliedSeq = 0;
  wchar_t captureId[kMicStrips][512]{};
  wchar_t renderId[kPhysicalBuses][512]{};

  std::atomic<float> gain[kStrips];
  std::atomic<float> busGain[kBuses];
  std::atomic<bool> busMute[kBuses];
  std::atomic<bool> busMono[kBuses];
  std::atomic<bool> busSolo[kPhysicalBuses];
  std::atomic<float> bassDb[kStrips];
  std::atomic<float> trebleDb[kStrips];
  std::atomic<bool> mute[kStrips];
  std::atomic<bool> solo[kStrips];
  std::atomic<bool> mono[kStrips];
  std::atomic<float> pan[kStrips];
  std::atomic<bool> insertOn[kStrips];
  std::atomic<bool> gateOn[kStrips];
  std::atomic<float> gateDepth[kStrips];
  std::atomic<float> gateLevel[kMicStrips];
  std::atomic<bool> gateOpen[kMicStrips];
  std::atomic<uint16_t> route[kStrips];
  PublishedEq eq[kStrips];
  std::atomic<float> peakL[kStrips];
  std::atomic<float> peakR[kStrips];
  std::atomic<float> busPeakL[kBuses];
  std::atomic<float> busPeakR[kBuses];
  std::atomic<uint64_t> framesCaptured{0};
  std::atomic<uint64_t> framesRendered{0};
  std::atomic<uint64_t> blocks{0};
  std::atomic<uint64_t> renderUnderruns{0};
  std::atomic<uint64_t> captureUnderruns{0};
  std::atomic<uint64_t> driverReads{0};
  std::atomic<uint64_t> driverReportedFrames{0};
  std::atomic<uint64_t> driverPaddedFrames{0};
  std::atomic<uint64_t> driverEmptyReads{0};
  std::atomic<uint64_t> virtualUnderrunFrames{0};
  std::atomic<uint64_t> overrunFrames{0};
  std::atomic<uint64_t> nanSamples{0};
  std::atomic<uint32_t> ringFill{0};
  std::atomic<uint32_t> ringMin{0xffffffffu};
  std::atomic<uint32_t> ringMax{0};
  std::atomic<uint32_t> wasapiPadding{0};
  std::atomic<uint32_t> wasapiBuffer{0};
  std::atomic<uint32_t> renderRate{0};
  std::atomic<uint32_t> renderChannels{0};
  std::atomic<uint32_t> renderBits{0};
  std::atomic<uint32_t> captureRate{0};
  std::atomic<uint32_t> captureChannels{0};
  std::atomic<uint32_t> captureBits{0};
  std::atomic<uint64_t> processUsMax{0};
  std::atomic<uint64_t> processUsSum{0};
  std::atomic<uint64_t> intervalUsMax{0};
  std::atomic<uint64_t> intervalUsSum{0};
  std::atomic<uint64_t> intervalCount{0};
  std::atomic<uint32_t> driverReturnMin{0xffffffffu};
  std::atomic<uint32_t> driverReturnMax{0};
  std::atomic<uint64_t> shortReads{0};
  std::atomic<uint64_t> popRequested{0};
  std::atomic<uint64_t> popActual{0};
  std::atomic<uint64_t> fillBeforeSum{0};
  std::atomic<uint64_t> fillAfterSum{0};
  std::atomic<uint64_t> silenceFrames{0};
  std::atomic<uint64_t> virtualRingEmptyCount{0};
  std::atomic<uint64_t> renderStarvationCount{0};
  std::atomic<uint64_t> renderSilenceFrames{0};
  std::atomic<uint64_t> wasapiZeroCount{0};
  std::atomic<uint32_t> firstZero{0};
  std::atomic<uint32_t> prebufferFrames{0};
  std::atomic<uint64_t> activeFillSum{0};
  std::atomic<uint64_t> activeFillCount{0};
  std::atomic<uint64_t> renderSpaceSum{0};
  std::atomic<uint64_t> renderSpaceCount{0};
  std::atomic<uint64_t> intervalUsMin{0xffffffffffffffffull};
  std::atomic<uint64_t> missedDeadlines{0};
  std::atomic<int> audioPriority{0};
  std::atomic<int> mmcssOn{0};
  struct CycleTrace {
    uint64_t qpc;
    uint16_t wasapiFree;
    uint16_t written[2];
    uint16_t pushed[5];
    uint16_t popped[5];
    uint16_t before[5];
    uint16_t after[5];
    uint8_t reads[5];
    uint16_t asked;
  };
  static constexpr uint32_t kTraceCap = 65536;
  CycleTrace* trace = nullptr;
  std::atomic<uint32_t> traceWrite{0};
  struct MixSync {
    uint64_t qpcGame;
    uint64_t qpcMusic;
    uint32_t availGame;
    uint32_t availMusic;
    uint32_t consumedGame;
    uint32_t consumedMusic;
    uint32_t fillBeforeGame;
    uint32_t fillBeforeMusic;
    uint32_t fillAfterGame;
    uint32_t fillAfterMusic;
    uint64_t blockGame;
    uint64_t blockMusic;
    uint32_t asked;
  };
  MixSync* mixSync = nullptr;
  std::atomic<uint32_t> mixSyncWrite{0};
  struct ProdEvent {
    uint32_t stream;
    uint64_t qpcIoctl;
    uint64_t qpcWrite;
    uint32_t framesWritten;
    uint32_t driverFrames;
    uint32_t fillBefore;
    uint32_t fillAfter;
    uint64_t packet;
    uint32_t driverReserved;
  };
  ProdEvent* prod = nullptr;
  std::atomic<uint32_t> prodWrite{0};
  std::thread diagThread;

  static constexpr uint32_t kRecCapacity = 1u << 20;
  static constexpr uint32_t kRecMask = kRecCapacity - 1;

  std::atomic<bool> recording{false};
  std::atomic<int> recordBusy{0};
  std::atomic<int> recordBus{5};
  std::atomic<uint32_t> monitorMask{0};
  std::atomic<uint32_t> playbackMask{0};
  FloatRing playRing;
  SRWLOCK playLock = SRWLOCK_INIT;
  HANDLE playSpace = nullptr;
  std::wstring recordPath;
  HANDLE recordFile = INVALID_HANDLE_VALUE;
  uint32_t recordBytes = 0;
  std::unique_ptr<int16_t[]> recordRing;
  std::atomic<uint32_t> recordWrite{0};
  std::atomic<uint32_t> recordRead{0};
  HANDLE recordEvent = nullptr;
  std::atomic<bool> recordWriterRun{false};
  std::thread recordThread;

  std::wstring deviceError;
  wchar_t cableNote[160]{};
  wchar_t linkNote[160]{};
  std::atomic<int> vadOpen{0};
  int cableRetry = 0;
  LinkClient link;
  HANDLE driver = INVALID_HANDLE_VALUE;
  int driverRetry = 0;
  HANDLE asioMap = nullptr;
  AsioBridge* asio = nullptr;
  HANDLE asioTick = nullptr;
  static constexpr int kAsioReturnCap = kBlockFrames * 4;
  float asioPending[kStrips][kBlockFrames * kChannels]{};
  int asioFill = 0;
  float asioReturn[kStrips][kAsioReturnCap * kChannels]{};
  int asioReturnRead = 0;
  int asioReturnCount = 0;
  uint32_t asioReturnSeq = 0;
  uint32_t asioLatchedMask = 0;
  bool asioPrimed = false;
  int asioFade = 0;

  explicit Impl() {
    for (int i = 0; i < kStrips; ++i) {
      gain[i].store(1.f);
      bassDb[i].store(0.f);
      trebleDb[i].store(0.f);
      mute[i].store(false);
      solo[i].store(false);
      mono[i].store(false);
      pan[i].store(0.f);
      insertOn[i].store(false);
      gateOn[i].store(false);
      gateDepth[i].store(1.f);
      route[i].store(i <= kMic1 ? 0x1 : 0);
      peakL[i].store(0.f);
      peakR[i].store(0.f);
      PublishEq(eq[i], 0.f, 0.f);
    }
    for (int i = 0; i < kBuses; ++i) {
      busPeakL[i].store(0.f);
      busPeakR[i].store(0.f);
    }
    for (int i = 0; i < kBuses; ++i) {
      busGain[i].store(1.f);
      busMute[i].store(false);
      busMono[i].store(false);
    }
    for (int i = 0; i < kPhysicalBuses; ++i) {
      busSolo[i].store(false);
    }
    for (int i = 0; i < kMicStrips; ++i) {
      gateLevel[i].store(0.f);
      gateOpen[i].store(false);
    }
    playSpace = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  }

  ~Impl() {
    if (playSpace != nullptr) {
      CloseHandle(playSpace);
      playSpace = nullptr;
    }
  }

  void MixRouted(float* const buses[kBuses], int frames, PeakLimit* limits) {
    const int samples = frames * kChannels;
    const int source = recordBus.load(std::memory_order_relaxed);
    const uint32_t mirror = monitorMask.load(std::memory_order_relaxed);
    if (recording.load(std::memory_order_relaxed) && mirror != 0 && source >= 0 && source < kBuses) {
      bool anySolo = false;
      for (int bus = 0; bus < kPhysicalBuses; ++bus) {
        if (busSolo[bus].load(std::memory_order_relaxed)) {
          anySolo = true;
        }
      }
      const float* from = buses[source];
      for (int bus = 0; bus < kBuses; ++bus) {
        if ((mirror & (1u << bus)) == 0 || bus == source) {
          continue;
        }
        const bool soloCut = bus < kPhysicalBuses && anySolo && !busSolo[bus].load(std::memory_order_relaxed);
        if (busMute[bus].load(std::memory_order_relaxed) || soloCut) {
          continue;
        }
        float* dest = buses[bus];
        for (int i = 0; i < samples; ++i) {
          dest[i] += from[i];
        }
        LimitStereo(dest, frames, limits[bus]);
      }
    }
    float extra[kBlockFrames * 2];
    AcquireSRWLockExclusive(&playLock);
    playRing.Pop(extra, static_cast<uint32_t>(frames));
    ReleaseSRWLockExclusive(&playLock);
    if (playSpace != nullptr) {
      SetEvent(playSpace);
    }
    const uint32_t play = playbackMask.load(std::memory_order_relaxed);
    if (play != 0) {
      bool anySolo = false;
      for (int bus = 0; bus < kPhysicalBuses; ++bus) {
        if (busSolo[bus].load(std::memory_order_relaxed)) {
          anySolo = true;
        }
      }
      for (int bus = 0; bus < kBuses; ++bus) {
        if ((play & (1u << bus)) == 0) {
          continue;
        }
        const bool soloCut = bus < kPhysicalBuses && anySolo && !busSolo[bus].load(std::memory_order_relaxed);
        if (busMute[bus].load(std::memory_order_relaxed) || soloCut) {
          continue;
        }
        float* dest = buses[bus];
        for (int i = 0; i < samples; ++i) {
          dest[i] += extra[i];
        }
        LimitStereo(dest, frames, limits[bus]);
      }
    }
  }

  void EnsureAsio() {
    if (asio != nullptr) {
      return;
    }
    asioMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                 static_cast<DWORD>(sizeof(AsioBridge)), kAsioMappingName);
    if (asioMap == nullptr) {
      return;
    }
    void* view = MapViewOfFile(asioMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AsioBridge));
    asio = static_cast<AsioBridge*>(view);
    if (asio == nullptr) {
      CloseHandle(asioMap);
      asioMap = nullptr;
      return;
    }
    if (asio->magic != kAsioMagic || asio->version != kAsioVersion || asio->channels != kAsioChannels) {
      std::memset(asio, 0, sizeof(AsioBridge));
      asio->magic = kAsioMagic;
      asio->version = kAsioVersion;
      asio->bufferFrames = kBlockFrames;
      asio->channels = kAsioChannels;
    }
    if (asioTick == nullptr) {
      asioTick = CreateEventW(nullptr, FALSE, FALSE, kAsioTickName);
    }
  }

  void CloseAsio() {
    if (asio != nullptr) {
      UnmapViewOfFile(asio);
      asio = nullptr;
    }
    if (asioMap != nullptr) {
      CloseHandle(asioMap);
      asioMap = nullptr;
    }
    if (asioTick != nullptr) {
      CloseHandle(asioTick);
      asioTick = nullptr;
    }
  }

  void PollDriver() {
    if (driver != INVALID_HANDLE_VALUE) {
      return;
    }
    if (driverRetry > 0) {
      --driverRetry;
      return;
    }
    driver = CreateFileW(kDriverDevicePath, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (driver == INVALID_HANDLE_VALUE) {
      driverRetry = 50;
    }
  }

  void ReadVirtualInputs(float* strips[kStrips]) {
    PollDriver();
    if (driver == INVALID_HANDLE_VALUE) {
      for (int i = 0; i < kVirtualStrips; ++i) {
        std::memset(strips[i], 0, sizeof(float) * kBlockFrames * kChannels);
      }
      return;
    }
    for (int stream = 0; stream < kVirtualStrips; ++stream) {
      DriverBufferRequest request{};
      request.magic = kDriverMagic;
      request.stream = static_cast<uint32_t>(stream);
      request.frames = kBlockFrames;
      DWORD got = 0;
      const BOOL ok = DeviceIoControl(driver, kIoctlReadPlayback, &request, sizeof(request),
                                      &request, sizeof(request), &got, nullptr);
      if (!ok || got < 16) {
        std::memset(strips[stream], 0, sizeof(float) * kBlockFrames * kChannels);
        continue;
      }
      std::memcpy(strips[stream], request.samples, sizeof(float) * kBlockFrames * kChannels);
    }
  }

  void WriteVirtualOutputs(float* const buses[kBuses]) {
    if (driver == INVALID_HANDLE_VALUE) {
      return;
    }
    for (int bus = 0; bus < kVirtualBuses; ++bus) {
      DriverBufferRequest request{};
      request.magic = kDriverMagic;
      request.stream = static_cast<uint32_t>(bus);
      request.frames = kBlockFrames;
      std::memcpy(request.samples, buses[kPhysicalBuses + bus],
                  sizeof(float) * kBlockFrames * kChannels);
      DWORD got = 0;
      if (!DeviceIoControl(driver, kIoctlWriteCapture, &request, sizeof(request), nullptr, 0, &got,
                           nullptr)) {
        CloseHandle(driver);
        driver = INVALID_HANDLE_VALUE;
        driverRetry = 50;
        return;
      }
    }
  }

  void PublishAsioBlock() {
    const uint32_t slot = asio->sendSeq & static_cast<uint32_t>(kAsioBufferSlots - 1);
    float* dst = asio->send[slot];
    for (int frame = 0; frame < kBlockFrames; ++frame) {
      for (int strip = 0; strip < kStrips; ++strip) {
        dst[frame * kAsioChannels + strip * 2] = asioPending[strip][frame * 2];
        dst[frame * kAsioChannels + strip * 2 + 1] = asioPending[strip][frame * 2 + 1];
      }
    }
    asioFill = 0;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&asio->sendSeq));
    if (asioTick != nullptr) {
      SetEvent(asioTick);
    }
  }

  void ExchangeAsio(float* shaped[kStrips], int frames) {
    EnsureAsio();
    if (asio == nullptr || frames <= 0) {
      return;
    }
    // The speaker quantum is often shorter than 512. Cantabile only hears a
    // block when one is actually published, so partial quanta have to accumulate.
    int offset = 0;
    while (offset < frames) {
      const int room = kBlockFrames - asioFill;
      const int count = std::min(room, frames - offset);
      for (int strip = 0; strip < kStrips; ++strip) {
        std::memcpy(asioPending[strip] + asioFill * kChannels, shaped[strip] + offset * kChannels,
                    sizeof(float) * static_cast<size_t>(count) * kChannels);
      }
      asioFill += count;
      offset += count;
      if (asioFill == kBlockFrames) {
        PublishAsioBlock();
      }
    }

    if (asio->hostRunning == 0) {
      asioReturnCount = 0;
      asioPrimed = false;
      asioFade = 0;
      return;
    }
    const uint32_t mask = asio->outputMask;
    if (mask != asioLatchedMask) {
      asioLatchedMask = mask;
      asioReturnCount = 0;
      asioReturnRead = 0;
      asioPrimed = false;
      asioFade = 0;
    }
    const uint32_t returned = asio->returnSeq;
    if (mask != 0 && returned != 0 && returned != asioReturnSeq) {
      uint32_t from = asioReturnSeq;
      const uint32_t behind = returned - from;
      if (from == 0 || behind > static_cast<uint32_t>(kAsioBufferSlots)) {
        from = returned - 1;
      }
      while (from != returned) {
        ++from;
        const float* src = asio->ret[(from - 1) & static_cast<uint32_t>(kAsioBufferSlots - 1)];
        if (asioReturnCount + kBlockFrames > kAsioReturnCap) {
          const int drop = asioReturnCount + kBlockFrames - kAsioReturnCap;
          asioReturnRead = (asioReturnRead + drop) % kAsioReturnCap;
          asioReturnCount -= drop;
        }
        for (int frame = 0; frame < kBlockFrames; ++frame) {
          const int at = (asioReturnRead + asioReturnCount) % kAsioReturnCap;
          for (int strip = 0; strip < kStrips; ++strip) {
            asioReturn[strip][at * kChannels] = src[frame * kAsioChannels + strip * 2];
            asioReturn[strip][at * kChannels + 1] = src[frame * kAsioChannels + strip * 2 + 1];
          }
          ++asioReturnCount;
        }
      }
      asioReturnSeq = returned;
    }
    // Two blocks of slack absorb a slow VST callback. Until that slack exists,
    // the dry strip keeps playing instead of freezing on one sample.
    constexpr int kAsioCushion = kBlockFrames * 2;
    if (!asioPrimed) {
      if (asioReturnCount < kAsioCushion) {
        return;
      }
      asioPrimed = true;
    }
    if (mask == 0 || asioReturnSeq == 0) {
      return;
    }
    for (int frame = 0; frame < frames; ++frame) {
      const bool have = asioReturnCount > 0;
      const float mix = asioFade < 64 ? static_cast<float>(asioFade) / 64.f : 1.f;
      for (int strip = 0; strip < kStrips; ++strip) {
        const uint32_t pair = (1u << (strip * 2)) | (1u << (strip * 2 + 1));
        if ((mask & pair) != pair || !have || !insertOn[strip].load(std::memory_order_relaxed)) {
          continue;
        }
        const int at = asioReturnRead;
        const float wetL = asioReturn[strip][at * kChannels];
        const float wetR = asioReturn[strip][at * kChannels + 1];
        const float dryL = shaped[strip][frame * kChannels];
        const float dryR = shaped[strip][frame * kChannels + 1];
        shaped[strip][frame * kChannels] = dryL + (wetL - dryL) * mix;
        shaped[strip][frame * kChannels + 1] = dryR + (wetR - dryR) * mix;
      }
      if (have) {
        asioReturnRead = (asioReturnRead + 1) % kAsioReturnCap;
        --asioReturnCount;
        if (asioFade < 64) {
          ++asioFade;
        }
      }
    }
  }

  bool OpenRecorder(const wchar_t* folder, std::wstring& error) {
    wchar_t path[MAX_PATH] = {};
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wsprintfW(path, L"%s\\AnyMix-%04u%02u%02u-%02u%02u%02u.wav", folder, time.wYear, time.wMonth,
              time.wDay, time.wHour, time.wMinute, time.wSecond);
    recordFile = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (recordFile == INVALID_HANDLE_VALUE) {
      error = L"Cannot create the WAV file in that folder.";
      return false;
    }
    recordPath = path;
    recordBytes = 0;
    uint8_t header[44] = {};
    DWORD written = 0;
    if (!WriteFile(recordFile, header, 44, &written, nullptr) || written != 44) {
      CloseHandle(recordFile);
      recordFile = INVALID_HANDLE_VALUE;
      error = L"Cannot write the WAV header.";
      return false;
    }
    return true;
  }

  void FinishRecorder() {
    StopRecordWriter();
    if (recordFile == INVALID_HANDLE_VALUE) {
      return;
    }
    const uint32_t dataBytes = recordBytes;
    const uint32_t riffSize = 36 + dataBytes;
    uint8_t header[44] = {};
    std::memcpy(header, "RIFF", 4);
    std::memcpy(header + 4, &riffSize, 4);
    std::memcpy(header + 8, "WAVE", 4);
    std::memcpy(header + 12, "fmt ", 4);
    const uint32_t fmtSize = 16;
    const uint16_t pcm = 1;
    const uint16_t channels = 2;
    const uint32_t rate = kSampleRate;
    const uint16_t bits = 16;
    const uint16_t align = 4;
    const uint32_t byteRate = rate * align;
    std::memcpy(header + 16, &fmtSize, 4);
    std::memcpy(header + 20, &pcm, 2);
    std::memcpy(header + 22, &channels, 2);
    std::memcpy(header + 24, &rate, 4);
    std::memcpy(header + 28, &byteRate, 4);
    std::memcpy(header + 32, &align, 2);
    std::memcpy(header + 34, &bits, 2);
    std::memcpy(header + 36, "data", 4);
    std::memcpy(header + 40, &dataBytes, 4);
    SetFilePointer(recordFile, 0, nullptr, FILE_BEGIN);
    DWORD written = 0;
    WriteFile(recordFile, header, 44, &written, nullptr);
    FlushFileBuffers(recordFile);
    CloseHandle(recordFile);
    recordFile = INVALID_HANDLE_VALUE;
  }

  void RecordWriter() {
    int16_t chunk[8192];
    while (recordWriterRun.load(std::memory_order_acquire) ||
           recordRead.load(std::memory_order_acquire) != recordWrite.load(std::memory_order_acquire)) {
      const uint32_t read = recordRead.load(std::memory_order_relaxed);
      const uint32_t write = recordWrite.load(std::memory_order_acquire);
      const uint32_t available = write - read;
      if (available == 0 || recordFile == INVALID_HANDLE_VALUE) {
        if (recordEvent != nullptr) {
          WaitForSingleObject(recordEvent, 40);
        } else {
          Sleep(2);
        }
        continue;
      }
      const uint32_t count = std::min(available, static_cast<uint32_t>(8192));
      for (uint32_t i = 0; i < count; ++i) {
        chunk[i] = recordRing[(read + i) & kRecMask];
      }
      DWORD written = 0;
      const DWORD bytes = count * static_cast<DWORD>(sizeof(int16_t));
      if (WriteFile(recordFile, chunk, bytes, &written, nullptr) && written == bytes) {
        recordBytes += written;
        recordRead.store(read + count, std::memory_order_release);
      } else if (!recordWriterRun.load(std::memory_order_acquire)) {
        recordRead.store(recordWrite.load(std::memory_order_acquire), std::memory_order_release);
      } else {
        Sleep(2);
      }
    }
  }

  void StopRecordWriter() {
    recordWriterRun.store(false, std::memory_order_release);
    if (recordEvent != nullptr) {
      SetEvent(recordEvent);
    }
    if (recordThread.joinable()) {
      recordThread.join();
    }
  }

  void WriteRecordBlock(const float* interleaved, int frames) {
    recordBusy.fetch_add(1, std::memory_order_acq_rel);
    if (recording.load(std::memory_order_acquire) && recordRing != nullptr && frames > 0 &&
        frames <= kBlockFrames) {
      const uint32_t samples = static_cast<uint32_t>(frames * kChannels);
      const uint32_t write = recordWrite.load(std::memory_order_relaxed);
      const uint32_t read = recordRead.load(std::memory_order_acquire);
      if (write - read + samples < kRecCapacity) {
        for (uint32_t i = 0; i < samples; ++i) {
          const float clamped = std::max(-1.f, std::min(1.f, interleaved[i]));
          recordRing[(write + i) & kRecMask] = static_cast<int16_t>(clamped * 32767.f);
        }
        recordWrite.store(write + samples, std::memory_order_release);
        if (recordEvent != nullptr) {
          SetEvent(recordEvent);
        }
      }
    }
    recordBusy.fetch_sub(1, std::memory_order_acq_rel);
  }

  void DrainCapture(WasapiEnd& end, FloatRing& ring, bool& underrun) {
    if (!end.open || end.capture == nullptr) {
      return;
    }
    UINT32 packet = 0;
    while (SUCCEEDED(end.capture->GetNextPacketSize(&packet)) && packet > 0) {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      if (FAILED(end.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
        break;
      }
      float pair[2];
      const BYTE* bytes = reinterpret_cast<const BYTE*>(data);
      for (UINT32 i = 0; i < frames; ++i) {
        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
          pair[0] = 0.f;
          pair[1] = 0.f;
        } else {
          DecodeFrame(end, bytes + static_cast<size_t>(i) * end.blockAlign, pair[0], pair[1]);
        }
        ring.Push(pair, 1);
        framesCaptured.fetch_add(1, std::memory_order_relaxed);
      }
      end.capture->ReleaseBuffer(frames);
      underrun = false;
    }
  }

  UINT32 DrainRender(WasapiEnd& end, FloatRing& ring) {
    UINT32 written = 0;
    if (!end.open || end.render == nullptr) {
      return 0;
    }
    UINT32 padding = 0;
    if (FAILED(end.client->GetCurrentPadding(&padding))) {
      return 0;
    }
    if (padding >= end.bufferFrames) {
      return 0;
    }
    UINT32 need = end.bufferFrames - padding;
    while (need > 0 && ring.Available() > 0) {
      const UINT32 chunk = std::min<UINT32>(need, ring.Available());
      BYTE* data = nullptr;
      if (FAILED(end.render->GetBuffer(chunk, &data)) || data == nullptr) {
        return written;
      }
      for (UINT32 i = 0; i < chunk; ++i) {
        float pair[2];
        ring.Pop(pair, 1);
        EncodeFrame(end, pair[0], pair[1], data + static_cast<size_t>(i) * end.blockAlign);
        framesRendered.fetch_add(1, std::memory_order_relaxed);
      }
      end.render->ReleaseBuffer(chunk, 0);
      written += chunk;
      need -= chunk;
    }
    return written;
  }

  void WriteDiagLoop() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash != nullptr) {
      wcscpy(slash + 1, L"audio_diagnostics.txt");
    }
    FILE* file = _wfopen(path, L"w");
    if (file == nullptr) {
      return;
    }
    std::fwprintf(file, L"AnyMix audio diagnostics\n");
    std::fwprintf(file, L"engine rate=%d channels=%d block=%d prebuffer=%u priority=%d mmcss=%d\n",
                  kSampleRate, kChannels, kBlockFrames, prebufferFrames.load(), audioPriority.load(),
                  mmcssOn.load());
    LARGE_INTEGER qpcFreq{};
    QueryPerformanceFrequency(&qpcFreq);
    wchar_t cyclePath[MAX_PATH] = {};
    wcscpy(cyclePath, path);
    wchar_t* cycleSlash = wcsrchr(cyclePath, L'\\');
    if (cycleSlash != nullptr) {
      wcscpy(cycleSlash + 1, L"audio_cycles.csv");
    }
    FILE* cycles = _wfopen(cyclePath, L"w");
    uint32_t dumped = 0;
    wchar_t syncPath[MAX_PATH] = {};
    wcscpy(syncPath, path);
    wchar_t* syncSlash = wcsrchr(syncPath, L'\\');
    if (syncSlash != nullptr) {
      wcscpy(syncSlash + 1, L"mix_sync.csv");
    }
    FILE* syncFile = _wfopen(syncPath, L"w");
    uint32_t syncDumped = 0;
    if (syncFile != nullptr) {
      std::fwprintf(syncFile,
                    L"qpcGame,qpcMusic,deltaUs,availGame,availMusic,consumedGame,consumedMusic,"
                    L"fillBeforeGame,fillBeforeMusic,fillAfterGame,fillAfterMusic,blockGame,blockMusic,asked\n");
    }
    wchar_t prodPath[MAX_PATH] = {};
    wcscpy(prodPath, path);
    wchar_t* prodSlash = wcsrchr(prodPath, L'\\');
    if (prodSlash != nullptr) {
      wcscpy(prodSlash + 1, L"mix_prod.csv");
    }
    FILE* prodFile = _wfopen(prodPath, L"w");
    uint32_t prodDumped = 0;
    if (prodFile != nullptr) {
      std::fwprintf(prodFile,
                    L"stream,qpcIoctl,qpcWrite,ioctlToWriteUs,framesWritten,driverFrames,fillBefore,fillAfter,"
                    L"packet,driverReserved\n");
    }
    if (cycles != nullptr) {
      std::fwprintf(cycles, L"qpc,us,free,asked,w0,w1");
      for (int s = 0; s < 5; ++s) {
        std::fwprintf(cycles, L",p%d,o%d,b%d,a%d,r%d", s, s, s, s, s);
      }
      std::fwprintf(cycles, L"\n");
    }
    uint64_t prevQpc = 0;
    while (running.load(std::memory_order_relaxed)) {
      const uint64_t intervals = intervalCount.load(std::memory_order_relaxed);
      const uint64_t blocksNow = blocks.load(std::memory_order_relaxed);
      const uint64_t avgInterval =
          intervals == 0 ? 0 : intervalUsSum.load(std::memory_order_relaxed) / intervals;
      const uint64_t avgProcess =
          blocksNow == 0 ? 0 : processUsSum.load(std::memory_order_relaxed) / blocksNow;
      std::fwprintf(
          file,
          L"t=%llu blocks=%llu intervalUs min=%llu avg=%llu max=%llu processUs avg=%llu max=%llu missed=%llu\n"
          L"wasapi buffer=%u padding=%u spaceAvg=%llu spaceN=%llu written=%llu renderUnderruns=%llu\n"
          L"driver reads=%llu reported=%llu min=%u max=%u avg=%llu padded=%llu empty=%llu short=%llu\n"
          L"pop req=%llu actual=%llu fillBeforeAvg=%llu fillAfterAvg=%llu silenceFrames=%llu "
          L"underrunFrames=%llu overrun=%llu ring=%u min=%u max=%u "
          L"ringEmpty=%llu starvation=%llu silenceOut=%llu wasapiZero=%llu firstZero=%u "
          L"activeFillAvg=%llu activeN=%llu capture=%llu micHolds=%llu\n",
          static_cast<unsigned long long>(GetTickCount64()),
          static_cast<unsigned long long>(blocksNow),
          static_cast<unsigned long long>(intervalUsMin.load()),
          static_cast<unsigned long long>(avgInterval),
          static_cast<unsigned long long>(intervalUsMax.load()),
          static_cast<unsigned long long>(avgProcess),
          static_cast<unsigned long long>(processUsMax.load()),
          static_cast<unsigned long long>(missedDeadlines.load()), wasapiBuffer.load(),
          wasapiPadding.load(),
          renderSpaceCount.load() == 0 ? 0ull : renderSpaceSum.load() / renderSpaceCount.load(),
          static_cast<unsigned long long>(renderSpaceCount.load()),
          static_cast<unsigned long long>(framesRendered.load()),
          static_cast<unsigned long long>(renderUnderruns.load()),
          static_cast<unsigned long long>(driverReads.load()),
          static_cast<unsigned long long>(driverReportedFrames.load()), driverReturnMin.load(),
          driverReturnMax.load(),
          driverReads.load() == 0 ? 0ull : driverReportedFrames.load() / driverReads.load(),
          static_cast<unsigned long long>(driverPaddedFrames.load()),
          static_cast<unsigned long long>(driverEmptyReads.load()),
          static_cast<unsigned long long>(shortReads.load()),
          static_cast<unsigned long long>(popRequested.load()),
          static_cast<unsigned long long>(popActual.load()),
          blocksNow == 0 ? 0ull : fillBeforeSum.load() / (blocksNow * 5ull),
          blocksNow == 0 ? 0ull : fillAfterSum.load() / (blocksNow * 5ull),
          static_cast<unsigned long long>(silenceFrames.load()),
          static_cast<unsigned long long>(virtualUnderrunFrames.load()),
          static_cast<unsigned long long>(overrunFrames.load()), ringFill.load(), ringMin.load(),
          ringMax.load(), static_cast<unsigned long long>(virtualRingEmptyCount.load()),
          static_cast<unsigned long long>(renderStarvationCount.load()),
          static_cast<unsigned long long>(renderSilenceFrames.load()),
          static_cast<unsigned long long>(wasapiZeroCount.load()), firstZero.load(),
          activeFillCount.load() == 0 ? 0ull : activeFillSum.load() / activeFillCount.load(),
          static_cast<unsigned long long>(activeFillCount.load()),
          static_cast<unsigned long long>(framesCaptured.load()),
          static_cast<unsigned long long>(captureUnderruns.load()));
      std::fflush(file);
      if (cycles != nullptr && trace != nullptr && qpcFreq.QuadPart != 0) {
        const uint32_t ready = std::min(traceWrite.load(std::memory_order_relaxed), kTraceCap);
        while (dumped < ready) {
          const CycleTrace& row = trace[dumped];
          const uint64_t us = prevQpc == 0
                                  ? 0
                                  : (row.qpc - prevQpc) * 1000000ull / static_cast<uint64_t>(qpcFreq.QuadPart);
          prevQpc = row.qpc;
          std::fwprintf(cycles, L"%llu,%llu,%u,%u,%u,%u", static_cast<unsigned long long>(row.qpc),
                        static_cast<unsigned long long>(us), row.wasapiFree, row.asked, row.written[0],
                        row.written[1]);
          for (int s = 0; s < 5; ++s) {
            std::fwprintf(cycles, L",%u,%u,%u,%u,%u", row.pushed[s], row.popped[s], row.before[s],
                          row.after[s], row.reads[s]);
          }
          std::fwprintf(cycles, L"\n");
          ++dumped;
        }
        std::fflush(cycles);
      }
      if (syncFile != nullptr && mixSync != nullptr && qpcFreq.QuadPart != 0) {
        const uint32_t ready = std::min(mixSyncWrite.load(std::memory_order_relaxed), kTraceCap);
        while (syncDumped < ready) {
          const MixSync& row = mixSync[syncDumped];
          const uint64_t deltaUs =
              row.qpcGame == 0 || row.qpcMusic == 0
                  ? 0
                  : (row.qpcMusic > row.qpcGame ? row.qpcMusic - row.qpcGame : row.qpcGame - row.qpcMusic) *
                        1000000ull / static_cast<uint64_t>(qpcFreq.QuadPart);
          std::fwprintf(syncFile, L"%llu,%llu,%llu,%u,%u,%u,%u,%u,%u,%u,%u,%llu,%llu,%u\n",
                        static_cast<unsigned long long>(row.qpcGame),
                        static_cast<unsigned long long>(row.qpcMusic),
                        static_cast<unsigned long long>(deltaUs), row.availGame, row.availMusic,
                        row.consumedGame, row.consumedMusic, row.fillBeforeGame, row.fillBeforeMusic,
                        row.fillAfterGame, row.fillAfterMusic,
                        static_cast<unsigned long long>(row.blockGame),
                        static_cast<unsigned long long>(row.blockMusic), row.asked);
          ++syncDumped;
        }
        std::fflush(syncFile);
      }
      if (prodFile != nullptr && prod != nullptr && qpcFreq.QuadPart != 0) {
        const uint32_t ready = std::min(prodWrite.load(std::memory_order_relaxed), kTraceCap);
        while (prodDumped < ready) {
          const ProdEvent& row = prod[prodDumped];
          const uint64_t gapUs = row.qpcWrite >= row.qpcIoctl
                                     ? (row.qpcWrite - row.qpcIoctl) * 1000000ull /
                                           static_cast<uint64_t>(qpcFreq.QuadPart)
                                     : 0;
          std::fwprintf(prodFile, L"%u,%llu,%llu,%llu,%u,%u,%u,%u,%llu,%u\n", row.stream,
                        static_cast<unsigned long long>(row.qpcIoctl),
                        static_cast<unsigned long long>(row.qpcWrite),
                        static_cast<unsigned long long>(gapUs), row.framesWritten, row.driverFrames,
                        row.fillBefore, row.fillAfter, static_cast<unsigned long long>(row.packet),
                        row.driverReserved);
          ++prodDumped;
        }
        std::fflush(prodFile);
      }
      Sleep(500);
    }
    std::fclose(file);
    if (cycles != nullptr) {
      std::fclose(cycles);
    }
    if (syncFile != nullptr) {
      std::fclose(syncFile);
    }
    if (prodFile != nullptr) {
      std::fclose(prodFile);
    }
  }

  void EnsureCables(WasapiEnd ends[kVirtualStrips]) {
    int openCount = 0;
    for (int i = 0; i < kVirtualStrips; ++i) {
      if (ends[i].open) {
        ++openCount;
      }
    }
    if (openCount == kVirtualStrips) {
      vadOpen.store(kVirtualStrips, std::memory_order_relaxed);
      cableNote[0] = 0;
      return;
    }
    if (cableRetry > 0) {
      --cableRetry;
      vadOpen.store(openCount, std::memory_order_relaxed);
      return;
    }
    for (int i = 0; i < kVirtualStrips; ++i) {
      if (ends[i].open) {
        continue;
      }
      wchar_t id[512];
      std::wstring err;
      if (!FindVadRender(kVirtualDeviceNames[i], id, 512)) {
        wchar_t text[160];
        wsprintfW(text, L"%s introuvable.", kVirtualDeviceNames[i]);
        CopyText(cableNote, 160, text);
        continue;
      }
      if (!OpenLoopback(id, ends[i], err)) {
        CopyText(cableNote, 160, err.c_str());
      }
    }
    openCount = 0;
    for (int i = 0; i < kVirtualStrips; ++i) {
      if (ends[i].open) {
        ++openCount;
      }
    }
    vadOpen.store(openCount, std::memory_order_relaxed);
    if (openCount == kVirtualStrips) {
      cableNote[0] = 0;
    } else {
      cableRetry = 80;
    }
  }

  void Loop() {
    _mm_setcsr(_mm_getcsr() | 0x8040);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    mmcssOn.store(mmcss != nullptr ? 1 : 0, std::memory_order_relaxed);
    audioPriority.store(GetThreadPriority(GetCurrentThread()), std::memory_order_relaxed);
    prebufferFrames.store(960);
    wchar_t prebufferText[16] = {};
    if (GetEnvironmentVariableW(L"ANYMIX_PREBUFFER_FRAMES", prebufferText, 16) > 0 ||
        GetEnvironmentVariableW(L"ANYMIX_PREBUFFER", prebufferText, 16) > 0) {
      prebufferFrames.store(static_cast<uint32_t>(wcstoul(prebufferText, nullptr, 10)));
    }
    LARGE_INTEGER qpcFreq{};
    QueryPerformanceFrequency(&qpcFreq);
    LARGE_INTEGER lastTick{};
    bool renderPrimed = true;
    bool streamReady[kVirtualStrips] = {};
    uint64_t lastPushQpc[kVirtualStrips] = {};
    bool yieldMix = false;
    bool paceMic = false;
    uint64_t paceSent = 0;
    uint64_t deviceTaken[kPhysicalBuses] = {};

    struct Storage {
      WasapiEnd captures[kMicStrips];
      WasapiEnd renders[kPhysicalBuses];
      FloatRing captureRings[kMicStrips];
      MicPull micPull[kMicStrips];
      FloatRing renderRings[kPhysicalBuses];
      wchar_t openCapture[kMicStrips][512]{};
      wchar_t openRender[kPhysicalBuses][512]{};
      float micBlock[kMicStrips][kBlockFrames * kChannels]{};
      float virtualBlock[kVirtualStrips][kBlockFrames * kChannels]{};
      float shapedStore[kStrips][kBlockFrames * kChannels]{};
      float busStore[kBuses][kBlockFrames * kChannels]{};
      StripEq stripEq[kStrips];
      NoiseGate micGate[kMicStrips];
      PeakLimit busLimit[kBuses];
      FloatRing virtualIn[kVirtualStrips];
      WasapiEnd loops[kVirtualStrips];
    };
    const auto store = std::make_unique<Storage>();
    auto& captures = store->captures;
    auto& renders = store->renders;
    auto& captureRings = store->captureRings;
    auto& micPull = store->micPull;
    auto& renderRings = store->renderRings;
    auto& openCapture = store->openCapture;
    auto& openRender = store->openRender;
    auto& micBlock = store->micBlock;
    auto& virtualBlock = store->virtualBlock;
    auto& shapedStore = store->shapedStore;
    auto& busStore = store->busStore;
    auto& stripEq = store->stripEq;
    auto& micGate = store->micGate;
    auto& busLimit = store->busLimit;
    auto& virtualIn = store->virtualIn;
    auto& loops = store->loops;
    link.Start(running, linkNote);
    trace = new CycleTrace[kTraceCap]{};
    traceWrite.store(0, std::memory_order_relaxed);
    mixSync = new MixSync[kTraceCap]{};
    mixSyncWrite.store(0, std::memory_order_relaxed);
    prod = new ProdEvent[kTraceCap]{};
    prodWrite.store(0, std::memory_order_relaxed);
    uint64_t prodPacket[kVirtualStrips] = {};
    uint64_t blockCount[kVirtualStrips] = {};
    uint64_t qpcGame = 0;
    uint64_t qpcMusic = 0;
    auto drainQueued = [&]() {
      for (int i = 0; i < kPhysicalBuses; ++i) {
        if (!renders[i].open) {
          continue;
        }
        const UINT32 wrote = DrainRender(renders[i], renderRings[i]);
        if (paceMic) {
          deviceTaken[i] += wrote;
        }
      }
    };

    for (int i = 0; i < kMicStrips; ++i) {
      micGate[i].Prepare(static_cast<float>(kSampleRate));
    }

    while (running.load(std::memory_order_relaxed)) {
      const uint32_t seq = configSeq.load(std::memory_order_acquire);
      if (seq != appliedSeq) {
        wchar_t nextCapture[kMicStrips][512];
        wchar_t nextRender[kPhysicalBuses][512];
        {
          std::lock_guard<std::mutex> lock(configLock);
          std::memcpy(nextCapture, captureId, sizeof(nextCapture));
          std::memcpy(nextRender, renderId, sizeof(nextRender));
        }
        std::wstring openError;
        bool anyFail = false;
        for (int i = 0; i < kMicStrips; ++i) {
          if (wcscmp(nextCapture[i], openCapture[i]) != 0) {
            captures[i].Close();
            captureRings[i].Clear();
            micPull[i].Reset();
            if (nextCapture[i][0] == 0) {
              openCapture[i][0] = 0;
            } else if (OpenEndpoint(nextCapture[i], true, captures[i], openError)) {
              CopyText(openCapture[i], 512, nextCapture[i]);
              captureRate.store(static_cast<uint32_t>(captures[i].rate), std::memory_order_relaxed);
              captureChannels.store(static_cast<uint32_t>(captures[i].channels), std::memory_order_relaxed);
              captureBits.store(static_cast<uint32_t>(captures[i].bits), std::memory_order_relaxed);
            } else {
              openCapture[i][0] = 0;
              anyFail = true;
            }
          }
        }
        for (int i = 0; i < kPhysicalBuses; ++i) {
          if (wcscmp(nextRender[i], openRender[i]) != 0) {
            renders[i].Close();
            if (nextRender[i][0] == 0) {
              openRender[i][0] = 0;
            } else if (OpenEndpoint(nextRender[i], false, renders[i], openError)) {
              CopyText(openRender[i], 512, nextRender[i]);
              renderRate.store(static_cast<uint32_t>(renders[i].rate), std::memory_order_relaxed);
              renderChannels.store(static_cast<uint32_t>(renders[i].channels), std::memory_order_relaxed);
              renderBits.store(static_cast<uint32_t>(renders[i].bits), std::memory_order_relaxed);
              wasapiBuffer.store(renders[i].bufferFrames, std::memory_order_relaxed);
            } else {
              openRender[i][0] = 0;
              anyFail = true;
            }
          }
        }
        {
          std::lock_guard<std::mutex> lock(configLock);
          deviceError = anyFail ? openError : L"";
        }
        appliedSeq = seq;
      }

      bool captureShort = false;
      uint32_t pushedFrames[5] = {};
      uint32_t poppedFrames[5] = {};
      uint32_t fillBefore[5] = {};
      uint32_t fillAfter[5] = {};
      uint32_t driverPackets[5] = {};
      uint32_t wasapiWritten[2] = {};
      for (int i = 0; i < kMicStrips; ++i) {
        DrainCapture(captures[i], captureRings[i], captureShort);
      }

      uint32_t renderSpace = 0;
      bool renderOpen = false;
      bool renderNeeds = false;
      for (int i = 0; i < kPhysicalBuses; ++i) {
        if (!renders[i].open) {
          continue;
        }
        renderOpen = true;
        UINT32 padding = 0;
        if (FAILED(renders[i].client->GetCurrentPadding(&padding))) {
          continue;
        }
        if (padding == 0) {
          renderUnderruns.fetch_add(1, std::memory_order_relaxed);
          if (renderPrimed) {
            wasapiZeroCount.fetch_add(1, std::memory_order_relaxed);
            uint32_t seen = 0;
            firstZero.compare_exchange_strong(seen, 2, std::memory_order_relaxed);
          }
        }
        if (padding >= renders[i].bufferFrames) {
          continue;
        }
        renderNeeds = true;
        wasapiPadding.store(padding, std::memory_order_relaxed);
        wasapiBuffer.store(renders[i].bufferFrames, std::memory_order_relaxed);
        renderSpace = std::max(renderSpace, renders[i].bufferFrames - padding);
      }

      uint32_t frames = 0;
      if (yieldMix) {
        yieldMix = false;
        frames = 0;
      } else if (renderNeeds) {
        frames = std::min<uint32_t>(renderSpace, kBlockFrames);
        renderSpaceSum.fetch_add(renderSpace, std::memory_order_relaxed);
        renderSpaceCount.fetch_add(1, std::memory_order_relaxed);
      } else if (!renderOpen) {
        frames = kBlockFrames;
      }
      EnsureCables(loops);
      for (int stream = 0; stream < kVirtualStrips; ++stream) {
        const uint32_t pushed = DrainLoopback(loops[stream], virtualIn[stream]);
        if (pushed == 0) {
          continue;
        }
        pushedFrames[stream] += pushed;
        driverPackets[stream] += 1;
        LARGE_INTEGER ioctlTick{};
        QueryPerformanceCounter(&ioctlTick);
        lastPushQpc[stream] = static_cast<uint64_t>(ioctlTick.QuadPart);
      }
      auto noteCycle = [&]() {
        if (trace == nullptr) {
          return;
        }
        const uint32_t index = traceWrite.fetch_add(1, std::memory_order_relaxed);
        if (index >= kTraceCap) {
          return;
        }
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        CycleTrace& row = trace[index];
        row.qpc = static_cast<uint64_t>(now.QuadPart);
        row.wasapiFree = static_cast<uint16_t>(std::min<uint32_t>(renderSpace, 65535));
        row.asked = static_cast<uint16_t>(std::min<uint32_t>(frames, 65535));
        for (int i = 0; i < 2; ++i) {
          row.written[i] = static_cast<uint16_t>(std::min(wasapiWritten[i], 65535u));
        }
        for (int i = 0; i < 5; ++i) {
          row.pushed[i] = static_cast<uint16_t>(std::min(pushedFrames[i], 65535u));
          row.popped[i] = static_cast<uint16_t>(std::min(poppedFrames[i], 65535u));
          row.before[i] = static_cast<uint16_t>(std::min(fillBefore[i], 65535u));
          row.after[i] = static_cast<uint16_t>(std::min(fillAfter[i], 65535u));
          row.reads[i] = static_cast<uint8_t>(std::min(driverPackets[i], 255u));
        }
      };
      if (frames == 0) {
        noteCycle();
        drainQueued();
        HANDLE events[kPhysicalBuses];
        DWORD count = 0;
        for (int i = 0; i < kPhysicalBuses; ++i) {
          if (renders[i].open && renders[i].event != nullptr) {
            events[count++] = renders[i].event;
          }
        }
        if (count > 0) {
          WaitForMultipleObjects(count, events, FALSE, 10);
        } else {
          Sleep(2);
        }
        continue;
      }

      LARGE_INTEGER workStart{};
      QueryPerformanceCounter(&workStart);
      if (lastTick.QuadPart != 0 && qpcFreq.QuadPart != 0) {
        const uint64_t intervalUs = static_cast<uint64_t>(
            (workStart.QuadPart - lastTick.QuadPart) * 1000000 / qpcFreq.QuadPart);
        intervalUsSum.fetch_add(intervalUs, std::memory_order_relaxed);
        intervalCount.fetch_add(1, std::memory_order_relaxed);
        uint64_t seenInterval = intervalUsMin.load(std::memory_order_relaxed);
        while (intervalUs < seenInterval &&
               !intervalUsMin.compare_exchange_weak(seenInterval, intervalUs, std::memory_order_relaxed)) {
        }
        uint64_t seen = intervalUsMax.load(std::memory_order_relaxed);
        while (intervalUs > seen &&
               !intervalUsMax.compare_exchange_weak(seen, intervalUs, std::memory_order_relaxed)) {
        }
      }
      lastTick = workStart;

      uint32_t playable = 0;
      const uint32_t reserve = prebufferFrames.load(std::memory_order_relaxed);
      int readyStreams = 0;
      bool liveGap = false;
      uint32_t leastExcess = 0xffffffffu;
      LARGE_INTEGER gapNow{};
      QueryPerformanceCounter(&gapNow);
      for (int stream = 0; stream < kVirtualStrips; ++stream) {
        const uint32_t fill = virtualIn[stream].Available();
        if (!streamReady[stream] && reserve > 0 && fill >= reserve) {
          streamReady[stream] = true;
        }
        if (reserve == 0) {
          if (fill > playable) {
            playable = fill;
          }
          continue;
        }
        if (!streamReady[stream]) {
          continue;
        }
        const uint32_t excess = fill > reserve ? fill - reserve : 0;
        const uint64_t ageUs =
            qpcFreq.QuadPart == 0 || lastPushQpc[stream] == 0
                ? 1000000ull
                : (static_cast<uint64_t>(gapNow.QuadPart) - lastPushQpc[stream]) * 1000000ull /
                      static_cast<uint64_t>(qpcFreq.QuadPart);
        if (excess == 0 && ageUs > 80000ull) {
          continue;
        }
        if (excess == 0) {
          liveGap = true;
          continue;
        }
        readyStreams += 1;
        if (excess < leastExcess) {
          leastExcess = excess;
        }
        if (excess > playable) {
          playable = excess;
        }
      }
      if (reserve > 0) {
        if (liveGap) {
          playable = 0;
        } else if (readyStreams == 0) {
          playable = frames;
        } else {
          playable = leastExcess;
        }
      }
      if (!renderPrimed) {
        frames = 0;
      }
      if (renderPrimed) {
        activeFillSum.fetch_add(playable, std::memory_order_relaxed);
        activeFillCount.fetch_add(1, std::memory_order_relaxed);
        ringFill.store(playable, std::memory_order_relaxed);
        uint32_t seenMin = ringMin.load(std::memory_order_relaxed);
        while (playable < seenMin &&
               !ringMin.compare_exchange_weak(seenMin, playable, std::memory_order_relaxed)) {
        }
        uint32_t seenMax = ringMax.load(std::memory_order_relaxed);
        while (playable > seenMax &&
               !ringMax.compare_exchange_weak(seenMax, playable, std::memory_order_relaxed)) {
        }
      }
      if (renderPrimed && renderOpen && frames > 0 && playable < frames) {
        renderStarvationCount.fetch_add(1, std::memory_order_relaxed);
        virtualUnderrunFrames.fetch_add(frames - playable, std::memory_order_relaxed);
        if (playable == 0) {
          virtualRingEmptyCount.fetch_add(1, std::memory_order_relaxed);
          uint32_t seen = 0;
          firstZero.compare_exchange_strong(seen, 1, std::memory_order_relaxed);
        }
        frames = playable;
      }
      const bool micOnly = reserve > 0 && !liveGap && readyStreams == 0 && renderOpen;
      if (!micOnly) {
        paceMic = false;
        paceSent = 0;
        std::memset(deviceTaken, 0, sizeof(deviceTaken));
      } else if (frames > 0) {
        paceMic = true;
        uint64_t slowest = ~0ull;
        for (int i = 0; i < kPhysicalBuses; ++i) {
          if (renders[i].open && deviceTaken[i] < slowest) {
            slowest = deviceTaken[i];
          }
        }
        if (slowest == ~0ull) {
          slowest = 0;
        }
        const uint64_t allowed = slowest + static_cast<uint64_t>(frames);
        if (paceSent >= allowed) {
          frames = 0;
        } else if (paceSent + static_cast<uint64_t>(frames) > allowed) {
          frames = static_cast<uint32_t>(allowed - paceSent);
        }
      } else {
        paceMic = true;
      }
      if (frames == 0) {
        noteCycle();
        drainQueued();
        HANDLE events[kPhysicalBuses];
        DWORD count = 0;
        for (int i = 0; i < kPhysicalBuses; ++i) {
          if (renders[i].open && renders[i].event != nullptr) {
            events[count++] = renders[i].event;
          }
        }
        if (count > 0) {
          WaitForMultipleObjects(count, events, FALSE, 10);
        } else {
          Sleep(2);
        }
        continue;
      }
      if (paceMic) {
        paceSent += frames;
      }
      for (int i = 0; i < kMicStrips; ++i) {
        if (captures[i].open) {
          uint64_t holds = 0;
          PullMic(captureRings[i], micPull[i], micBlock[i], frames, holds);
          if (holds > 0) {
            captureUnderruns.fetch_add(holds, std::memory_order_relaxed);
          }
        } else {
          std::memset(micBlock[i], 0, sizeof(float) * frames * kChannels);
        }
        const float micGain = gain[kMic1 + i].load(std::memory_order_relaxed);
        if (micGain != 1.f) {
          const int micSamples = static_cast<int>(frames) * kChannels;
          for (int s = 0; s < micSamples; ++s) {
            micBlock[i][s] *= micGain;
          }
        }
        micGate[i].SetLevel(gateLevel[i].load(std::memory_order_relaxed));
        const bool open = micGate[i].Process(micBlock[i], static_cast<int>(frames));
        gateOpen[i].store(open, std::memory_order_relaxed);
      }
      for (int stream = 0; stream < kVirtualStrips; ++stream) {
        const uint32_t fill = virtualIn[stream].Available();
        fillBefore[stream] = fill;
        fillBeforeSum.fetch_add(fill, std::memory_order_relaxed);
        popRequested.fetch_add(frames, std::memory_order_relaxed);
        if (stream == kGame || stream == kMusic) {
          LARGE_INTEGER prep{};
          QueryPerformanceCounter(&prep);
          if (stream == kGame) {
            qpcGame = static_cast<uint64_t>(prep.QuadPart);
          } else {
            qpcMusic = static_cast<uint64_t>(prep.QuadPart);
          }
        }
        const bool holdReserve = reserve > 0 && streamReady[stream] && fill >= frames + reserve;
        const bool unreserved = reserve == 0 && fill >= frames;
        if (holdReserve || unreserved) {
          virtualIn[stream].Pop(virtualBlock[stream], frames);
          poppedFrames[stream] = frames;
          popActual.fetch_add(frames, std::memory_order_relaxed);
        } else {
          std::memset(virtualBlock[stream], 0, sizeof(float) * frames * kChannels);
        }
        fillAfter[stream] = virtualIn[stream].Available();
        fillAfterSum.fetch_add(fillAfter[stream], std::memory_order_relaxed);
        if (poppedFrames[stream] > 0) {
          blockCount[stream] += 1;
        }
      }
      if (mixSync != nullptr) {
        const uint32_t index = mixSyncWrite.fetch_add(1, std::memory_order_relaxed);
        if (index < kTraceCap) {
          MixSync& row = mixSync[index];
          row.qpcGame = qpcGame;
          row.qpcMusic = qpcMusic;
          row.availGame = fillBefore[kGame];
          row.availMusic = fillBefore[kMusic];
          row.consumedGame = poppedFrames[kGame];
          row.consumedMusic = poppedFrames[kMusic];
          row.fillBeforeGame = fillBefore[kGame];
          row.fillBeforeMusic = fillBefore[kMusic];
          row.fillAfterGame = fillAfter[kGame];
          row.fillAfterMusic = fillAfter[kMusic];
          row.blockGame = blockCount[kGame];
          row.blockMusic = blockCount[kMusic];
          row.asked = frames;
        }
      }

      float* inputs[kStrips];
      for (int i = 0; i < kVirtualStrips; ++i) {
        inputs[i] = virtualBlock[i];
      }
      inputs[kMic1] = micBlock[0];
      inputs[kMic2] = micBlock[1];

      bool anySolo = false;
      BlockParams params[kStrips];
      for (int i = 0; i < kStrips; ++i) {
        params[i].gain = i >= kMic1 ? 1.f : gain[i].load(std::memory_order_relaxed);
        params[i].mono = mono[i].load(std::memory_order_relaxed);
        params[i].pan = pan[i].load(std::memory_order_relaxed);
        params[i].route = route[i].load(std::memory_order_relaxed);
        params[i].audible = !mute[i].load(std::memory_order_relaxed);
        if (solo[i].load(std::memory_order_relaxed)) {
          anySolo = true;
        }
        LoadEq(eq[i], params[i].bass, params[i].treble);
        const float bass = bassDb[i].load(std::memory_order_relaxed);
        const float treble = trebleDb[i].load(std::memory_order_relaxed);
        float back = 0.f;
        if (bass > 0.f) {
          back += bass * 0.5f;
        }
        if (treble > 0.f) {
          back += treble * 0.4f;
        }
        params[i].toneTrim = back > 0.05f ? std::pow(10.f, -back / 20.f) : 1.f;
      }
      if (anySolo) {
        for (int i = 0; i < kStrips; ++i) {
          if (!solo[i].load(std::memory_order_relaxed)) {
            params[i].audible = false;
          }
        }
      }

      float* shaped[kStrips];
      float* buses[kBuses];
      for (int i = 0; i < kStrips; ++i) {
        shaped[i] = shapedStore[i];
      }
      for (int i = 0; i < kBuses; ++i) {
        buses[i] = busStore[i];
      }
      ShapeStrips(inputs, static_cast<int>(frames), params, stripEq, shaped);
      vst.Process(shaped, kStrips, static_cast<int>(frames));
      ExchangeAsio(shaped, static_cast<int>(frames));
      for (int i = 0; i < kStrips; ++i) {
        if (!params[i].audible) {
          std::memset(shaped[i], 0, sizeof(float) * static_cast<size_t>(frames) * kChannels);
        }
      }
      MixToBuses(shaped, static_cast<int>(frames), params, buses);
      const int mixSamples = static_cast<int>(frames) * kChannels;
      bool anyBusSolo = false;
      for (int bus = 0; bus < kPhysicalBuses; ++bus) {
        if (busSolo[bus].load(std::memory_order_relaxed)) {
          anyBusSolo = true;
        }
      }
      float preMuteL[kBuses]{};
      float preMuteR[kBuses]{};
      for (int bus = 0; bus < kBuses; ++bus) {
        float* samples = buses[bus];
        const float outputGain = busGain[bus].load(std::memory_order_relaxed);
        const bool toMono = busMono[bus].load(std::memory_order_relaxed);
        StereoPeaks(samples, static_cast<int>(frames), outputGain, toMono, preMuteL[bus], preMuteR[bus]);
        const bool soloCut = bus < kPhysicalBuses && anyBusSolo && !busSolo[bus].load(std::memory_order_relaxed);
        const bool audible = !busMute[bus].load(std::memory_order_relaxed) && !soloCut;
        if (!audible) {
          std::memset(samples, 0, sizeof(float) * static_cast<size_t>(mixSamples));
          busLimit[bus] = {};
          continue;
        }
        if (toMono || outputGain != 1.f) {
          const int blockFrames = static_cast<int>(frames);
          for (int i = 0; i < blockFrames; ++i) {
            float left = samples[i * 2];
            float right = samples[i * 2 + 1];
            if (toMono) {
              const float mid = 0.5f * (left + right);
              left = mid;
              right = mid;
            }
            samples[i * 2] = left * outputGain;
            samples[i * 2 + 1] = right * outputGain;
          }
        }
        LimitStereo(samples, static_cast<int>(frames), busLimit[bus]);
      }
      if (frames > 0 && frames <= kBlockFrames) {
        MixRouted(buses, static_cast<int>(frames), busLimit);
      }

      for (int i = 0; i < kStrips; ++i) {
        float left = 0.f;
        float right = 0.f;
        if (mute[i].load(std::memory_order_relaxed)) {
          StereoPeaks(inputs[i], static_cast<int>(frames), params[i].gain, params[i].mono, left, right,
                      params[i].pan);
        } else {
          StereoPeaks(shaped[i], static_cast<int>(frames), 1.f, false, left, right);
        }
        peakL[i].store(left, std::memory_order_relaxed);
        peakR[i].store(right, std::memory_order_relaxed);
      }
      for (int i = 0; i < kBuses; ++i) {
        float left = 0.f;
        float right = 0.f;
        if (busMute[i].load(std::memory_order_relaxed)) {
          left = preMuteL[i];
          right = preMuteR[i];
        } else {
          StereoPeaks(buses[i], static_cast<int>(frames), 1.f, false, left, right);
        }
        busPeakL[i].store(left, std::memory_order_relaxed);
        busPeakR[i].store(right, std::memory_order_relaxed);
      }
      UINT32 rendered = 0;
      for (int i = 0; i < kPhysicalBuses; ++i) {
        if (renders[i].open) {
          renderRings[i].Push(buses[i], frames);
          const UINT32 wrote = DrainRender(renders[i], renderRings[i]);
          rendered += wrote;
          if (paceMic) {
            deviceTaken[i] += wrote;
          }
          if (i < 2) {
            wasapiWritten[i] = wrote;
          }
        }
      }
      yieldMix = true;
      noteCycle();
      for (int bus = 0; bus < kVirtualBuses; ++bus) {
        if (frames == 0 || frames > kBlockFrames) {
          continue;
        }
        link.Push(bus, buses[kPhysicalBuses + bus], frames);
      }

      if (recording.load(std::memory_order_relaxed)) {
        const int bus = recordBus.load(std::memory_order_relaxed);
        if (bus >= 0 && bus < kBuses) {
          bool real = false;
          for (int strip = 0; strip < kStrips; ++strip) {
            if ((params[strip].route & static_cast<uint16_t>(1u << bus)) == 0 || !params[strip].audible) {
              continue;
            }
            if (strip < kVirtualStrips) {
              if (poppedFrames[strip] > 0) {
                real = true;
              }
            } else if (captures[strip - kVirtualStrips].open) {
              real = true;
            }
          }
          if (real) {
            WriteRecordBlock(buses[bus], static_cast<int>(frames));
          }
        }
      }

      LARGE_INTEGER workEnd{};
      QueryPerformanceCounter(&workEnd);
      if (qpcFreq.QuadPart != 0) {
        const uint64_t processUs = static_cast<uint64_t>(
            (workEnd.QuadPart - workStart.QuadPart) * 1000000 / qpcFreq.QuadPart);
        processUsSum.fetch_add(processUs, std::memory_order_relaxed);
        uint64_t seen = processUsMax.load(std::memory_order_relaxed);
        while (processUs > seen &&
               !processUsMax.compare_exchange_weak(seen, processUs, std::memory_order_relaxed)) {
        }
        const uint64_t deadlineUs = static_cast<uint64_t>(frames) * 1000000ull / kSampleRate;
        if (processUs > deadlineUs) {
          missedDeadlines.fetch_add(1, std::memory_order_relaxed);
        }
      }
      blocks.fetch_add(1, std::memory_order_relaxed);
      HANDLE events[kPhysicalBuses];
      DWORD count = 0;
      for (int i = 0; i < kPhysicalBuses; ++i) {
        if (renders[i].open && renders[i].event != nullptr) {
          events[count++] = renders[i].event;
        }
      }
      if (count > 0 && (rendered == 0 || renderSpace < 48)) {
        WaitForMultipleObjects(count, events, FALSE, 10);
      }
    }

    for (int i = 0; i < kMicStrips; ++i) {
      captures[i].Close();
    }
    for (int i = 0; i < kPhysicalBuses; ++i) {
      renders[i].Close();
    }
    for (int i = 0; i < kVirtualStrips; ++i) {
      loops[i].Close();
    }
    link.Stop();
    if (mmcss != nullptr) {
      AvRevertMmThreadCharacteristics(mmcss);
    }
    CoUninitialize();
  }
};

Engine::Engine() : impl_(new Impl) {}

Engine::~Engine() {
  Stop();
  impl_->FinishRecorder();
  if (impl_->driver != INVALID_HANDLE_VALUE) {
    CloseHandle(impl_->driver);
  }
  impl_->CloseAsio();
  delete impl_;
}

bool Engine::Start(std::wstring& error) {
  if (impl_->running.load()) {
    return true;
  }
  impl_->running.store(true);
  try {
    impl_->diagThread = std::thread([this] { impl_->WriteDiagLoop(); });
    impl_->thread = std::thread([this] { impl_->Loop(); });
  } catch (...) {
    impl_->running.store(false);
    error = L"Cannot start the audio thread.";
    return false;
  }
  error.clear();
  return true;
}

void Engine::Stop() {
  if (!impl_->running.exchange(false)) {
    return;
  }
  if (impl_->thread.joinable()) {
    impl_->thread.join();
  }
  if (impl_->diagThread.joinable()) {
    impl_->diagThread.join();
  }
  delete[] impl_->trace;
  impl_->trace = nullptr;
  delete[] impl_->mixSync;
  impl_->mixSync = nullptr;
  delete[] impl_->prod;
  impl_->prod = nullptr;
}

bool Engine::Running() const { return impl_->running.load(); }

void Engine::SetCaptureDevice(int micIndex, const wchar_t* endpointId) {
  if (micIndex < 0 || micIndex >= kMicStrips) {
    return;
  }
  std::lock_guard<std::mutex> lock(impl_->configLock);
  CopyText(impl_->captureId[micIndex], 512, endpointId);
  impl_->configSeq.fetch_add(1, std::memory_order_release);
}

void Engine::SetRenderDevice(int outputIndex, const wchar_t* endpointId) {
  if (outputIndex < 0 || outputIndex >= kPhysicalBuses) {
    return;
  }
  std::lock_guard<std::mutex> lock(impl_->configLock);
  CopyText(impl_->renderId[outputIndex], 512, endpointId);
  impl_->configSeq.fetch_add(1, std::memory_order_release);
}

void Engine::SetBusGainDb(int bus, float db) {
  if (bus < 0 || bus >= kBuses) {
    return;
  }
  db = std::max(kFaderMinDb, std::min(kFaderMaxDb, db));
  impl_->busGain[bus].store(DbToGain(db), std::memory_order_relaxed);
}

void Engine::SetBusMute(int bus, bool on) {
  if (bus < 0 || bus >= kBuses) {
    return;
  }
  impl_->busMute[bus].store(on, std::memory_order_relaxed);
}

void Engine::SetBusMono(int bus, bool on) {
  if (bus < 0 || bus >= kBuses) {
    return;
  }
  impl_->busMono[bus].store(on, std::memory_order_relaxed);
}

void Engine::SetBusSolo(int bus, bool on) {
  if (bus < 0 || bus >= kPhysicalBuses) {
    return;
  }
  impl_->busSolo[bus].store(on, std::memory_order_relaxed);
}

void Engine::SetGainDb(int strip, float db) {
  if (strip < 0 || strip >= kStrips) {
    return;
  }
  db = std::max(kFaderMinDb, std::min(kFaderMaxDb, db));
  impl_->gain[strip].store(DbToGain(db), std::memory_order_relaxed);
}

void Engine::SetBassDb(int strip, float db) {
  if (strip < 0 || strip >= kStrips) {
    return;
  }
  db = std::max(-12.f, std::min(12.f, db));
  impl_->bassDb[strip].store(db, std::memory_order_relaxed);
  PublishEq(impl_->eq[strip], db, impl_->trebleDb[strip].load(std::memory_order_relaxed));
}

void Engine::SetTrebleDb(int strip, float db) {
  if (strip < 0 || strip >= kStrips) {
    return;
  }
  db = std::max(-12.f, std::min(12.f, db));
  impl_->trebleDb[strip].store(db, std::memory_order_relaxed);
  PublishEq(impl_->eq[strip], impl_->bassDb[strip].load(std::memory_order_relaxed), db);
}

void Engine::SetMute(int strip, bool on) {
  if (strip < 0 || strip >= kStrips) {
    return;
  }
  impl_->mute[strip].store(on, std::memory_order_relaxed);
}

void Engine::SetSolo(int strip, bool on) {
  if (strip < 0 || strip >= kStrips) {
    return;
  }
  impl_->solo[strip].store(on, std::memory_order_relaxed);
}

void Engine::SetMono(int strip, bool on) {
  if (strip < 0 || strip >= kStrips) {
    return;
  }
  impl_->mono[strip].store(on, std::memory_order_relaxed);
}

void Engine::SetInsert(int strip, bool on) {
  if (strip < 0 || strip >= kStrips) {
    return;
  }
  impl_->insertOn[strip].store(on, std::memory_order_relaxed);
}

void Engine::SetGate(int strip, bool on) {
  if (strip < 0 || strip >= kStrips) {
    return;
  }
  impl_->gateOn[strip].store(on, std::memory_order_relaxed);
}

void Engine::SetGateDepth(int strip, float depth) {
  if (strip < 0 || strip >= kStrips) {
    return;
  }
  depth = std::max(0.f, std::min(1.f, depth));
  impl_->gateDepth[strip].store(depth, std::memory_order_relaxed);
}

void Engine::SetGateLevel(int strip, float level) {
  const int mic = strip - kVirtualStrips;
  if (mic < 0 || mic >= kMicStrips) {
    return;
  }
  if (!(level >= 0.f)) {
    level = 0.f;
  }
  impl_->gateLevel[mic].store(std::min(10.f, level), std::memory_order_relaxed);
}

bool Engine::GateOpen(int strip) const {
  const int mic = strip - kVirtualStrips;
  if (mic < 0 || mic >= kMicStrips) {
    return false;
  }
  return impl_->gateOpen[mic].load(std::memory_order_relaxed);
}

void Engine::SetPan(int strip, float pan) {
  if (strip < 0 || strip >= kStrips) {
    return;
  }
  pan = std::max(-1.f, std::min(1.f, pan));
  impl_->pan[strip].store(pan, std::memory_order_relaxed);
}

void Engine::SetRoute(int strip, int bus, bool on) {
  if (strip < 0 || strip >= kStrips || bus < 0 || bus >= kBuses) {
    return;
  }
  uint16_t mask = impl_->route[strip].load(std::memory_order_relaxed);
  const uint16_t bit = static_cast<uint16_t>(1u << bus);
  if (on) {
    mask = static_cast<uint16_t>(mask | bit);
  } else {
    mask = static_cast<uint16_t>(mask & ~bit);
  }
  impl_->route[strip].store(mask, std::memory_order_relaxed);
}

float Engine::Peak(int strip) const {
  if (strip < 0 || strip >= kStrips) {
    return 0.f;
  }
  return std::max(impl_->peakL[strip].load(std::memory_order_relaxed),
                  impl_->peakR[strip].load(std::memory_order_relaxed));
}

void Engine::PeakStereo(int strip, float& left, float& right) const {
  if (strip < 0 || strip >= kStrips) {
    left = 0.f;
    right = 0.f;
    return;
  }
  left = impl_->peakL[strip].load(std::memory_order_relaxed);
  right = impl_->peakR[strip].load(std::memory_order_relaxed);
}

float Engine::BusPeak(int bus) const {
  if (bus < 0 || bus >= kBuses) {
    return 0.f;
  }
  return std::max(impl_->busPeakL[bus].load(std::memory_order_relaxed),
                  impl_->busPeakR[bus].load(std::memory_order_relaxed));
}

void Engine::BusPeakStereo(int bus, float& left, float& right) const {
  if (bus < 0 || bus >= kBuses) {
    left = 0.f;
    right = 0.f;
    return;
  }
  left = impl_->busPeakL[bus].load(std::memory_order_relaxed);
  right = impl_->busPeakR[bus].load(std::memory_order_relaxed);
}

EngineStats Engine::Stats() const {
  EngineStats stats;
  stats.framesCaptured = impl_->framesCaptured.load();
  stats.framesRendered = impl_->framesRendered.load();
  stats.blocks = impl_->blocks.load();
  stats.renderUnderruns = impl_->renderUnderruns.load();
  stats.captureUnderruns = impl_->captureUnderruns.load();
  return stats;
}

bool Engine::StartRecording(int bus, const wchar_t* folder, std::wstring& error) {
  if (bus < 0 || bus >= kBuses || folder == nullptr || folder[0] == 0) {
    error = L"Choose a bus and a folder.";
    return false;
  }
  if (impl_->recording.load()) {
    error = L"Already recording.";
    return false;
  }
  if (!impl_->OpenRecorder(folder, error)) {
    return false;
  }
  if (impl_->recordRing == nullptr) {
    impl_->recordRing = std::make_unique<int16_t[]>(Impl::kRecCapacity);
  }
  if (impl_->recordEvent == nullptr) {
    impl_->recordEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  }
  impl_->recordWrite.store(0, std::memory_order_relaxed);
  impl_->recordRead.store(0, std::memory_order_relaxed);
  impl_->recordWriterRun.store(true, std::memory_order_release);
  try {
    impl_->recordThread = std::thread([this] { impl_->RecordWriter(); });
  } catch (...) {
    impl_->recordWriterRun.store(false, std::memory_order_release);
    impl_->FinishRecorder();
    error = L"Cannot start the recording writer.";
    return false;
  }
  impl_->recordBus.store(bus);
  impl_->recording.store(true);
  return true;
}

void Engine::SetMonitorMask(uint32_t mask) {
  impl_->monitorMask.store(mask & ((1u << kBuses) - 1), std::memory_order_relaxed);
}

void Engine::SetPlaybackMask(uint32_t mask) {
  impl_->playbackMask.store(mask & ((1u << kBuses) - 1), std::memory_order_relaxed);
}

void Engine::PushPlayback(const float* interleaved, int frames) {
  if (interleaved == nullptr || frames <= 0) {
    return;
  }
  int left = frames;
  const float* src = interleaved;
  while (left > 0 && impl_->running.load(std::memory_order_relaxed)) {
    AcquireSRWLockExclusive(&impl_->playLock);
    const uint32_t free = static_cast<uint32_t>(kRingFrames) - impl_->playRing.Available();
    const uint32_t take = std::min(free, static_cast<uint32_t>(left));
    if (take > 0) {
      impl_->playRing.Push(src, take);
      src += static_cast<int>(take) * kChannels;
      left -= static_cast<int>(take);
    }
    ReleaseSRWLockExclusive(&impl_->playLock);
    if (left > 0 && impl_->playSpace != nullptr) {
      WaitForSingleObject(impl_->playSpace, 30);
    }
  }
}

void Engine::ClearPlayback() {
  AcquireSRWLockExclusive(&impl_->playLock);
  impl_->playRing.Clear();
  ReleaseSRWLockExclusive(&impl_->playLock);
  if (impl_->playSpace != nullptr) {
    SetEvent(impl_->playSpace);
  }
}

void Engine::StopRecording() {
  impl_->recording.store(false, std::memory_order_release);
  while (impl_->recordBusy.load(std::memory_order_acquire) != 0) {
    Sleep(1);
  }
  impl_->FinishRecorder();
}

bool Engine::Recording() const { return impl_->recording.load(); }

const wchar_t* Engine::RecordingPath() const { return impl_->recordPath.c_str(); }

bool Engine::DriverConnected() const {
  return impl_->vadOpen.load(std::memory_order_relaxed) > 0 || impl_->link.Live();
}

const wchar_t* Engine::DeviceError() const {
  if (!impl_->deviceError.empty()) {
    return impl_->deviceError.c_str();
  }
  if (impl_->linkNote[0] != 0) {
    return impl_->linkNote;
  }
  return impl_->cableNote;
}

void Engine::AsioLinkStatus(wchar_t* text, int capacity, int& fault) const {
  impl_->link.CopyStatus(text, capacity, fault);
}

int Engine::AddVst(int strip, const wchar_t* path, std::wstring& error) {
  return impl_->vst.Add(strip, path, error);
}

void Engine::RemoveVst(int slot) { impl_->vst.Remove(slot); }

void Engine::SetVstBypass(int slot, bool on) { impl_->vst.SetBypass(slot, on); }

void Engine::SetVstGroup(int slot, int group) { impl_->vst.SetGroup(slot, group); }

void Engine::SetVstGroupBypass(int group, bool on) { impl_->vst.SetGroupBypass(group, on); }

void Engine::SetLocalVst(int strip, bool on) { impl_->vst.SetLocal(strip, on); }

void Engine::ToggleVstEditor(int slot) { impl_->vst.ToggleEditor(slot); }

void Engine::VstSlot(int slot, VstSlotView& out) const { impl_->vst.Slot(slot, out); }

}  // namespace anymix
