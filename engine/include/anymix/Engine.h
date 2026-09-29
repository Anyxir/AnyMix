#pragma once

#include "anymix/Types.h"
#include "anymix/VstHost.h"

#include <cstdint>
#include <string>
#include <vector>

namespace anymix {

struct EngineStats {
  uint64_t framesCaptured = 0;
  uint64_t framesRendered = 0;
  uint64_t blocks = 0;
  uint64_t renderUnderruns = 0;
  uint64_t captureUnderruns = 0;
};

class Engine {
 public:
  Engine();
  ~Engine();

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  bool Start(std::wstring& error);
  void Stop();
  bool Running() const;

  void SetCaptureDevice(int micIndex, const wchar_t* endpointId);
  void SetRenderDevice(int outputIndex, const wchar_t* endpointId);

  void SetGainDb(int strip, float db);
  void SetBusGainDb(int bus, float db);
  void SetBusMute(int bus, bool on);
  void SetBusMono(int bus, bool on);
  void SetBusSolo(int bus, bool on);
  void SetBassDb(int strip, float db);
  void SetTrebleDb(int strip, float db);
  void SetMute(int strip, bool on);
  void SetSolo(int strip, bool on);
  void SetMono(int strip, bool on);
  void SetPan(int strip, float pan);
  void SetInsert(int strip, bool on);
  void SetGate(int strip, bool on);
  void SetGateDepth(int strip, float depth);
  void SetGateLevel(int strip, float level);
  bool GateOpen(int strip) const;
  void SetRoute(int strip, int bus, bool on);

  float Peak(int strip) const;
  void PeakStereo(int strip, float& left, float& right) const;
  float BusPeak(int bus) const;
  void BusPeakStereo(int bus, float& left, float& right) const;
  EngineStats Stats() const;

  bool StartRecording(int bus, const wchar_t* folder, std::wstring& error);
  void StopRecording();
  bool Recording() const;
  const wchar_t* RecordingPath() const;
  void SetMonitorMask(uint32_t mask);
  void SetPlaybackMask(uint32_t mask);
  void PushPlayback(const float* interleaved, int frames);
  void ClearPlayback();

  bool DriverConnected() const;
  const wchar_t* DeviceError() const;
  void AsioLinkStatus(wchar_t* text, int capacity, int& fault) const;

  int AddVst(int strip, const wchar_t* path, std::wstring& error);
  void RemoveVst(int slot);
  void SetVstBypass(int slot, bool on);
  void SetVstGroup(int slot, int group);
  void SetVstGroupBypass(int group, bool on);
  void SetLocalVst(int strip, bool on);
  void ToggleVstEditor(int slot);
  void VstSlot(int slot, VstSlotView& out) const;
  bool CaptureVstState(int slot, std::vector<uint8_t>& out);
  void RestoreVstState(int slot, const uint8_t* data, size_t size);

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace anymix
