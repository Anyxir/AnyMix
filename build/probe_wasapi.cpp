#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>

static int TryInit(IMMDevice* device, DWORD flags) {
  IAudioClient* client = nullptr;
  HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
  if (FAILED(hr) || client == nullptr) {
    return static_cast<int>(hr);
  }
  WAVEFORMATEXTENSIBLE format{};
  format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  format.Format.nChannels = 2;
  format.Format.nSamplesPerSec = 48000;
  format.Format.wBitsPerSample = 32;
  format.Format.nBlockAlign = 8;
  format.Format.nAvgBytesPerSec = 48000 * 8;
  format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  format.Samples.wValidBitsPerSample = 32;
  format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  const GUID ieee = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
  format.SubFormat = ieee;
  const REFERENCE_TIME duration = (512 * 4 * 10000000LL) / 48000;
  hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, duration, 0, reinterpret_cast<WAVEFORMATEX*>(&format), nullptr);
  client->Release();
  return static_cast<int>(hr);
}

int wmain() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* en = nullptr;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                   reinterpret_cast<void**>(&en));
  IMMDeviceCollection* col = nullptr;
  en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col);
  UINT n = 0;
  col->GetCount(&n);
  wprintf(L"render count %u\n", n);
  for (UINT i = 0; i < n; ++i) {
    IMMDevice* dev = nullptr;
    col->Item(i, &dev);
    IPropertyStore* store = nullptr;
    dev->OpenPropertyStore(STGM_READ, &store);
    PROPVARIANT name;
    PropVariantInit(&name);
    store->GetValue(PKEY_Device_FriendlyName, &name);
    const wchar_t* text = name.vt == VT_LPWSTR ? name.pwszVal : L"";
    const bool interesting = wcsstr(text, L"Microphone") || wcsstr(text, L"Realtek") || wcsstr(text, L"CABLE") ||
                             wcsstr(text, L"AnyMix B1") || i < 3;
    if (interesting) {
      if (wcsstr(text, L"Microphone (Realtek") == nullptr) {
        PropVariantClear(&name);
        store->Release();
        dev->Release();
        continue;
      }
      IAudioSessionManager2* mgr = nullptr;
      dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&mgr));
      if (mgr != nullptr) {
        IAudioSessionEnumerator* sessions = nullptr;
        mgr->GetSessionEnumerator(&sessions);
        int count = 0;
        sessions->GetCount(&count);
        wprintf(L"sessions-before %d\n", count);
        for (int s = 0; s < count; ++s) {
          IAudioSessionControl* ctrl = nullptr;
          sessions->GetSession(s, &ctrl);
          IAudioSessionControl2* ctrl2 = nullptr;
          ctrl->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&ctrl2));
          DWORD pid = 0;
          AudioSessionState state = AudioSessionStateExpired;
          ctrl2->GetProcessId(&pid);
          ctrl2->GetState(&state);
          wchar_t image[520] = L"?";
          HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
          if (proc != nullptr) {
            DWORD size = 520;
            QueryFullProcessImageNameW(proc, 0, image, &size);
            CloseHandle(proc);
          }
          wprintf(L"  pid=%lu state=%d %ls\n", pid, static_cast<int>(state), image);
          ctrl2->Release();
          ctrl->Release();
        }
        sessions->Release();
        mgr->Release();
      }
      IAudioClient* client = nullptr;
      dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
      WAVEFORMATEX* mix = nullptr;
      client->GetMixFormat(&mix);
      wprintf(L"mix tag=%u ch=%u rate=%lu bits=%u\n", mix->wFormatTag, mix->nChannels, mix->nSamplesPerSec, mix->wBitsPerSample);
      WAVEFORMATEX* closest = nullptr;
      HRESULT sup = client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, mix, &closest);
      wprintf(L"mix supported %08X\n", static_cast<unsigned>(sup));
      HRESULT hr0 = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, mix, nullptr);
      wprintf(L"init mix flags0 %08X\n", static_cast<unsigned>(hr0));
      client->Release();
      wprintf(L"autoconvert %08X\n", static_cast<unsigned>(TryInit(dev, 0x80000000 | 0x08000000)));
      if (closest) CoTaskMemFree(closest);
      CoTaskMemFree(mix);
    }
    PropVariantClear(&name);
    store->Release();
    dev->Release();
  }
  col->Release();
  en->Release();
  return 0;
}
