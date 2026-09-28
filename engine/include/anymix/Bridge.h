#pragma once

#include <cstdint>

// Shared with the AnyMix Insert ASIO DLL. Plain layout, no C++ objects.
// Created by the engine as Local\AnyMix.AsioInsert.

namespace anymix {

constexpr uint32_t kAsioMagic = 0x414D4958u;
constexpr uint32_t kAsioVersion = 3;
constexpr int kAsioChannels = 14;  // Game..Add plus Mic 1 and Mic 2
constexpr int kAsioBufferSlots = 8;
static_assert((kAsioBufferSlots & (kAsioBufferSlots - 1)) == 0, "ASIO slots must be a power of two");

#pragma pack(push, 4)
struct AsioBridge {
  uint32_t magic;
  uint32_t version;
  uint32_t bufferFrames;
  uint32_t channels;
  volatile uint32_t sendSeq;
  volatile uint32_t returnSeq;
  volatile uint32_t hostRunning;
  volatile uint32_t outputMask;  // bit N set when the host opened output channel N
  float send[kAsioBufferSlots][512 * kAsioChannels];
  float ret[kAsioBufferSlots][512 * kAsioChannels];
};
#pragma pack(pop)

constexpr wchar_t kAsioMappingName[] = L"Local\\AnyMix.AsioInsert3";
constexpr wchar_t kAsioTickName[] = L"Local\\AnyMix.AsioInsert3.Tick";

// Engine <-> kernel driver. The driver device is opened only if it exists.
constexpr wchar_t kDriverDevicePath[] = L"\\\\.\\AnyMix";
constexpr uint32_t kDriverMagic = 0x414D5831u;

#pragma pack(push, 4)
struct DriverBufferRequest {
  uint32_t magic;
  uint32_t stream;  // 0..4
  uint32_t frames;  // <= 512
  uint32_t reserved;
  float samples[512 * 2];
};
#pragma pack(pop)

constexpr unsigned long kAnyMixDeviceType = 0x0000001D;  // FILE_DEVICE_SOUND

inline constexpr unsigned long AnyMixCtlCode(unsigned long function, unsigned long access) {
  return (kAnyMixDeviceType << 16) | (access << 14) | (function << 2) | 0u;
}

// FILE_READ_DATA = 1, FILE_WRITE_DATA = 2, METHOD_BUFFERED = 0
constexpr unsigned long kIoctlReadPlayback = (0x0000001Du << 16) | (1u << 14) | (0x800u << 2);
constexpr unsigned long kIoctlWriteCapture = (0x0000001Du << 16) | (2u << 14) | (0x801u << 2);

}  // namespace anymix
