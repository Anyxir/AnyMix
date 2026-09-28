#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

static IMMDevice* FindRender(IMMDeviceEnumerator* enumerator, const wchar_t* want) {
  IMMDeviceCollection* collection = nullptr;
  enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
  UINT total = 0;
  collection->GetCount(&total);
  IMMDevice* found = nullptr;
  for (UINT i = 0; i < total; ++i) {
    IMMDevice* device = nullptr;
    collection->Item(i, &device);
    IPropertyStore* store = nullptr;
    device->OpenPropertyStore(STGM_READ, &store);
    PROPVARIANT name;
    PropVariantInit(&name);
    store->GetValue(PKEY_Device_FriendlyName, &name);
    if (name.vt == VT_LPWSTR && wcsstr(name.pwszVal, want) != nullptr) {
      wprintf(L"render name %ls\n", name.pwszVal);
      found = device;
    } else {
      device->Release();
    }
    PropVariantClear(&name);
    store->Release();
    if (found != nullptr) {
      break;
    }
  }
  collection->Release();
  return found;
}

static IMMDevice* FindCapture(IMMDeviceEnumerator* enumerator, const wchar_t* want) {
  IMMDeviceCollection* collection = nullptr;
  enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
  UINT total = 0;
  collection->GetCount(&total);
  IMMDevice* found = nullptr;
  for (UINT i = 0; i < total; ++i) {
    IMMDevice* device = nullptr;
    collection->Item(i, &device);
    IPropertyStore* store = nullptr;
    device->OpenPropertyStore(STGM_READ, &store);
    PROPVARIANT name;
    PropVariantInit(&name);
    store->GetValue(PKEY_Device_FriendlyName, &name);
    if (name.vt == VT_LPWSTR && wcsstr(name.pwszVal, want) != nullptr) {
      found = device;
      wprintf(L"capture name %ls\n", name.pwszVal);
    } else {
      device->Release();
    }
    PropVariantClear(&name);
    store->Release();
    if (found != nullptr) {
      break;
    }
  }
  collection->Release();
  return found;
}

struct Cap {
  IAudioClient* client = nullptr;
  IAudioCaptureClient* capture = nullptr;
  WAVEFORMATEX* format = nullptr;
  HANDLE event = nullptr;
  std::vector<float> left;
  std::vector<float> right;
};

static bool OpenCapture(IMMDevice* device, Cap& cap) {
  device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&cap.client));
  cap.client->GetMixFormat(&cap.format);
  const WAVEFORMATEX* f = cap.format;
  wprintf(L"mix tag %u rate %lu ch %u bits %u align %u avg %lu\n", f->wFormatTag, f->nSamplesPerSec, f->nChannels,
          f->wBitsPerSample, f->nBlockAlign, f->nAvgBytesPerSec);
  if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
    wprintf(L"ext valid %lu samples %u sub %08lx\n", ext->dwChannelMask, ext->Samples.wValidBitsPerSample,
            ext->SubFormat.Data1);
  }
  cap.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  const HRESULT hr = cap.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 10000000, 0,
                                            cap.format, nullptr);
  wprintf(L"capture init 0x%08lx\n", static_cast<unsigned long>(hr));
  if (FAILED(hr)) {
    return false;
  }
  cap.client->SetEventHandle(cap.event);
  cap.client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&cap.capture));
  cap.client->Start();
  return true;
}

