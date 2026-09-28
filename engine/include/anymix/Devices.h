#pragma once

#include "anymix/Types.h"

#include <string>

namespace anymix {

int EnumRenderDevices(DeviceInfo* out, int capacity);
int EnumCaptureDevices(DeviceInfo* out, int capacity);
bool DefaultRenderId(wchar_t* id, int capacity, wchar_t* name, int nameCapacity);
bool DefaultCaptureId(wchar_t* id, int capacity, wchar_t* name, int nameCapacity);

int EnumSessions(const wchar_t* endpointId, SessionInfo* out, int capacity, std::wstring& error);
void SampleSessionPeaks(const wchar_t* endpointId, SessionInfo* sessions, int count);
bool SetSessionMute(const wchar_t* endpointId, unsigned long sessionId, bool mute, std::wstring& error);
bool SetSessionVolume(const wchar_t* endpointId, unsigned long sessionId, float level, std::wstring& error);
bool MoveSession(unsigned long processId, const wchar_t* endpointId, std::wstring& error);

}  // namespace anymix
