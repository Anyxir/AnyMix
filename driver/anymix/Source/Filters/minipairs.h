/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    minipairs.h

Abstract:

    Local audio endpoint filter definitions. 
--*/

#ifndef _SIMPLEAUDIOSAMPLE_MINIPAIRS_H_
#define _SIMPLEAUDIOSAMPLE_MINIPAIRS_H_

#include "speakertopo.h"
#include "speakertoptable.h"
#include "speakerwavtable.h"

#include "micarraytopo.h"
#include "micarray1toptable.h"
#include "micarraywavtable.h"


NTSTATUS
CreateMiniportWaveRTSimpleAudioSample
( 
    _Out_       PUNKNOWN *,
    _In_        REFCLSID,
    _In_opt_    PUNKNOWN,
    _In_        POOL_FLAGS,
    _In_        PUNKNOWN,
    _In_opt_    PVOID,
    _In_        PENDPOINT_MINIPAIR
);

NTSTATUS
CreateMiniportTopologySimpleAudioSample
( 
    _Out_       PUNKNOWN *,
    _In_        REFCLSID,
    _In_opt_    PUNKNOWN,
    _In_        POOL_FLAGS,
    _In_        PUNKNOWN,
    _In_opt_    PVOID,
    _In_        PENDPOINT_MINIPAIR
);

//
// Render miniports.
//

/*********************************************************************
* Topology/Wave bridge connection for speaker (internal)             *
*                                                                    *
*              +------+                +------+                      *
*              | Wave |                | Topo |                      *
*              |      |                |      |                      *
* System   --->|0    1|--------------->|0    1|---> Line Out         *
*              |      |                |      |                      *
*              +------+                +------+                      *
*********************************************************************/
static
PHYSICALCONNECTIONTABLE SpeakerTopologyPhysicalConnections[] =
{
    {
        KSPIN_TOPO_WAVEOUT_SOURCE,  // TopologyIn
        KSPIN_WAVE_RENDER3_SOURCE,   // WaveOut
        CONNECTIONTYPE_WAVE_OUTPUT
    }
};

static
ENDPOINT_MINIPAIR SpeakerMiniports =
{
    eAnyMixGame,
    L"TopoGame",
    NULL,
    CreateMiniportTopologySimpleAudioSample,
    &SpeakerTopoMiniportFilterDescriptor,
    0, NULL,
    L"WaveGame",
    NULL,
    CreateMiniportWaveRTSimpleAudioSample,
    &SpeakerWaveMiniportFilterDescriptor,
    0,
    NULL,
    SPEAKER_DEVICE_MAX_CHANNELS,
    SpeakerPinDeviceFormatsAndModes,
    SIZEOF_ARRAY(SpeakerPinDeviceFormatsAndModes),
    SpeakerTopologyPhysicalConnections,
    SIZEOF_ARRAY(SpeakerTopologyPhysicalConnections),
    ENDPOINT_NO_FLAGS,
};

static ENDPOINT_MINIPAIR ChatMiniports =
{
    eAnyMixChat,
    L"TopoChat",
    NULL,
    CreateMiniportTopologySimpleAudioSample,
    &SpeakerTopoMiniportFilterDescriptor,
    0, NULL,
    L"WaveChat",
    NULL,
    CreateMiniportWaveRTSimpleAudioSample,
    &SpeakerWaveMiniportFilterDescriptor,
    0, NULL,
    SPEAKER_DEVICE_MAX_CHANNELS,
    SpeakerPinDeviceFormatsAndModes,
    SIZEOF_ARRAY(SpeakerPinDeviceFormatsAndModes),
    SpeakerTopologyPhysicalConnections,
    SIZEOF_ARRAY(SpeakerTopologyPhysicalConnections),
    ENDPOINT_NO_FLAGS,
};

