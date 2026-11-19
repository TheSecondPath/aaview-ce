/*
 * Minimal DirectShow declarations for aaview. The Windows CE 5.0 Standard SDK
 * ships no DirectShow headers, so the few interfaces we use are declared here.
 *
 * Interfaces are flattened (base-interface methods listed first), so each
 * vtable must match Microsoft's strmif.h / control.h order exactly. Check
 * these first if a call crashes or returns nonsense.
 */
#ifndef DSHOW_MIN_H
#define DSHOW_MIN_H

#include <windows.h>
#include <ole2.h>

typedef LONGLONG REFERENCE_TIME;

struct IFilterGraph;
struct IPin;
struct IEnumPins;
struct IReferenceClock;

typedef struct {
    WCHAR         achName[128];
    IFilterGraph *pGraph;
} FILTER_INFO;

/* IPersist + IMediaFilter + IBaseFilter */
struct IBaseFilter : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetClassID(CLSID *pClassID) = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop() = 0;
    virtual HRESULT STDMETHODCALLTYPE Pause() = 0;
    virtual HRESULT STDMETHODCALLTYPE Run(REFERENCE_TIME tStart) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetState(DWORD dwMilliSecsTimeout, int *State) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetSyncSource(IReferenceClock *pClock) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSyncSource(IReferenceClock **pClock) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnumPins(IEnumPins **ppEnum) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindPin(LPCWSTR Id, IPin **ppPin) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryFilterInfo(FILTER_INFO *pInfo) = 0;
    virtual HRESULT STDMETHODCALLTYPE JoinFilterGraph(IFilterGraph *pGraph, LPCWSTR pName) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryVendorInfo(LPWSTR *pVendorInfo) = 0;
};

struct IEnumFilters : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Next(ULONG cFilters, IBaseFilter **ppFilter, ULONG *pcFetched) = 0;
    virtual HRESULT STDMETHODCALLTYPE Skip(ULONG cFilters) = 0;
    virtual HRESULT STDMETHODCALLTYPE Reset() = 0;
    virtual HRESULT STDMETHODCALLTYPE Clone(IEnumFilters **ppEnum) = 0;
};

/* IFilterGraph + IGraphBuilder */
struct IGraphBuilder : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE AddFilter(IBaseFilter *pFilter, LPCWSTR pName) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveFilter(IBaseFilter *pFilter) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnumFilters(IEnumFilters **ppEnum) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindFilterByName(LPCWSTR pName, IBaseFilter **ppFilter) = 0;
    virtual HRESULT STDMETHODCALLTYPE ConnectDirect(IPin *ppinOut, IPin *ppinIn, const void *pmt) = 0;
    virtual HRESULT STDMETHODCALLTYPE Reconnect(IPin *ppin) = 0;
    virtual HRESULT STDMETHODCALLTYPE Disconnect(IPin *ppin) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultSyncSource() = 0;
    virtual HRESULT STDMETHODCALLTYPE Connect(IPin *ppinOut, IPin *ppinIn) = 0;
    virtual HRESULT STDMETHODCALLTYPE Render(IPin *ppinOut) = 0;
    virtual HRESULT STDMETHODCALLTYPE RenderFile(LPCWSTR lpcwstrFile, LPCWSTR lpcwstrPlayList) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddSourceFilter(LPCWSTR lpcwstrFileName, LPCWSTR lpcwstrFilterName,
                                                      IBaseFilter **ppFilter) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetLogFile(DWORD_PTR hFile) = 0;
    virtual HRESULT STDMETHODCALLTYPE Abort() = 0;
    virtual HRESULT STDMETHODCALLTYPE ShouldOperationContinue() = 0;
};

struct IMediaControl : public IDispatch {
    virtual HRESULT STDMETHODCALLTYPE Run() = 0;
    virtual HRESULT STDMETHODCALLTYPE Pause() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetState(LONG msTimeout, LONG *pfs) = 0;
    virtual HRESULT STDMETHODCALLTYPE RenderFile(BSTR strFilename) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddSourceFilter(BSTR strFilename, IDispatch **ppUnk) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_FilterCollection(IDispatch **ppUnk) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_RegFilterCollection(IDispatch **ppUnk) = 0;
    virtual HRESULT STDMETHODCALLTYPE StopWhenReady() = 0;
};

