#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cmath>

#include "anymix/Bridge.h"

int main() {
  HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, anymix::kAsioMappingName);
  if (map == nullptr) {
    std::printf("no mapping err=%lu\n", GetLastError());
    return 1;
  }
  auto* bridge = static_cast<anymix::AsioBridge*>(
      MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(anymix::AsioBridge)));
  if (bridge == nullptr) {
    std::printf("no view\n");
    return 1;
  }
  const uint32_t seq1 = bridge->sendSeq;
  const uint32_t ret1 = bridge->returnSeq;
  float peak[anymix::kAsioChannels]{};
  float retPeak[anymix::kAsioChannels]{};
  const int chans = anymix::kAsioChannels;
  for (int spin = 0; spin < 20; ++spin) {
    const uint32_t seq = bridge->sendSeq;
    const uint32_t ret = bridge->returnSeq;
    if (seq == 0 || ret == 0) {
      Sleep(100);
      continue;
    }
    const float* src = bridge->send[(seq - 1) & 1u];
    const float* back = bridge->ret[(ret - 1) & 1u];
    for (int i = 0; i < 512 * chans; ++i) {
      const int ch = i % chans;
      const float s = std::fabs(src[i]);
      const float r = std::fabs(back[i]);
      if (s > peak[ch]) peak[ch] = s;
      if (r > retPeak[ch]) retPeak[ch] = r;
    }
    Sleep(100);
  }
  std::printf("send %u -> %u  return %u -> %u  host=%u mask=%08x\n", seq1, bridge->sendSeq, ret1,
              bridge->returnSeq, bridge->hostRunning, bridge->outputMask);
  for (int ch = 0; ch < chans; ++ch) {
    std::printf("ch %d send %.4f return %.4f\n", ch, peak[ch], retPeak[ch]);
  }
  return 0;
}