static ENDPOINT_MINIPAIR MusicMiniports =
{
    eAnyMixMusic,
    L"TopoMusic",
    NULL,
    CreateMiniportTopologySimpleAudioSample,
    &SpeakerTopoMiniportFilterDescriptor,
    0, NULL,
    L"WaveMusic",
    NULL,
    CreateMiniportWaveRTSimpleAudioSample,
    &SpeakerWaveMiniportFilterDescriptor,
    0, NULL,
    SPEAKER_DEVICE_MAX_CHANNELS,
    SpeakerPinDeviceFormatsAndModes,
    SIZEOF_ARRAY(SpeakerPinDeviceFormatsAndModes),
    SpeakerTopologyPhysicalConnections,
    SIZEOF_ARRAY(SpeakerTopologyPhysicalConnections),
    ENDPOINT_NO_FLAGS,
};

static ENDPOINT_MINIPAIR AuxMiniports =
{
    eAnyMixAux,
    L"TopoAux",
    NULL,
    CreateMiniportTopologySimpleAudioSample,
    &SpeakerTopoMiniportFilterDescriptor,
    0, NULL,
    L"WaveAux",
    NULL,
    CreateMiniportWaveRTSimpleAudioSample,
    &SpeakerWaveMiniportFilterDescriptor,
    0, NULL,
    SPEAKER_DEVICE_MAX_CHANNELS,
    SpeakerPinDeviceFormatsAndModes,
    SIZEOF_ARRAY(SpeakerPinDeviceFormatsAndModes),
    SpeakerTopologyPhysicalConnections,
    SIZEOF_ARRAY(SpeakerTopologyPhysicalConnections),
    ENDPOINT_NO_FLAGS,
};

static ENDPOINT_MINIPAIR AddMiniports =
{
    eAnyMixAdd,
    L"TopoAdd",
    NULL,
    CreateMiniportTopologySimpleAudioSample,
    &SpeakerTopoMiniportFilterDescriptor,
    0, NULL,
    L"WaveAdd",
    NULL,
    CreateMiniportWaveRTSimpleAudioSample,
    &SpeakerWaveMiniportFilterDescriptor,
    0, NULL,
    SPEAKER_DEVICE_MAX_CHANNELS,
    SpeakerPinDeviceFormatsAndModes,
    SIZEOF_ARRAY(SpeakerPinDeviceFormatsAndModes),
    SpeakerTopologyPhysicalConnections,
    SIZEOF_ARRAY(SpeakerTopologyPhysicalConnections),
    ENDPOINT_NO_FLAGS,
};

//
// Capture miniports.
//

/*********************************************************************
* Topology/Wave bridge connection for mic array  1 (front)           *
*                                                                    *
*              +------+    +------+                                  *
*              | Topo |    | Wave |                                  *
*              |      |    |      |                                  *
*  Mic in  --->|0    1|===>|0    1|---> Capture Host Pin             *
*              |      |    |      |                                  *
*              +------+    +------+                                  *
*********************************************************************/
static
PHYSICALCONNECTIONTABLE MicArray1TopologyPhysicalConnections[] =
{
    {
        KSPIN_TOPO_BRIDGE,          // TopologyOut
        KSPIN_WAVE_BRIDGE,          // WaveIn
        CONNECTIONTYPE_TOPOLOGY_OUTPUT
    }
};

static
ENDPOINT_MINIPAIR MicArray1Miniports =
{
    eAnyMixB1,
    L"TopoB1",
    NULL,
    CreateMicArrayMiniportTopology,
    &MicArray1TopoMiniportFilterDescriptor,
    0, NULL,
    L"WaveB1",
    NULL,
    CreateMiniportWaveRTSimpleAudioSample,
    &MicArrayWaveMiniportFilterDescriptor,
    0,
    NULL,
    MICARRAY_DEVICE_MAX_CHANNELS,
    MicArrayPinDeviceFormatsAndModes,
    SIZEOF_ARRAY(MicArrayPinDeviceFormatsAndModes),
    MicArray1TopologyPhysicalConnections,
    SIZEOF_ARRAY(MicArray1TopologyPhysicalConnections),
    ENDPOINT_NO_FLAGS,
};

