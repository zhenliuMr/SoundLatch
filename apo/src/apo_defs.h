// apo_defs.h - minimal APO (Audio Processing Object) interface definitions.
// Declared locally so the build does not depend on the WDK / audioenginebaseapo.h.
#pragma once
#include <windows.h>
#include <objbase.h>
#include <mmreg.h>
#include <cstdint>

typedef LONGLONG HNSTIME;

#ifndef STDMETHODCALLTYPE
#define STDMETHODCALLTYPE __stdcall
#endif

// ---------------------------------------------------------------- structures
struct APOInitBaseStruct {
    UINT32 cbSize;
    const GUID* clsid;
};

typedef enum APO_BUFFER_FLAGS {
    BUFFER_INVALID = 0,
    BUFFER_VALID = 1,
    BUFFER_SILENT = 2
} APO_BUFFER_FLAGS;

typedef enum APO_CONNECTION_BUFFER_TYPE {
    APO_CONNECTION_BUFFER_TYPE_ALLOCATED = 0,
    APO_CONNECTION_BUFFER_TYPE_EXTERNAL = 1,
    APO_CONNECTION_BUFFER_TYPE_DEPENDANT = 2
} APO_CONNECTION_BUFFER_TYPE;

typedef enum APO_FLAG {
    APO_FLAG_NONE = 0x0,
    APO_FLAG_INPLACE = 0x1,
    APO_FLAG_FRAMESPERSECOND_MUST_MATCH = 0x2,
    APO_FLAG_BITSPERSAMPLE_MUST_MATCH = 0x4,
    APO_FLAG_MIXER = 0x8,
    APO_FLAG_SAMPLESPERFRAME_MUST_MATCH = 0x10
} APO_FLAG;

struct IAudioMediaType;   // opaque for our purposes

struct APO_CONNECTION_DESCRIPTOR {
    APO_CONNECTION_BUFFER_TYPE Type;
    UINT_PTR pBuffer;
    UINT32 u32MaxFrameCount;
    IAudioMediaType* pFormat;
    UINT32 u32Signature;
};

struct APO_CONNECTION_PROPERTY {
    UINT_PTR pBuffer;
    UINT32 u32ValidFrameCount;
    APO_BUFFER_FLAGS BufferFlags;
    UINT32 u32Signature;
};

struct APO_REG_PROPERTIES {
    CLSID clsid;
    APO_FLAG Flags;
    WCHAR szFriendlyName[256];
    WCHAR szCopyrightInfo[256];
    UINT32 u32MajorVersion;
    UINT32 u32MinorVersion;
    UINT32 u32MinInputConnections;
    UINT32 u32MaxInputConnections;
    UINT32 u32MinOutputConnections;
    UINT32 u32MaxOutputConnections;
    UINT32 u32MaxInstances;
    UINT32 u32NumAPOInterfaces;
    IID iidAPOInterfaceList[1];
};

// ---------------------------------------------------------------- interfaces
struct IAudioProcessingObject : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Initialize(APOInitBaseStruct* pInit) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetLatency(HNSTIME* pTime) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRegistrationProperties(APO_REG_PROPERTIES** ppProps) = 0;
    virtual HRESULT STDMETHODCALLTYPE Uninitialize() = 0;
};

struct IAudioProcessingObjectConfiguration : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE LockForProcess(UINT32 u32NumInputConnections,
                                                     APO_CONNECTION_DESCRIPTOR** ppInputConnections,
                                                     UINT32 u32NumOutputConnections,
                                                     APO_CONNECTION_DESCRIPTOR** ppOutputConnections) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnlockForProcess() = 0;
};

struct IAudioProcessingObjectRT : public IAudioProcessingObject {
    virtual void STDMETHODCALLTYPE APOProcess(UINT32 u32NumInputConnections,
                                              APO_CONNECTION_PROPERTY** ppInputConnections,
                                              UINT32 u32NumOutputConnections,
                                              APO_CONNECTION_PROPERTY** ppOutputConnections) = 0;
    virtual UINT32 STDMETHODCALLTYPE CalcInputFrames(UINT32 u32OutputFrameCount) = 0;
    virtual UINT32 STDMETHODCALLTYPE CalcOutputFrames(UINT32 u32InputFrameCount) = 0;
};

// ---------------------------------------------------------------- guids
// {A5C6D1E0-3B7F-4C21-9E44-9E1D2C0A7781}
static const CLSID CLSID_SoundLockApo =
    {0xa5c6d1e0, 0x3b7f, 0x4c21, {0x9e, 0x44, 0x9e, 0x1d, 0x2c, 0x0a, 0x77, 0x81}};

// IAudioProcessingObject           {FD7F2B29-24D0-4B5C-B177-592C39F9CA10}
static const IID IID_IAudioProcessingObject =
    {0xfd7f2b29, 0x24d0, 0x4b5c, {0xb1, 0x77, 0x59, 0x2c, 0x39, 0xf9, 0xca, 0x10}};
// IAudioProcessingObjectConfiguration {FB1D5E2A-2B3C-4A9E-8E63-1C1E2B7F3A55}
static const IID IID_IAudioProcessingObjectConfiguration =
    {0xfb1d5e2a, 0x2b3c, 0x4a9e, {0x8e, 0x63, 0x1c, 0x1e, 0x2b, 0x7f, 0x3a, 0x55}};
// IAudioProcessingObjectRT     {9E1B2C3D-4A5F-4E6D-8B7A-0C1D2E3F4A5B}
static const IID IID_IAudioProcessingObjectRT =
    {0x9e1b2c3d, 0x4a5f, 0x4e6d, {0x8b, 0x7a, 0x0c, 0x1d, 0x2e, 0x3f, 0x4a, 0x5b}};