struct IMediaEvent : public IDispatch {
    virtual HRESULT STDMETHODCALLTYPE GetEventHandle(LONG_PTR *hEvent) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetEvent(long *lEventCode, LONG_PTR *lParam1, LONG_PTR *lParam2,
                                               long msTimeout) = 0;
    virtual HRESULT STDMETHODCALLTYPE WaitForCompletion(long msTimeout, long *pEvCode) = 0;
    virtual HRESULT STDMETHODCALLTYPE CancelDefaultHandling(long lEvCode) = 0;
    virtual HRESULT STDMETHODCALLTYPE RestoreDefaultHandling(long lEvCode) = 0;
    virtual HRESULT STDMETHODCALLTYPE FreeEventParams(long lEvCode, LONG_PTR lParam1, LONG_PTR lParam2) = 0;
};

struct IVideoWindow : public IDispatch {
    virtual HRESULT STDMETHODCALLTYPE put_Caption(BSTR strCaption) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Caption(BSTR *strCaption) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_WindowStyle(long WindowStyle) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_WindowStyle(long *WindowStyle) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_WindowStyleEx(long WindowStyleEx) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_WindowStyleEx(long *WindowStyleEx) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AutoShow(long AutoShow) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AutoShow(long *AutoShow) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_WindowState(long WindowState) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_WindowState(long *WindowState) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_BackgroundPalette(long pBackgroundPalette) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BackgroundPalette(long *pBackgroundPalette) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Visible(long Visible) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Visible(long *pVisible) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Left(long Left) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Left(long *pLeft) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Width(long Width) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Width(long *pWidth) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Top(long Top) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Top(long *pTop) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Height(long Height) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Height(long *pHeight) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Owner(LONG_PTR Owner) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Owner(LONG_PTR *Owner) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_MessageDrain(LONG_PTR Drain) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MessageDrain(LONG_PTR *Drain) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BorderColor(long *Color) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_BorderColor(long Color) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_FullScreenMode(long *FullScreenMode) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_FullScreenMode(long FullScreenMode) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetWindowForeground(long Focus) = 0;
    virtual HRESULT STDMETHODCALLTYPE NotifyOwnerMessage(LONG_PTR hwnd, long uMsg, LONG_PTR wParam,
                                                         LONG_PTR lParam) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetWindowPosition(long Left, long Top, long Width, long Height) = 0;
};

/* Renderer statistics (amvideo.h). Not every renderer implements it. */
struct IQualProp : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_FramesDroppedInRenderer(int *pcFrames) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_FramesDrawn(int *pcFramesDrawn) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AvgFrameRate(int *piAvgFrameRate) = 0;   /* frames/s x 100 */
    virtual HRESULT STDMETHODCALLTYPE get_Jitter(int *iJitter) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AvgSyncOffset(int *piAvg) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DevSyncOffset(int *piDev) = 0;
};

static const CLSID CLSID_FilterGraph =
    { 0xe436ebb3, 0x524f, 0x11ce, { 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };
static const IID IID_IGraphBuilder =
    { 0x56a868a9, 0x0ad4, 0x11ce, { 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };
static const IID IID_IMediaControl =
    { 0x56a868b1, 0x0ad4, 0x11ce, { 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };
static const IID IID_IMediaEvent =
    { 0x56a868b6, 0x0ad4, 0x11ce, { 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };
static const IID IID_IVideoWindow =
    { 0x56a868b4, 0x0ad4, 0x11ce, { 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };
/*
 * IVrend: private interface of the Alchemy Video Renderer (IID from Iris.exe, also in
 * VideoRenderer.dll). Iris calls vtable slot +0x14 with 1 ("Turn Video On") after loading a
 * movie and with 0 when the rear camera takes over. Slots +0x0C/+0x10 are unknown and unused.
 */
struct IVrend : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Unknown0C() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unknown10() = 0;
    virtual HRESULT STDMETHODCALLTYPE VideoOnOff(int on) = 0;
};
static const IID IID_IVrend =
    { 0xa08ea375, 0xc7da, 0x4ef7, { 0xa5, 0xc7, 0x0a, 0x31, 0xfd, 0x92, 0x36, 0x83 } };

/* IMediaFilter is the start of IBaseFilter's vtable; the graph manager exposes it. */
static const IID IID_IMediaFilter =
    { 0x56a86899, 0x0ad4, 0x11ce, { 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };
static const IID IID_IQualProp =
    { 0x1bd0ecb0, 0xf8e2, 0x11ce, { 0xaa, 0xc6, 0x00, 0x20, 0xaf, 0x0b, 0x99, 0xa3 } };

#define EC_COMPLETE         0x01
#define EC_USERABORT        0x02
#define EC_ERRORABORT       0x03
#define OATRUE              (-1)

#endif
