/*
 * DirectShow declarations for writing our own filters (a source and a sink), on top of
 * dshow_min.h. The Windows CE 5.0 Standard SDK has no
 * DirectShow headers or base classes, so everything is declared by hand.
 *
 * Vtables are flattened and must match Microsoft's strmif.h order exactly.
 */
#ifndef DSHOW_FILTERS_H
#define DSHOW_FILTERS_H

#include "dshow_min.h"

struct IMemAllocator;
struct IMediaSample;

typedef struct {
    GUID      majortype;
    GUID      subtype;
    BOOL      bFixedSizeSamples;
    BOOL      bTemporalCompression;
    ULONG     lSampleSize;
    GUID      formattype;
    IUnknown *pUnk;
    ULONG     cbFormat;
    BYTE     *pbFormat;
} AM_MEDIA_TYPE;

typedef enum { PINDIR_INPUT = 0, PINDIR_OUTPUT = 1 } PIN_DIRECTION;

typedef struct {
    IBaseFilter  *pFilter;
    PIN_DIRECTION dir;
    WCHAR         achName[128];
} PIN_INFO;

typedef struct {
    long cBuffers;
    long cbBuffer;
    long cbAlign;
    long cbPrefix;
} ALLOCATOR_PROPERTIES;

typedef struct {
    RECT             rcSource;
    RECT             rcTarget;
    DWORD            dwBitRate;
    DWORD            dwBitErrorRate;
    REFERENCE_TIME   AvgTimePerFrame;
    BITMAPINFOHEADER bmiHeader;
} VIDEOINFOHEADER;

typedef enum { State_Stopped = 0, State_Paused = 1, State_Running = 2 } FILTER_STATE;

struct IEnumMediaTypes : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Next(ULONG cMediaTypes, AM_MEDIA_TYPE **ppMediaTypes, ULONG *pcFetched) = 0;
    virtual HRESULT STDMETHODCALLTYPE Skip(ULONG cMediaTypes) = 0;
    virtual HRESULT STDMETHODCALLTYPE Reset() = 0;
    virtual HRESULT STDMETHODCALLTYPE Clone(IEnumMediaTypes **ppEnum) = 0;
};

struct IPin : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Connect(IPin *pReceivePin, const AM_MEDIA_TYPE *pmt) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReceiveConnection(IPin *pConnector, const AM_MEDIA_TYPE *pmt) = 0;
    virtual HRESULT STDMETHODCALLTYPE Disconnect() = 0;
    virtual HRESULT STDMETHODCALLTYPE ConnectedTo(IPin **pPin) = 0;
    virtual HRESULT STDMETHODCALLTYPE ConnectionMediaType(AM_MEDIA_TYPE *pmt) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryPinInfo(PIN_INFO *pInfo) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryDirection(PIN_DIRECTION *pPinDir) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryId(LPWSTR *Id) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryAccept(const AM_MEDIA_TYPE *pmt) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnumMediaTypes(IEnumMediaTypes **ppEnum) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryInternalConnections(IPin **apPin, ULONG *nPin) = 0;
    virtual HRESULT STDMETHODCALLTYPE EndOfStream() = 0;
    virtual HRESULT STDMETHODCALLTYPE BeginFlush() = 0;
    virtual HRESULT STDMETHODCALLTYPE EndFlush() = 0;
    virtual HRESULT STDMETHODCALLTYPE NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) = 0;
};

struct IEnumPins : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Next(ULONG cPins, IPin **ppPins, ULONG *pcFetched) = 0;
    virtual HRESULT STDMETHODCALLTYPE Skip(ULONG cPins) = 0;
    virtual HRESULT STDMETHODCALLTYPE Reset() = 0;
    virtual HRESULT STDMETHODCALLTYPE Clone(IEnumPins **ppEnum) = 0;
};