static ENDPOINT_MINIPAIR B2Miniports =
{
    eAnyMixB2,
    L"TopoB2",
    NULL,
    CreateMicArrayMiniportTopology,
    &MicArray1TopoMiniportFilterDescriptor,
    0, NULL,
    L"WaveB2",
    NULL,
    CreateMiniportWaveRTSimpleAudioSample,
    &MicArrayWaveMiniportFilterDescriptor,
    0, NULL,
    MICARRAY_DEVICE_MAX_CHANNELS,
    MicArrayPinDeviceFormatsAndModes,
    SIZEOF_ARRAY(MicArrayPinDeviceFormatsAndModes),
    MicArray1TopologyPhysicalConnections,
    SIZEOF_ARRAY(MicArray1TopologyPhysicalConnections),
    ENDPOINT_NO_FLAGS,
};

static ENDPOINT_MINIPAIR B3Miniports =
{
    eAnyMixB3,
    L"TopoB3",
    NULL,
    CreateMicArrayMiniportTopology,
    &MicArray1TopoMiniportFilterDescriptor,
    0, NULL,
    L"WaveB3",
    NULL,
    CreateMiniportWaveRTSimpleAudioSample,
    &MicArrayWaveMiniportFilterDescriptor,
    0, NULL,
    MICARRAY_DEVICE_MAX_CHANNELS,
    MicArrayPinDeviceFormatsAndModes,
    SIZEOF_ARRAY(MicArrayPinDeviceFormatsAndModes),
    MicArray1TopologyPhysicalConnections,
    SIZEOF_ARRAY(MicArray1TopologyPhysicalConnections),
    ENDPOINT_NO_FLAGS,
};

static ENDPOINT_MINIPAIR B4Miniports =
{
    eAnyMixB4,
    L"TopoB4",
    NULL,
    CreateMicArrayMiniportTopology,
    &MicArray1TopoMiniportFilterDescriptor,
    0, NULL,
    L"WaveB4",
    NULL,
    CreateMiniportWaveRTSimpleAudioSample,
    &MicArrayWaveMiniportFilterDescriptor,
    0, NULL,
    MICARRAY_DEVICE_MAX_CHANNELS,
    MicArrayPinDeviceFormatsAndModes,
    SIZEOF_ARRAY(MicArrayPinDeviceFormatsAndModes),
    MicArray1TopologyPhysicalConnections,
    SIZEOF_ARRAY(MicArray1TopologyPhysicalConnections),
    ENDPOINT_NO_FLAGS,
};

static ENDPOINT_MINIPAIR B5Miniports =
{
    eAnyMixB5,
    L"TopoB5",
    NULL,
    CreateMicArrayMiniportTopology,
    &MicArray1TopoMiniportFilterDescriptor,
    0, NULL,
    L"WaveB5",
    NULL,
    CreateMiniportWaveRTSimpleAudioSample,
    &MicArrayWaveMiniportFilterDescriptor,
    0, NULL,
    MICARRAY_DEVICE_MAX_CHANNELS,
    MicArrayPinDeviceFormatsAndModes,
    SIZEOF_ARRAY(MicArrayPinDeviceFormatsAndModes),
    MicArray1TopologyPhysicalConnections,
    SIZEOF_ARRAY(MicArray1TopologyPhysicalConnections),
    ENDPOINT_NO_FLAGS,
};


//=============================================================================
//
// Render miniport pairs. NOTE: the split of render and capture is arbitrary and
// unnessary, this array could contain capture endpoints.
//
static
PENDPOINT_MINIPAIR  g_RenderEndpoints[] = 
{
    &SpeakerMiniports,
    &ChatMiniports,
    &MusicMiniports,
    &AuxMiniports,
    &AddMiniports,
};

#define g_cRenderEndpoints  (SIZEOF_ARRAY(g_RenderEndpoints))

