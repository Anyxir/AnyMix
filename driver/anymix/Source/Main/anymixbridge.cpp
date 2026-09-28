#include "definitions.h"
#include <wdmsec.h>
#include "anymixbridge.h"

#define ANYMIX_RING_FRAMES 8192
#define ANYMIX_MAGIC 0x414D5831
#define ANYMIX_IOCTL_READ  0x001D6000
#define ANYMIX_IOCTL_WRITE 0x001DA004

#pragma pack(push, 4)
struct AnyMixRequest {
    ULONG magic;
    ULONG stream;
    ULONG frames;
    ULONG reserved;
    float samples[512 * 2];
};
#pragma pack(pop)

struct AnyMixRing {
    float samples[ANYMIX_RING_FRAMES * 2];
    ULONG read;
    ULONG write;
    KSPIN_LOCK lock;
};

static AnyMixRing g_playback[5];
static AnyMixRing g_capture[5];
static PDEVICE_OBJECT g_controlDevice = nullptr;
static PDRIVER_DISPATCH g_previousDeviceControl = nullptr;
static PDRIVER_DISPATCH g_previousCreate = nullptr;
static PDRIVER_DISPATCH g_previousClose = nullptr;

static int PlaybackIndex(eDeviceType type)
{
    if (type >= eAnyMixGame && type <= eAnyMixAdd) {
        return static_cast<int>(type - eAnyMixGame);
    }
    return -1;
}

static int CaptureIndex(eDeviceType type)
{
    if (type >= eAnyMixB1 && type <= eAnyMixB5) {
        return static_cast<int>(type - eAnyMixB1);
    }
    return -1;
}

static void PushFloat(AnyMixRing* ring, const float* interleaved, ULONG frames)
{
    KIRQL irql;
    KeAcquireSpinLock(&ring->lock, &irql);
    for (ULONG i = 0; i < frames; ++i) {
        const ULONG freeFrames = ANYMIX_RING_FRAMES - (ring->write - ring->read);
        if (freeFrames == 0) {
            ring->read++;
        }
        const ULONG index = ring->write % ANYMIX_RING_FRAMES;
        ring->samples[index * 2] = interleaved[i * 2];
        ring->samples[index * 2 + 1] = interleaved[i * 2 + 1];
        ring->write++;
    }
    KeReleaseSpinLock(&ring->lock, irql);
}

static ULONG PopFloat(AnyMixRing* ring, float* interleaved, ULONG frames)
{
    KIRQL irql;
    KeAcquireSpinLock(&ring->lock, &irql);
    const ULONG have = min(frames, ring->write - ring->read);
    for (ULONG i = 0; i < have; ++i) {
        const ULONG index = ring->read % ANYMIX_RING_FRAMES;
        interleaved[i * 2] = ring->samples[index * 2];
        interleaved[i * 2 + 1] = ring->samples[index * 2 + 1];
        ring->read++;
    }
    KeReleaseSpinLock(&ring->lock, irql);
    if (have < frames) {
        RtlZeroMemory(interleaved + have * 2, sizeof(float) * (frames - have) * 2);
    }
    return have;
}

static void Pcm16ToFloat(const BYTE* pcm, float* dst, ULONG bytes)
{
    const ULONG frames = bytes / 4;
    const SHORT* src = reinterpret_cast<const SHORT*>(pcm);
    for (ULONG i = 0; i < frames; ++i) {
        dst[i * 2] = static_cast<float>(src[i * 2]) / 32768.f;
        dst[i * 2 + 1] = static_cast<float>(src[i * 2 + 1]) / 32768.f;
    }
}

static void FloatToPcm32(const float* src, BYTE* pcm, ULONG bytes)
{
    const ULONG frames = bytes / 8;
    LONG* dst = reinterpret_cast<LONG*>(pcm);
    for (ULONG i = 0; i < frames; ++i) {
        const float left = max(-1.f, min(1.f, src[i * 2]));
        const float right = max(-1.f, min(1.f, src[i * 2 + 1]));
        dst[i * 2] = static_cast<LONG>(left * 2147483647.f);
        dst[i * 2 + 1] = static_cast<LONG>(right * 2147483647.f);
    }
}

