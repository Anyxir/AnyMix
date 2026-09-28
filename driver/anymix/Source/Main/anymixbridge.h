#pragma once

#include "common.h"

EXTERN_C_START

NTSTATUS AnyMixCreateControlDevice(PDRIVER_OBJECT DriverObject);
void AnyMixDeleteControlDevice();
void AnyMixLog(unsigned long step, NTSTATUS status);
void AnyMixLogPhases(NTSTATUS topo, NTSTATUS wave, NTSTATUS connect);
void AnyMixLogSub(unsigned long sub);
void AnyMixPushPlayback(eDeviceType type, const BYTE* pcm, ULONG bytes);
void AnyMixPullCapture(eDeviceType type, BYTE* pcm, ULONG bytes);
ULONG AnyMixCaptureBytesAvailable(eDeviceType type);

EXTERN_C_END