//=============================================================================
//
// Capture miniport pairs. NOTE: the split of render and capture is arbitrary and
// unnessary, this array could contain render endpoints.
//
static
PENDPOINT_MINIPAIR  g_CaptureEndpoints[] =
{
    &MicArray1Miniports,
    &B2Miniports,
    &B3Miniports,
    &B4Miniports,
    &B5Miniports,
};

#define g_cCaptureEndpoints (SIZEOF_ARRAY(g_CaptureEndpoints))

//=============================================================================
//
// Total miniports = # endpoints * 2 (topology + wave).
//
#define g_MaxMiniports  ((g_cRenderEndpoints + g_cCaptureEndpoints) * 2)

inline void AnyMixCloneFilter(PCFILTER_DESCRIPTOR* dstDesc,
    PCPIN_DESCRIPTOR* dstPins,
    PCNODE_DESCRIPTOR* dstNodes,
    PCCONNECTION_DESCRIPTOR* dstConnections,
    PCAUTOMATION_TABLE* dstAutomation,
    PCPROPERTY_ITEM* dstProps,
    ULONG pinCap,
    ULONG nodeCap,
    ULONG connectionCap,
    ULONG propCap,
    const PCFILTER_DESCRIPTOR* src)
{
    *dstDesc = *src;
    ULONG pinCount = src->PinCount;
    if (pinCount > pinCap) pinCount = pinCap;
    for (ULONG i = 0; i < pinCount; ++i) dstPins[i] = src->Pins[i];
    dstDesc->Pins = dstPins;
    dstDesc->PinCount = pinCount;

    ULONG nodeCount = src->NodeCount;
    if (nodeCount > nodeCap) nodeCount = nodeCap;
    if (src->Nodes != NULL)
    {
        for (ULONG i = 0; i < nodeCount; ++i) dstNodes[i] = src->Nodes[i];
        dstDesc->Nodes = dstNodes;
        dstDesc->NodeCount = nodeCount;
    }

    ULONG connectionCount = src->ConnectionCount;
    if (connectionCount > connectionCap) connectionCount = connectionCap;
    if (src->Connections != NULL)
    {
        for (ULONG i = 0; i < connectionCount; ++i) dstConnections[i] = src->Connections[i];
        dstDesc->Connections = dstConnections;
        dstDesc->ConnectionCount = connectionCount;
    }

    UNREFERENCED_PARAMETER(dstAutomation);
    UNREFERENCED_PARAMETER(dstProps);
    UNREFERENCED_PARAMETER(propCap);
}

static PCFILTER_DESCRIPTOR g_RenderTopoDesc[4];
static PCFILTER_DESCRIPTOR g_RenderWaveDesc[4];
static PCPIN_DESCRIPTOR g_RenderTopoPins[4][8];
static PCPIN_DESCRIPTOR g_RenderWavePins[4][8];
static PCNODE_DESCRIPTOR g_RenderTopoNodes[4][8];
static PCNODE_DESCRIPTOR g_RenderWaveNodes[4][8];
static PCCONNECTION_DESCRIPTOR g_RenderTopoConnections[4][8];
static PCCONNECTION_DESCRIPTOR g_RenderWaveConnections[4][8];
static PCAUTOMATION_TABLE g_RenderTopoAutomation[4];
static PCAUTOMATION_TABLE g_RenderWaveAutomation[4];
static PCPROPERTY_ITEM g_RenderTopoProps[4][8];
static PCPROPERTY_ITEM g_RenderWaveProps[4][8];
static PHYSICALCONNECTIONTABLE g_RenderConnections[4];
static int g_RenderSlot = 0;