ULONG AnyMixCaptureBytesAvailable(eDeviceType type)
{
    const int index = CaptureIndex(type);
    if (index < 0) {
        return 0;
    }
    KIRQL irql;
    KeAcquireSpinLock(&g_capture[index].lock, &irql);
    const ULONG have = g_capture[index].write - g_capture[index].read;
    KeReleaseSpinLock(&g_capture[index].lock, irql);
    const ULONG reserve = 960;
    if (have <= reserve) {
        return 0;
    }
    ULONG give = have - reserve;
    if (have > 6000) {
        give = have - 256;
    }
    return give * 8;
}

void AnyMixPushPlayback(eDeviceType type, const BYTE* pcm, ULONG bytes)
{
    const int index = PlaybackIndex(type);
    if (index < 0 || bytes < 4) {
        return;
    }
    float block[32 * 2];
    ULONG left = bytes;
    const BYTE* cursor = pcm;
    while (left >= 4) {
        const ULONG chunk = min(left, static_cast<ULONG>(sizeof(SHORT) * 2 * 32));
        const ULONG frames = chunk / 4;
        Pcm16ToFloat(cursor, block, chunk);
        PushFloat(&g_playback[index], block, frames);
        cursor += chunk;
        left -= chunk;
    }
}

void AnyMixPullCapture(eDeviceType type, BYTE* pcm, ULONG bytes)
{
    const int index = CaptureIndex(type);
    if (index < 0 || bytes < 4) {
        RtlZeroMemory(pcm, bytes);
        return;
    }
    float block[32 * 2];
    ULONG left = bytes;
    BYTE* cursor = pcm;
    while (left >= 8) {
        const ULONG aligned = left - (left % 8);
        const ULONG chunk = min(aligned, static_cast<ULONG>(8 * 32));
        if (chunk < 8) {
            break;
        }
        const ULONG frames = chunk / 8;
        PopFloat(&g_capture[index], block, frames);
        FloatToPcm32(block, cursor, chunk);
        cursor += chunk;
        left -= chunk;
    }
    if (left > 0) {
        RtlZeroMemory(cursor, left);
    }
}

static NTSTATUS AnyMixDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    if (DeviceObject != g_controlDevice) {
        return g_previousDeviceControl(DeviceObject, Irp);
    }
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR information = 0;
    if (stack->Parameters.DeviceIoControl.IoControlCode == ANYMIX_IOCTL_READ &&
        stack->Parameters.DeviceIoControl.InputBufferLength >= sizeof(AnyMixRequest) &&
        stack->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(AnyMixRequest)) {
        auto* request = static_cast<AnyMixRequest*>(Irp->AssociatedIrp.SystemBuffer);
        if (request->magic == ANYMIX_MAGIC && request->stream < 5) {
            request->frames = PopFloat(&g_playback[request->stream], request->samples, 512);
            status = STATUS_SUCCESS;
            information = sizeof(AnyMixRequest);
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
    } else if (stack->Parameters.DeviceIoControl.IoControlCode == ANYMIX_IOCTL_WRITE &&
               stack->Parameters.DeviceIoControl.InputBufferLength >= sizeof(AnyMixRequest)) {
        auto* request = static_cast<AnyMixRequest*>(Irp->AssociatedIrp.SystemBuffer);
        if (request->magic == ANYMIX_MAGIC && request->stream < 5 && request->frames <= 512) {
            PushFloat(&g_capture[request->stream], request->samples, request->frames);
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
    }
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS AnyMixCreate(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    if (DeviceObject != g_controlDevice) {
        const UCHAR major = IoGetCurrentIrpStackLocation(Irp)->MajorFunction;
        if (major == IRP_MJ_CLOSE && g_previousClose != nullptr) {
            return g_previousClose(DeviceObject, Irp);
        }
        if (g_previousCreate != nullptr) {
            return g_previousCreate(DeviceObject, Irp);
        }
    }
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS AnyMixCreateControlDevice(PDRIVER_OBJECT DriverObject)
{
    for (int i = 0; i < 5; ++i) {
        KeInitializeSpinLock(&g_playback[i].lock);
        KeInitializeSpinLock(&g_capture[i].lock);
        g_playback[i].read = g_playback[i].write = 0;
        g_capture[i].read = g_capture[i].write = 0;
    }
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\Device\\AnyMix");
    UNICODE_STRING link = RTL_CONSTANT_STRING(L"\\DosDevices\\AnyMix");
    UNICODE_STRING sddl = RTL_CONSTANT_STRING(L"D:P(A;;GA;;;SY)(A;;GRGWGX;;;BA)(A;;GRGWGX;;;WD)");
    NTSTATUS status = IoCreateDeviceSecure(DriverObject, 0, &name, FILE_DEVICE_SOUND, 0, FALSE, &sddl,
                                           nullptr, &g_controlDevice);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    g_controlDevice->Flags |= DO_BUFFERED_IO;
    g_controlDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    status = IoCreateSymbolicLink(&link, &name);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(g_controlDevice);
        g_controlDevice = nullptr;
        return status;
    }
    g_previousDeviceControl = DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL];
    g_previousCreate = DriverObject->MajorFunction[IRP_MJ_CREATE];
    g_previousClose = DriverObject->MajorFunction[IRP_MJ_CLOSE];
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = AnyMixDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = AnyMixCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = AnyMixCreate;
    return STATUS_SUCCESS;
}

void AnyMixLogPhases(NTSTATUS topo, NTSTATUS wave, NTSTATUS connect)
{
    UNICODE_STRING path = RTL_CONSTANT_STRING(L"\\Registry\\Machine\\SOFTWARE\\AnyMix");
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, nullptr);
    HANDLE key = nullptr;
    ULONG disposition = 0;
    if (!NT_SUCCESS(ZwCreateKey(&key, KEY_SET_VALUE, &attributes, 0, nullptr, REG_OPTION_VOLATILE, &disposition))) {
        return;
    }
    UNICODE_STRING topoName = RTL_CONSTANT_STRING(L"Topo");
    UNICODE_STRING waveName = RTL_CONSTANT_STRING(L"Wave");
    UNICODE_STRING connectName = RTL_CONSTANT_STRING(L"Connect");
    ULONG topoValue = static_cast<ULONG>(topo);
    ULONG waveValue = static_cast<ULONG>(wave);
    ULONG connectValue = static_cast<ULONG>(connect);
    ZwSetValueKey(key, &topoName, 0, REG_DWORD, &topoValue, sizeof(topoValue));
    ZwSetValueKey(key, &waveName, 0, REG_DWORD, &waveValue, sizeof(waveValue));
    ZwSetValueKey(key, &connectName, 0, REG_DWORD, &connectValue, sizeof(connectValue));
    ZwClose(key);
}

