#pragma once

#include "anymix/Types.h"

#include <atomic>
#include <string>

namespace anymix {

constexpr int kVstSlots = 8;
constexpr int kVstGroups = 4;

struct VstSlotView {
  wchar_t name[128]{};
  wchar_t path[520]{};
  int strip = -1;
  int group = -1;
  bool used = false;
  bool bypass = false;
  bool editor = false;
};

struct VstPlugin;

// Hosts VST2 and VST3 on a strip. Process() is the only call from the audio thread.
class VstRack {
 public:
  VstRack();
  ~VstRack();

  VstRack(const VstRack&) = delete;
  VstRack& operator=(const VstRack&) = delete;

  int Add(int strip, const wchar_t* path, std::wstring& error);
  void Remove(int slot);
  void SetBypass(int slot, bool on);
  void SetGroup(int slot, int group);
  void SetGroupBypass(int group, bool on);
  void SetLocal(int strip, bool on);
  void ToggleEditor(int slot);
  void Slot(int slot, VstSlotView& out) const;
  void Process(float* const* shaped, int strips, int frames);

 private:
  struct SlotState {
    std::atomic<VstPlugin*> plugin{nullptr};
    std::atomic<int> strip{-1};
    std::atomic<int> group{-1};
    std::atomic<bool> bypass{false};
  };
  SlotState slots_[kVstSlots];
  std::atomic<bool> localOn_[kStrips];
  std::atomic<bool> groupBypass_[kVstGroups];
};

}  // namespace anymix
