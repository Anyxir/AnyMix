// Read-only: mix format of the default capture endpoint (console role).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <cstdio>

int main() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* enumerator = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
  if (FAILED(hr) || enumerator == nullptr) {
    std::printf("enumerator failed 0x%08lX\n", static_cast<unsigned long>(hr));
    if (SUCCEEDED(com)) {
      CoUninitialize();
    }
    return 1;
  }
  const struct {
    const wchar_t* label;
    ERole role;
  } roles[] = {{L"console", eConsole}, {L"multimedia", eMultimedia}, {L"communications", eCommunications}};
  for (const auto& entry : roles) {
    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eCapture, entry.role, &device);
    if (FAILED(hr) || device == nullptr) {
      std::printf("capture %ls: none (0x%08lX)\n", entry.label, static_cast<unsigned long>(hr));
      continue;
    }
    IAudioClient* client = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    if (SUCCEEDED(hr) && client != nullptr) {
      WAVEFORMATEX* mix = nullptr;
      hr = client->GetMixFormat(&mix);
      if (SUCCEEDED(hr) && mix != nullptr) {
        std::printf("capture %ls: rate=%lu ch=%u bits=%u\n", entry.label,
                    static_cast<unsigned long>(mix->nSamplesPerSec), mix->nChannels,
                    mix->wBitsPerSample);
        CoTaskMemFree(mix);
      }
      client->Release();
    }
    device->Release();
  }
  enumerator->Release();
  if (SUCCEEDED(com)) {
    CoUninitialize();
  }
  return 0;
}