struct IMediaSample : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPointer(BYTE **ppBuffer) = 0;
    virtual long    STDMETHODCALLTYPE GetSize() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTime(REFERENCE_TIME *pTimeStart, REFERENCE_TIME *pTimeEnd) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetTime(REFERENCE_TIME *pTimeStart, REFERENCE_TIME *pTimeEnd) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsSyncPoint() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetSyncPoint(BOOL bIsSyncPoint) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsPreroll() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPreroll(BOOL bIsPreroll) = 0;
    virtual long    STDMETHODCALLTYPE GetActualDataLength() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetActualDataLength(long len) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMediaType(AM_MEDIA_TYPE **ppMediaType) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(AM_MEDIA_TYPE *pMediaType) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsDiscontinuity() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDiscontinuity(BOOL bDiscontinuity) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMediaTime(LONGLONG *pTimeStart, LONGLONG *pTimeEnd) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaTime(LONGLONG *pTimeStart, LONGLONG *pTimeEnd) = 0;
};

struct IMemAllocator : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetProperties(ALLOCATOR_PROPERTIES *pRequest, ALLOCATOR_PROPERTIES *pActual) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProperties(ALLOCATOR_PROPERTIES *pProps) = 0;
    virtual HRESULT STDMETHODCALLTYPE Commit() = 0;
    virtual HRESULT STDMETHODCALLTYPE Decommit() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetBuffer(IMediaSample **ppBuffer, REFERENCE_TIME *pStartTime,
                                                REFERENCE_TIME *pEndTime, DWORD dwFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReleaseBuffer(IMediaSample *pBuffer) = 0;
};

struct IMemInputPin : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetAllocator(IMemAllocator **ppAllocator) = 0;
    virtual HRESULT STDMETHODCALLTYPE NotifyAllocator(IMemAllocator *pAllocator, BOOL bReadOnly) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAllocatorRequirements(ALLOCATOR_PROPERTIES *pProps) = 0;
    virtual HRESULT STDMETHODCALLTYPE Receive(IMediaSample *pSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReceiveMultiple(IMediaSample **pSamples, long nSamples, long *nSamplesProcessed) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReceiveCanBlock() = 0;
};

#define DS_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    static const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }

DS_GUID(IID_IPersist_,      0x0000010c, 0x0000, 0x0000, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
DS_GUID(IID_IBaseFilter,    0x56a86895, 0x0ad4, 0x11ce, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70);
DS_GUID(IID_IPin,           0x56a86891, 0x0ad4, 0x11ce, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70);
DS_GUID(IID_IEnumPins,      0x56a86892, 0x0ad4, 0x11ce, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70);
DS_GUID(IID_IEnumMediaTypes,0x89c31040, 0x846b, 0x11ce, 0x97, 0xd3, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a);
DS_GUID(IID_IMemInputPin,   0x56a8689d, 0x0ad4, 0x11ce, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70);
DS_GUID(MEDIATYPE_Video,    0x73646976, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DS_GUID(FORMAT_VideoInfo,   0x05589f80, 0xc356, 0x11ce, 0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a);
/* FourCC subtypes: {XXXXXXXX-0000-0010-8000-00AA00389B71} with the FourCC as the first DWORD. */
DS_GUID(MEDIASUBTYPE_FOURCC_BASE, 0x00000000, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);

/* Alchemy filters (registry, and Iris.exe's CLSID_VREND). */
DS_GUID(CLSID_AlchemyVideoDecoder,  0x692100f0, 0x01c4, 0x4af0, 0xbd, 0xc2, 0xc8, 0xba, 0x5c, 0x5d, 0xed, 0x01);
DS_GUID(CLSID_AlchemyVideoRenderer, 0x0ba13ea1, 0x70e5, 0x11db, 0x96, 0x90, 0x00, 0xe0, 0x81, 0x61, 0x16, 0x5f);

#define VFW_E_NOT_CONNECTED       ((HRESULT)0x80040209L)
#define VFW_E_ALREADY_CONNECTED   ((HRESULT)0x80040204L)
#define VFW_E_NO_ALLOCATOR        ((HRESULT)0x8004020AL)
#define VFW_E_BUFFER_OVERFLOW     ((HRESULT)0x8004020DL)

#endif