static void Pull(Cap& cap, int maxFrames) {
  UINT32 packet = 0;
  while (SUCCEEDED(cap.capture->GetNextPacketSize(&packet)) && packet > 0) {
    BYTE* data = nullptr;
    UINT32 frames = 0;
    DWORD flags = 0;
    if (FAILED(cap.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
      break;
    }
    const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
    const int channels = cap.format->nChannels;
    const bool isFloat = cap.format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                         (cap.format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                          reinterpret_cast<WAVEFORMATEXTENSIBLE*>(cap.format)->SubFormat.Data1 == 3);
    for (UINT32 i = 0; i < frames && static_cast<int>(cap.left.size()) < maxFrames; ++i) {
      float left = 0.f;
      float right = 0.f;
      if (!silent && channels > 0) {
        auto take = [&](int channel) {
          if (isFloat) {
            return reinterpret_cast<float*>(data)[i * channels + channel];
          }
          if (cap.format->wBitsPerSample == 32) {
            return static_cast<float>(reinterpret_cast<int32_t*>(data)[i * channels + channel]) / 2147483648.f;
          }
          if (cap.format->wBitsPerSample == 16) {
            return static_cast<float>(reinterpret_cast<int16_t*>(data)[i * channels + channel]) / 32768.f;
          }
          return 0.f;
        };
        left = take(0);
        right = channels > 1 ? take(1) : left;
      }
      cap.left.push_back(left);
      cap.right.push_back(right);
    }
    cap.capture->ReleaseBuffer(frames);
  }
}

static void Report(const char* label, const std::vector<float>& samples, int rate) {
  if (samples.size() < 1000) {
    wprintf(L"%hs frames %zu too short\n", label, samples.size());
    return;
  }
  const int begin = static_cast<int>(samples.size()) / 10;
  const int end = static_cast<int>(samples.size()) * 9 / 10;
  double sum = 0;
  float peak = 0.f;
  int zeros = 0;
  int zeroRun = 0;
  int longestZero = 0;
  int crossings = 0;
  int over05 = 0;
  int over08 = 0;
  int over20 = 0;
  float maxDelta = 0.f;
  int spikes = 0;
  float prev = samples[static_cast<size_t>(begin)];
  float prev2 = prev;
  const int window = rate / 10;
  float worst = 0.f;
  int worstAt = 0;
  float winMax = 0.f;
  int winStart = begin;
  for (int i = begin; i < end; ++i) {
    const float s = samples[static_cast<size_t>(i)];
    sum += static_cast<double>(s) * s;
    peak = std::fabs(s) > peak ? std::fabs(s) : peak;
    if (std::fabs(s) < 1.0e-4f) {
      zeros++;
      zeroRun++;
      longestZero = zeroRun > longestZero ? zeroRun : longestZero;
    } else {
      zeroRun = 0;
    }
    if ((prev < 0.f && s >= 0.f) || (prev >= 0.f && s < 0.f)) {
      crossings++;
    }
    const float delta = std::fabs(s - prev);
    maxDelta = delta > maxDelta ? delta : maxDelta;
    winMax = delta > winMax ? delta : winMax;
    if (delta > 0.05f) {
      over05++;
    }
    if (delta > 0.08f) {
      over08++;
    }
    if (delta > 0.2f) {
      over20++;
    }
    const float accel = std::fabs(s - 2.f * prev + prev2);
    if (accel > 0.15f) {
      spikes++;
    }
    prev2 = prev;
    if ((i - winStart) >= window) {
      if (winMax > worst) {
        worst = winMax;
        worstAt = winStart;
      }
      winStart = i;
      winMax = 0.f;
    }
    prev = s;
  }
  const int n = end - begin;
  const double rms = std::sqrt(sum / n);
  const double hz = 0.5 * crossings * static_cast<double>(rate) / n;
  const double expected = (peak > 0.01f) ? (6.283185307179586 * 1000.0 * peak / rate) : 0.0;
  wprintf(L"%hs n %d rms %.4f peak %.3f hz %.1f maxDelta %.4f expected %.4f >0.05 %d >0.08 %d >0.20 %d spikes %d zero %.2f%% longestZero %d worstWindow %.4f at %.2fs\n",
          label, n, rms, static_cast<double>(peak), hz, static_cast<double>(maxDelta), expected, over05, over08, over20,
          spikes, 100.0 * zeros / n, longestZero, static_cast<double>(worst),
          static_cast<double>(worstAt) / rate);
}

int wmain() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* enumerator = nullptr;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                   reinterpret_cast<void**>(&enumerator));
  IMMDevice* game = FindRender(enumerator, L"AnyMix Game");
  IMMDevice* b1 = FindCapture(enumerator, L"AnyMix B1");
  if (game == nullptr || b1 == nullptr) {
    wprintf(L"missing endpoint game %d b1 %d\n", game != nullptr, b1 != nullptr);
    return 1;
  }
  Cap cap;
  if (!OpenCapture(b1, cap)) {
    return 2;
  }
  const bool listen = wcscmp(GetCommandLineW(), L"") != 0 && wcsstr(GetCommandLineW(), L"listen") != nullptr;
  const int rate = static_cast<int>(cap.format->nSamplesPerSec);
  const int want = rate * (listen ? 4 : 3);
  IAudioClient* play = nullptr;
  game->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&play));
  WAVEFORMATEX* playFormat = nullptr;
  play->GetMixFormat(&playFormat);
  const HRESULT playInit =
      play->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 10000000, 0, playFormat, nullptr);
  wprintf(L"play init 0x%08lx rate %lu bits %u\n", static_cast<unsigned long>(playInit), playFormat->nSamplesPerSec,
          playFormat->wBitsPerSample);
  HANDLE playEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  play->SetEventHandle(playEvent);
  IAudioRenderClient* render = nullptr;
  play->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render));
  UINT32 buffer = 0;
  play->GetBufferSize(&buffer);
  play->Start();
  const double tone = 1000.0;
  const float amp = 0.25f;
  double phase = 0;
  const double step = tone / playFormat->nSamplesPerSec;
  const ULONGLONG deadline = GetTickCount64() + (listen ? 4000 : 2500);
  int played = 0;
  while (GetTickCount64() < deadline && static_cast<int>(cap.left.size()) < want) {
    WaitForSingleObject(playEvent, 20);
    Pull(cap, want);
    UINT32 padding = 0;
    play->GetCurrentPadding(&padding);
    const UINT32 need = buffer - padding;
    if (need == 0 || listen) {
      continue;
    }
    BYTE* dest = nullptr;
    render->GetBuffer(need, &dest);
    const int channels = playFormat->nChannels;
    const bool isFloat = playFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                         (playFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                          reinterpret_cast<WAVEFORMATEXTENSIBLE*>(playFormat)->SubFormat.Data1 == 3);
    for (UINT32 i = 0; i < need; ++i) {
      const float s = amp * static_cast<float>(std::sin(phase * 6.283185307179586));
      phase += step;
      if (phase >= 1.0) {
        phase -= 1.0;
      }
      if (isFloat) {
        for (int c = 0; c < channels; ++c) {
          reinterpret_cast<float*>(dest)[i * channels + c] = s;
        }
      } else if (playFormat->wBitsPerSample == 16) {
        const int16_t pcm = static_cast<int16_t>(s * 32767.f);
        for (int c = 0; c < channels; ++c) {
          reinterpret_cast<int16_t*>(dest)[i * channels + c] = pcm;
        }
      }
    }
    render->ReleaseBuffer(need, 0);
    played += static_cast<int>(need);
  }
  Pull(cap, want);
  wprintf(L"played %d captured %zu listen %d\n", played, cap.left.size(), listen ? 1 : 0);
  Report("B1 L", cap.left, rate);
  Report("B1 R", cap.right, rate);
  play->Stop();
  cap.client->Stop();
  return 0;
}
