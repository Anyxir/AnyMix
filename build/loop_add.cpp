#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <cmath>
#include <cstdio>
int wmain() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* e = nullptr;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&e);
  IMMDevice* add = nullptr;
  IMMDeviceCollection* col = nullptr;
  e->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
  UINT n = 0; col->GetCount(&n);
  for (UINT i = 0; i < n && add == nullptr; ++i) {
    IMMDevice* d = nullptr; col->Item(i, &d);
    IPropertyStore* s = nullptr; d->OpenPropertyStore(STGM_READ, &s);
    PROPVARIANT name; PropVariantInit(&name);
    s->GetValue(PKEY_Device_FriendlyName, &name);
    if (name.vt == VT_LPWSTR && wcscmp(name.pwszVal, L"AnyMix Add (ASIOVADPRO Driver)") == 0) add = d;
    else d->Release();
    PropVariantClear(&name); s->Release();
  }
  IAudioClient* play = nullptr; add->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&play);
  WAVEFORMATEX* mix = nullptr; play->GetMixFormat(&mix);
  HRESULT hr = play->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, mix, nullptr);
  wprintf(L"play %08X rate %u\n", (unsigned)hr, mix->nSamplesPerSec);
  IAudioRenderClient* rc = nullptr; play->GetService(__uuidof(IAudioRenderClient), (void**)&rc);
  UINT32 buf = 0; play->GetBufferSize(&buf);
  IAudioClient* cap = nullptr; add->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&cap);
  const DWORD flags = 0x00020000 | 0x80000000;
  hr = cap->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 10000000, 0, mix, nullptr);
  wprintf(L"loop %08X\n", (unsigned)hr);
  IAudioCaptureClient* cc = nullptr; cap->GetService(__uuidof(IAudioCaptureClient), (void**)&cc);
  play->Start(); cap->Start();
  float peak = 0; double phase = 0; DWORD t0 = GetTickCount();
  while (GetTickCount() - t0 < 700) {
    UINT32 pad = 0; play->GetCurrentPadding(&pad);
    UINT32 need = buf > pad ? buf - pad : 0;
    if (need) {
      BYTE* dest = nullptr; rc->GetBuffer(need, &dest);
      float* s = (float*)dest;
      for (UINT32 i = 0; i < need; ++i) {
        float v = 0.25f * sinf((float)phase);
        phase += 2 * 3.14159265 * 440.0 / mix->nSamplesPerSec;
        s[i * mix->nChannels] = v; if (mix->nChannels > 1) s[i * mix->nChannels + 1] = v;
      }
      rc->ReleaseBuffer(need, 0);
    }
    UINT32 packet = 0; cc->GetNextPacketSize(&packet);
    while (packet) {
      BYTE* data = nullptr; UINT32 count = 0; DWORD f = 0;
      cc->GetBuffer(&data, &count, &f, nullptr, nullptr);
      if ((f & 2) == 0 && data) {
        float* s = (float*)data;
        UINT32 m = count * mix->nChannels;
        for (UINT32 i = 0; i < m; ++i) peak = fmaxf(peak, fabsf(s[i]));
      }
      cc->ReleaseBuffer(count); cc->GetNextPacketSize(&packet);
    }
    Sleep(5);
  }
  wprintf(L"peak %.5f\n", peak);
  return 0;
}