void AnyMixLogSub(unsigned long sub)
{
    UNICODE_STRING path = RTL_CONSTANT_STRING(L"\\Registry\\Machine\\SOFTWARE\\AnyMix");
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, nullptr);
    HANDLE key = nullptr;
    ULONG disposition = 0;
    if (!NT_SUCCESS(ZwCreateKey(&key, KEY_SET_VALUE, &attributes, 0, nullptr, REG_OPTION_VOLATILE, &disposition))) {
        return;
    }
    UNICODE_STRING subName = RTL_CONSTANT_STRING(L"Sub");
    ZwSetValueKey(key, &subName, 0, REG_DWORD, &sub, sizeof(sub));
    ZwClose(key);
}

void AnyMixLog(unsigned long step, NTSTATUS status)
{
    UNICODE_STRING path = RTL_CONSTANT_STRING(L"\\Registry\\Machine\\SOFTWARE\\AnyMix");
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, nullptr);
    HANDLE key = nullptr;
    ULONG disposition = 0;
    if (!NT_SUCCESS(ZwCreateKey(&key, KEY_SET_VALUE, &attributes, 0, nullptr, REG_OPTION_VOLATILE, &disposition))) {
        return;
    }
    UNICODE_STRING stepName = RTL_CONSTANT_STRING(L"Step");
    ZwSetValueKey(key, &stepName, 0, REG_DWORD, &step, sizeof(step));
    UNICODE_STRING statusName = RTL_CONSTANT_STRING(L"Status");
    ULONG statusValue = static_cast<ULONG>(status);
    ZwSetValueKey(key, &statusName, 0, REG_DWORD, &statusValue, sizeof(statusValue));
    ZwClose(key);
}

void AnyMixDeleteControlDevice()
{
    UNICODE_STRING link = RTL_CONSTANT_STRING(L"\\DosDevices\\AnyMix");
    IoDeleteSymbolicLink(&link);
    if (g_controlDevice != nullptr) {
        IoDeleteDevice(g_controlDevice);
        g_controlDevice = nullptr;
    }
}