static PCFILTER_DESCRIPTOR g_CaptureTopoDesc[4];
static PCFILTER_DESCRIPTOR g_CaptureWaveDesc[4];
static PCPIN_DESCRIPTOR g_CaptureTopoPins[4][8];
static PCPIN_DESCRIPTOR g_CaptureWavePins[4][8];
static PCNODE_DESCRIPTOR g_CaptureTopoNodes[4][8];
static PCNODE_DESCRIPTOR g_CaptureWaveNodes[4][8];
static PCCONNECTION_DESCRIPTOR g_CaptureTopoConnections[4][8];
static PCCONNECTION_DESCRIPTOR g_CaptureWaveConnections[4][8];
static PCAUTOMATION_TABLE g_CaptureTopoAutomation[4];
static PCAUTOMATION_TABLE g_CaptureWaveAutomation[4];
static PCPROPERTY_ITEM g_CaptureTopoProps[4][8];
static PCPROPERTY_ITEM g_CaptureWaveProps[4][8];
static PHYSICALCONNECTIONTABLE g_CaptureConnections[4];
static int g_CaptureSlot = 0;

inline void AnyMixPrivatizeRender(ENDPOINT_MINIPAIR& pair, eDeviceType type, PWSTR topoName, PWSTR waveName)
{
    int index = g_RenderSlot++;
    if (index > 3) index = 3;

    pair.DeviceType = type;
    pair.TopoName = topoName;
    pair.WaveName = waveName;
    pair.TopoCreateCallback = SpeakerMiniports.TopoCreateCallback;
    pair.WaveCreateCallback = SpeakerMiniports.WaveCreateCallback;
    AnyMixCloneFilter(&g_RenderTopoDesc[index], g_RenderTopoPins[index], g_RenderTopoNodes[index], g_RenderTopoConnections[index],
        &g_RenderTopoAutomation[index], g_RenderTopoProps[index], 8, 8, 8, 8, SpeakerMiniports.TopoDescriptor);
    AnyMixCloneFilter(&g_RenderWaveDesc[index], g_RenderWavePins[index], g_RenderWaveNodes[index], g_RenderWaveConnections[index],
        &g_RenderWaveAutomation[index], g_RenderWaveProps[index], 8, 8, 8, 8, SpeakerMiniports.WaveDescriptor);
    g_RenderConnections[index] = SpeakerMiniports.PhysicalConnections[0];
    pair.TopoDescriptor = &g_RenderTopoDesc[index];
    pair.WaveDescriptor = &g_RenderWaveDesc[index];
    pair.PhysicalConnections = &g_RenderConnections[index];
    pair.PhysicalConnectionCount = 1;
}

inline void AnyMixPrivatizeCapture(ENDPOINT_MINIPAIR& pair, eDeviceType type, PWSTR topoName, PWSTR waveName)
{
    int index = g_CaptureSlot++;
    if (index > 3) index = 3;

    pair.DeviceType = type;
    pair.TopoName = topoName;
    pair.WaveName = waveName;
    pair.TopoCreateCallback = MicArray1Miniports.TopoCreateCallback;
    pair.WaveCreateCallback = MicArray1Miniports.WaveCreateCallback;
    AnyMixCloneFilter(&g_CaptureTopoDesc[index], g_CaptureTopoPins[index], g_CaptureTopoNodes[index], g_CaptureTopoConnections[index],
        &g_CaptureTopoAutomation[index], g_CaptureTopoProps[index], 8, 8, 8, 8, MicArray1Miniports.TopoDescriptor);
    AnyMixCloneFilter(&g_CaptureWaveDesc[index], g_CaptureWavePins[index], g_CaptureWaveNodes[index], g_CaptureWaveConnections[index],
        &g_CaptureWaveAutomation[index], g_CaptureWaveProps[index], 8, 8, 8, 8, MicArray1Miniports.WaveDescriptor);
    g_CaptureConnections[index] = MicArray1Miniports.PhysicalConnections[0];
    pair.TopoDescriptor = &g_CaptureTopoDesc[index];
    pair.WaveDescriptor = &g_CaptureWaveDesc[index];
    pair.PhysicalConnections = &g_CaptureConnections[index];
    pair.PhysicalConnectionCount = 1;
}

inline void AnyMixBindEndpoints()
{
}

#endif // _SIMPLEAUDIOSAMPLE_MINIPAIRS_H_
