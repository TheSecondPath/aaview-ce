/*
 * aaview - the head unit's half of the Android Auto receiver (CE 6.0, MIPS).
 *
 * The Pi does everything phone-facing (Bluetooth/Wi-Fi, TLS, the AA protocol). It hands this app raw H.264 access
 * units and PCM audio through a ring buffer on a raw "stream" LUN of its USB gadget (no file system; read by sector,
 * uncached), and reads touches back from the same disk. aaview feeds the video to the Alchemy hardware decoder
 * through its own DirectShow source filter and shows it full screen, plays the audio through waveOut, records the
 * microphone when the phone asks, and reports every touch and key.
 *
 * Stream LUN (4 MB, 8192 sectors; aarecvd on the Pi writes it, see src/Projection/StreamRing), all little-endian:
 *   sector 0  Pi header   "AVS1", u32 seq, u32 session, u32 wr, u32 ring_sector, u32 ring_bytes, u32 idr_pos,
 *                         u32 width, u32 height, u32 flags, u32 mic_gen, u32 utc (the Pi's clock, Unix seconds,
 *                         0 = not to be trusted), i32 local offset (s east of UTC), u32 pairing code, ...,
 *                         u32 seq copy at 504.
 *                         flags: bit 0 quit, bit 1 projecting, bit 2 phone connected, bit 3 microphone wanted,
 *                         bit 4 pairing open, bits 8..15 media-start counter, bits 16..17 log level
 *   sector 1  our status  "AVH1", u32 seq, u32 session, u32 rd, u32 fed, u32 decoded, u32 dropped, u32 flags,
 *                         u32 tick, u32 nevents (cumulative), then the last 16 events (8 bytes each: u16 x, u16 y,
 *                         u8 action, u8 pointer, u16 ms), seq copy at 504. Actions: 0 down, 1 up, 2 move,
 *                         3/4 key down/up, 5 hard key, 6 ISV event, 7 icon tap, 8 lights, 9 pairing.
 *   2 to 13   microphone (ours, while header flag bit 3 is set): 3 slots of 4 sectors, slot = seq % 3, each
 *                         "AVM1", u32 mic_gen, u32 seq (from 1), u32 nbytes, PCM 16 kHz 16-bit mono, seq copy at 2044
 *   ring      records, each 16-byte aligned: u32 len, u16 type (1 H.264 AU, 2 PCM audio, 9 bench filler), u16 flags
 *             (video: bit 0 IDR; audio: the stream, 0 media, 1 speech, 2 system), u32 pts_ms, u32 rec_no, then len
 *             payload bytes. wr / rd / idr_pos are cumulative byte counters. The ring is sectors 16 to 4111
 *             (2 MiB); the rest is unused.
 * The Pi never writes past our rd (it drops frames up to the next IDR instead), so nothing we read is overwritten.
 *
 * aaview never ends on its own. Ending it by force leaves the decoder's memory behind until the head unit restarts,
 * so whatever happens (the phone leaves, the Pi's receiver stops, the decoder fails, a WM_CLOSE from somewhere) it
 * hides the picture, shows the floating icon and waits. The one way out is the Pi's quit flag
 * ("aarecvd --quit-aaview"), which takes the decoder down in order.
 *
 * Usage: aaview.exe [/auto] [hold|nohold] [noicon] [icon=X,Y] [ts0|tsN] [clock] [abufN]
 *                   [arateN|noarate] [micN|nomic] [nograb] [norestore] [yuvcamN]
 *     hold      take the foreground back when another full-screen window takes it (the default); nohold: don't
 *     tsN       timestamp = arrival + N ms (default 40); ts0 = no timestamps
 *     clock     keep the graph clock (default: SetSyncSource(NULL), render as soon as decoded)
 *     abufN     media audio jitter buffer in ms (default 600)
 *     arateN    where the audio clock correction starts, in ppm (default 7000); noarate: play samples as they come
 *     micN      wave-in device for the assistant's microphone (default 1); nomic: never record
 *     nograb    leave the head unit's audio source alone; norestore: leave it on USB media when the phone goes
 *     yuvcamN   cap on the decoder's frame region, in MB (default 6)
 * Files, all in the exe's folder on the stick: aaview_log.txt (the previous one is aaview_log.old.txt), the icon's
 * position in aaview_icon.txt and the measured audio clock in aaview_clock.txt.
 */
#include "dshow_filters.h"
#include <storemgr.h>
#include <tlhelp32.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

/* coredll has no lstrcpyn; bounded copy that always terminates. */
#define lstrcpyn(d, s, n)  (wcsncpy((d), (s), (n) - 1), (d)[(n) - 1] = 0, (d))

#define AAVIEW_VERSION  L"0.8.13"
#define FRAME_100NS     (10000000 / 30)
#define TEMP_AAVIEW     L"\\Temp\\aaview.exe"

/* ---------- logging (shared with the decoder's threads) ---------- */

static HANDLE           g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_logLock;
static WCHAR            g_logPath[MAX_PATH];
static DWORD            g_logRetry;
/* Lines collect in memory and the main thread writes them about once a second (logFlush, from pump()). Writing
 * from the thread that logged would stall the ring reader, because the stick shares its USB link with the stream
 * LUN. Lines logged while the stick is out wait for it to come back. */
#define LOGBUF_BYTES    (48 * 1024)
static char             g_logBuf[LOGBUF_BYTES], g_logOut[LOGBUF_BYTES];
static DWORD            g_logLen, g_logSince, g_logLost;
static volatile DWORD   g_worstLog;     /* the longest write + flush, ms (the feeder reports and clears it) */
/* Size cap and levels.
 * At LOG_MAX_BYTES the log becomes aaview_log.old.txt (replacing the one before) and a new one is started, so the
 * two together never take more than twice that on the stick.
 * The level is set on the Pi (one word in /etc/aarecv/loglevel) and comes in the Pi's header flags, bits 16..17:
 * 0 everything, 1 no chatter (statistics, window tracking, keys, the graph's building), 2 or 3 only what went wrong.
 * Until the first header is read everything is logged, so each start's first lines are always there.
 *   logd  chatter          logmsg  what happened          logw  what went wrong, and what must always be there
 *   logh  a call's result: chatter if it worked, a warning if it failed */
#define LOG_MAX_BYTES   (4u * 1024 * 1024)
static WCHAR            g_logOld[MAX_PATH];
static DWORD            g_logSize;
static volatile LONG    g_logLevel;

static void logOpen(void)
{
    int pass;

    for (pass = 0; pass < 2; pass++) {
        g_log = CreateFile(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_log == INVALID_HANDLE_VALUE)
            return;
        g_logSize = GetFileSize(g_log, NULL);
        if (g_logSize == 0xFFFFFFFF)
            g_logSize = 0;
        if (pass || g_logSize < LOG_MAX_BYTES || !g_logOld[0])
            break;
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
        DeleteFile(g_logOld);
        if (!MoveFile(g_logPath, g_logOld))
            DeleteFile(g_logPath);      /* keeping under the cap matters more than the old lines */
    }
    SetFilePointer(g_log, 0, NULL, FILE_END);
}

static void vlog(int level, const WCHAR *fmt, va_list ap)
{
    WCHAR   w[1024];
    char    a[3 * 1024];
    int     n;

    if (level < g_logLevel)
        return;
    _vsnwprintf(w, 1023, fmt, ap);
    w[1023] = 0;
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, a, sizeof(a), NULL, NULL);
    if (n > 1) {
        EnterCriticalSection(&g_logLock);
        if (g_logLen + (DWORD)(n - 1) <= LOGBUF_BYTES) {
            if (!g_logLen)
                g_logSince = GetTickCount();
            memcpy(g_logBuf + g_logLen, a, n - 1);
            g_logLen += n - 1;
        } else {
            g_logLost++;
        }
        LeaveCriticalSection(&g_logLock);
    }
}

static void logmsg(const WCHAR *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vlog(1, fmt, ap);
    va_end(ap);
}

static void logd(const WCHAR *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vlog(0, fmt, ap);
    va_end(ap);
}

static void logw(const WCHAR *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vlog(2, fmt, ap);
    va_end(ap);
}

static void logh(HRESULT hr, const WCHAR *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vlog(FAILED(hr) ? 2 : 0, fmt, ap);
    va_end(ap);
}

/* Main thread only: write what has collected (once it is a second old, or now). */
static void logFlush(BOOL now)
{
    DWORD len, lost, written, t;

    if (!g_logLen || (!now && GetTickCount() - g_logSince < 1000))
        return;
    /* The log lives on the Pi's stick, which comes and goes (re-plugs, Pi power): reopen it when it's back. */
    if (g_log == INVALID_HANDLE_VALUE && g_logPath[0] && GetTickCount() - g_logRetry > 2000) {
        g_logRetry = GetTickCount();
        logOpen();
    }
    if (g_log == INVALID_HANDLE_VALUE)
        return;
    EnterCriticalSection(&g_logLock);
    len = g_logLen;
    lost = g_logLost;
    memcpy(g_logOut, g_logBuf, len);
    g_logLen = g_logLost = 0;
    LeaveCriticalSection(&g_logLock);
    t = GetTickCount();
    if (lost) {
        char m[64];
        WriteFile(g_log, m, sprintf(m, "   (%u log lines lost: the buffer was full)\r\n", lost), &written, NULL);
    }
    if (!WriteFile(g_log, g_logOut, len, &written, NULL) || !FlushFileBuffers(g_log)) {
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    } else if ((g_logSize += len) >= LOG_MAX_BYTES) {
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
        logOpen();                     /* full: logOpen rotates it */
    }
    t = GetTickCount() - t;
    if (t > g_worstLog)
        g_worstLog = t;
}

/* ---------- decoder log capture ---------- */

#define DEC_BASE        0x41710000
#define DEC_CHECK_AT    0x46DC          /* jal 0x417B097C in the output routine */
#define DEC_CHECK_WORD  0x0C5EC25F
#define DEC_OUT_SLOT    0xA9124
#define DEC_MAX_LINES   3000

static volatile LONG g_decLines;
static volatile BOOL g_memFail;          /* decoder reported its memory pools missing */

static void decLogOut(const WCHAR *text)
{
    WCHAR line[600];
    int   n;

    if (wcsstr(text, L"buffer not provided"))
        g_memFail = TRUE;
    if (InterlockedIncrement((LONG *)&g_decLines) > DEC_MAX_LINES)
        return;
    lstrcpyn(line, text, 600);
    n = (int)wcslen(line);
    while (n && (line[n - 1] == L'\n' || line[n - 1] == L'\r'))
        line[--n] = 0;
    logd(L"   [dec] %s\r\n", line);
}

static void hookDecoderLog(void)
{
    static HMODULE dec;
    BYTE  *base = (BYTE *)DEC_BASE;
    DWORD *slot = (DWORD *)(base + DEC_OUT_SLOT);

    if (!dec)
        dec = LoadLibrary(L"DecodeFilter.dll");
    __try {
        if (*(DWORD *)(base + DEC_CHECK_AT) != DEC_CHECK_WORD) {
            logw(L"   decoder log hook: unexpected code at 0x%08X; not hooking\r\n", (DWORD)(base + DEC_CHECK_AT));
            return;
        }
        if (*slot != (DWORD)decLogOut) {
            DWORD old;
            VirtualProtect(slot, 4, PAGE_READWRITE, &old);
            *slot = (DWORD)decLogOut;
        }
        logd(L"   decoder log hook: on\r\n");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logw(L"   decoder log hook failed (exception 0x%08X)\r\n", GetExceptionCode());
    }
}

/*
 * Memory remap. Left alone, the decoder decodes exactly one frame: at start-up its 16 VideoSample
 * allocations all fail in MEM1 region 0 (MAE, 4 MB, about 3.5 MB already in use by then). Region 4
 * ("DUM", 16 MB at 0x0B600000) is used only by the rear camera's layer_control. The decoder reaches
 * MEM1 through its DeviceIoControl import slot, so inside this process we redirect its region requests
 * (query, alloc and free). Nothing persists; a restart undoes it.
 */
#define DEC_IOCTL_SLOT  0xA913C         /* 0x417B913C: lw from 0x417C0000 - 0x6EC4 */
#define MEM1_ALLOC      0x00220404
#define MEM1_FREE       0x00220408
#define MEM1_QUERY      0x0022040C

typedef BOOL (WINAPI *PFN_DEVIOCTL)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static PFN_DEVIOCTL  g_realIoctl;
static BOOL          g_remap;
static DWORD         g_capYuv = 6 * 1024 * 1024;   /* cap on the decoder's frame region (1), "yuvcamN"; 0 = none */

/* Read-only: what MEM1 says about region numbers 0..7 (the registry defines 0, 1, 2 and 4). */
static void queryRegions(void)
{
    HANDLE mem = CreateFile(L"MEM1:", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD  i;

    if (mem == INVALID_HANDLE_VALUE) {
        logw(L"   regions: open MEM1: failed: %u\r\n", GetLastError());
        return;
    }
    for (i = 0; i < 8; i++) {
        DWORD req[8], size = 0, bytes = 0;
        BOOL  ok;
        memset(req, 0, sizeof(req));
        req[5] = i;
        SetLastError(0);
        ok = DeviceIoControl(mem, MEM1_QUERY, req, sizeof(req), &size, sizeof(size), &bytes, NULL);
        logd(L"   region %u: query ok %d, %u KB, error %u\r\n", i, ok, size / 1024, ok ? 0 : GetLastError());
    }
    CloseHandle(mem);
}
static volatile LONG g_remapLines;

/* Where each of the decoder's regions goes: its working memory (0, 4 MB) to region 1 (16 MB), and its frames (1) to
 * region 4, the rear camera's 16 MB, capped at g_capYuv, which leaves the camera working. With a 5-picture pool it
 * needs about 14.8 MB of working (MAE) memory but only about 2.9 MB for frames. The Pi rewrites the SPS level_idc so
 * the picture pool fits. */
static DWORD g_regionMap[8] = { 1, 4, 2, 3, 4, 5, 6, 7 };

static BOOL WINAPI decIoctl(HANDLE h, DWORD code, LPVOID in, DWORD inSize, LPVOID out, DWORD outSize,
                             LPDWORD ret, LPOVERLAPPED ov)
{
    BOOL r;

    if (g_remap && (code == MEM1_ALLOC || code == MEM1_FREE || code == MEM1_QUERY) && in && inSize >= 0x18) {
        DWORD *req = (DWORD *)in;
        DWORD  from = req[5];
        if (from < 8 && g_regionMap[from] != from) {
            DWORD reported = 0, to = g_regionMap[from];
            req[5] = to;
            r = g_realIoctl(h, code, in, inSize, out, outSize, ret, ov);
            /* The decoder allocates exactly what the query reports, so capping the reported size
             * of its frame region (1) leaves the rest of the target region free. */
            if (code == MEM1_QUERY && r && out && outSize >= 4) {
                reported = *(DWORD *)out;
                DWORD cap = from == 1 ? g_capYuv : 0;
                if (cap && reported > cap)
                    *(DWORD *)out = cap;
            }
            if (InterlockedIncrement((LONG *)&g_remapLines) <= 12) {
                if (code == MEM1_QUERY)
                    logd(L"   [mem] decoder query region %u -> %u: ok %d, region reports %u KB, decoder told %u KB\r\n",
                           from, to, r, reported / 1024, (r && out) ? *(DWORD *)out / 1024 : 0);
                else
                    logd(L"   [mem] decoder %s region %u -> %u: ok %d, size %u KB, phys 0x%08X\r\n",
                           code == MEM1_ALLOC ? L"alloc" : L"free", from, to, r, req[3] / 1024, req[2]);
            }
            req[5] = from;              /* the decoder's own copy keeps its region number */
            return r;
        }
    }
    return g_realIoctl(h, code, in, inSize, out, outSize, ret, ov);
}

static void hookDecoderIoctl(void)
{
    BYTE  *base = (BYTE *)DEC_BASE;
    DWORD *slot = (DWORD *)(base + DEC_IOCTL_SLOT);
    DWORD  real = (DWORD)GetProcAddress(GetModuleHandle(L"coredll.dll"), L"DeviceIoControl");

    __try {
        if (*(DWORD *)(base + DEC_CHECK_AT) != DEC_CHECK_WORD)
            return;
        if (*slot != (DWORD)decIoctl) {
            logd(L"   memory remap: DeviceIoControl slot holds 0x%08X (coredll 0x%08X)\r\n", *slot, real);
            if (*slot != real) {
                logw(L"   memory remap: slot is not DeviceIoControl; not hooking\r\n");
                return;
            }
            g_realIoctl = (PFN_DEVIOCTL)*slot;
            {
                DWORD old;
                VirtualProtect(slot, 4, PAGE_READWRITE, &old);
            }
            *slot = (DWORD)decIoctl;
        }
        g_remap = TRUE;
        logd(L"   memory remap: on (decoder region 0 -> %u, 1 -> %u)\r\n", g_regionMap[0], g_regionMap[1]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logw(L"   memory remap failed (exception 0x%08X)\r\n", GetExceptionCode());
    }
}

/* ---------- overlay plane and renderer switch ---------- */

#define OVL_ENABLE      0x00229C44
#define OVL_DISABLE     0x00229C48
#define OVL_PREDISABLE  0x00229C7C
#define MAE_PLANE       1

static int ovlEscape(DWORD code, DWORD index)
{
    HDC dc = GetDC(NULL);
    int r = ExtEscape(dc, (int)code, 4, (LPCSTR)&index, 0, NULL);
    ReleaseDC(NULL, dc);
    return r;
}

/* Turning the video plane back on after the camera. The display driver has a "rear on" state of its own in which
 * it refuses to enable or disable the other planes, so after a reverse Android Auto can come back with frames being
 * decoded and drawn and the screen still blank. Nothing else turns plane 1 on again (the renderer never enables it),
 * so we do: when the camera is over and whenever we come back in front of another full screen, three times over
 * about 3 s in case the driver is still in "rear on". */
static DWORD g_ovlAgain;                /* tick at which to enable the plane again (0: nothing to do) */
static int   g_ovlTries;

static void planeAgain(void)
{
    g_ovlTries = 0;
    g_ovlAgain = GetTickCount() | 1;
}

/* ---------- media type helpers ---------- */

static VIDEOINFOHEADER g_vih;
static AM_MEDIA_TYPE   g_feedType;

static void makeFeedType(DWORD fourcc)
{
    memset(&g_vih, 0, sizeof(g_vih));
    SetRect(&g_vih.rcSource, 0, 0, 800, 480);
    SetRect(&g_vih.rcTarget, 0, 0, 800, 480);
    g_vih.dwBitRate = 2000000;
    g_vih.AvgTimePerFrame = FRAME_100NS;
    g_vih.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    g_vih.bmiHeader.biWidth = 800;
    g_vih.bmiHeader.biHeight = 480;
    g_vih.bmiHeader.biPlanes = 1;
    g_vih.bmiHeader.biBitCount = 24;
    g_vih.bmiHeader.biCompression = fourcc;
    memset(&g_feedType, 0, sizeof(g_feedType));
    g_feedType.majortype = MEDIATYPE_Video;
    g_feedType.subtype = MEDIASUBTYPE_FOURCC_BASE;
    g_feedType.subtype.Data1 = fourcc;
    g_feedType.bTemporalCompression = TRUE;
    g_feedType.formattype = FORMAT_VideoInfo;
    g_feedType.cbFormat = sizeof(g_vih);
    g_feedType.pbFormat = (BYTE *)&g_vih;
}

/* Deep copy; the format block is CoTaskMemAlloc'd as DirectShow callers expect. */
static HRESULT copyMt(AM_MEDIA_TYPE *dst, const AM_MEDIA_TYPE *src)
{
    *dst = *src;
    dst->pUnk = NULL;
    dst->pbFormat = NULL;
    if (src->cbFormat && src->pbFormat) {
        dst->pbFormat = (BYTE *)CoTaskMemAlloc(src->cbFormat);
        if (!dst->pbFormat)
            return E_OUTOFMEMORY;
        memcpy(dst->pbFormat, src->pbFormat, src->cbFormat);
    }
    return S_OK;
}

static void freeMt(AM_MEDIA_TYPE *mt)
{
    if (mt->pbFormat)
        CoTaskMemFree(mt->pbFormat);
    mt->pbFormat = NULL;
    mt->cbFormat = 0;
}

/* Enumerates zero or one media type. */
class TypeEnum : public IEnumMediaTypes {
    LONG          m_ref;
    AM_MEDIA_TYPE m_mt;
    BOOL          m_have;
    ULONG         m_pos;
public:
    TypeEnum(const AM_MEDIA_TYPE *mt) : m_ref(1), m_have(mt != NULL), m_pos(0)
    {
        memset(&m_mt, 0, sizeof(m_mt));
        if (mt)
            copyMt(&m_mt, mt);
    }
    ~TypeEnum() { freeMt(&m_mt); }
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        if (!memcmp(&riid, &IID_IUnknown, sizeof(GUID)) || !memcmp(&riid, &IID_IEnumMediaTypes, sizeof(GUID))) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r)
            delete this;
        return r;
    }
    STDMETHODIMP Next(ULONG n, AM_MEDIA_TYPE **out, ULONG *got)
    {
        ULONG k = 0;
        if (n && m_have && m_pos == 0) {
            AM_MEDIA_TYPE *mt = (AM_MEDIA_TYPE *)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
            if (!mt)
                return E_OUTOFMEMORY;
            copyMt(mt, &m_mt);
            out[0] = mt;
            k = 1;
            m_pos = 1;
        }
        if (got)
            *got = k;
        return k == n ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG n) { m_pos += n; return m_have && m_pos <= 1 ? S_OK : S_FALSE; }
    STDMETHODIMP Reset() { m_pos = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumMediaTypes **pp)
    {
        TypeEnum *e = new TypeEnum(m_have ? &m_mt : NULL);
        if (!e)
            return E_OUTOFMEMORY;
        e->m_pos = m_pos;
        *pp = e;
        return S_OK;
    }
};

/* Enumerates exactly one pin. */
class PinEnum : public IEnumPins {
    LONG  m_ref;
    IPin *m_pin;
    ULONG m_pos;
public:
    PinEnum(IPin *pin) : m_ref(1), m_pin(pin), m_pos(0) {}
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        if (!memcmp(&riid, &IID_IUnknown, sizeof(GUID)) || !memcmp(&riid, &IID_IEnumPins, sizeof(GUID))) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r)
            delete this;
        return r;
    }
    STDMETHODIMP Next(ULONG n, IPin **out, ULONG *got)
    {
        ULONG k = 0;
        if (n && m_pos == 0) {
            out[0] = m_pin;
            m_pin->AddRef();
            k = 1;
            m_pos = 1;
        }
        if (got)
            *got = k;
        return k == n ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG n) { m_pos += n; return m_pos <= 1 ? S_OK : S_FALSE; }
    STDMETHODIMP Reset() { m_pos = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumPins **pp)
    {
        PinEnum *e = new PinEnum(m_pin);
        if (!e)
            return E_OUTOFMEMORY;
        e->m_pos = m_pos;
        *pp = e;
        return S_OK;
    }
};

/*
 * Common filter plumbing. Our filters and pins live for the whole program, so
 * reference counts are tracked but never free the object.
 */
class FilterBase : public IBaseFilter {
protected:
    LONG          m_ref;
    FILTER_STATE  m_state;
    IFilterGraph *m_graph;
    WCHAR         m_name[128];
    IPin         *m_pin;
public:
    FilterBase() : m_ref(1), m_state(State_Stopped), m_graph(NULL), m_pin(NULL) { m_name[0] = 0; }
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        if (!memcmp(&riid, &IID_IUnknown, sizeof(GUID)) || !memcmp(&riid, &IID_IPersist_, sizeof(GUID))
            || !memcmp(&riid, &IID_IMediaFilter, sizeof(GUID)) || !memcmp(&riid, &IID_IBaseFilter, sizeof(GUID))) {
            *ppv = (IBaseFilter *)this;
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() { return InterlockedDecrement(&m_ref); }
    STDMETHODIMP GetClassID(CLSID *p) { memset(p, 0, sizeof(*p)); return S_OK; }
    STDMETHODIMP GetState(DWORD ms, int *s) { (void)ms; *s = m_state; return S_OK; }
    STDMETHODIMP SetSyncSource(IReferenceClock *c) { (void)c; return S_OK; }
    STDMETHODIMP GetSyncSource(IReferenceClock **c) { *c = NULL; return S_OK; }
    STDMETHODIMP EnumPins(IEnumPins **pp)
    {
        *pp = new PinEnum(m_pin);
        return *pp ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP FindPin(LPCWSTR id, IPin **pp)
    {
        (void)id;
        *pp = m_pin;
        m_pin->AddRef();
        return S_OK;
    }
    STDMETHODIMP QueryFilterInfo(FILTER_INFO *fi)
    {
        lstrcpyn(fi->achName, m_name, 128);
        fi->pGraph = m_graph;
        if (m_graph)
            ((IUnknown *)m_graph)->AddRef();
        return S_OK;
    }
    STDMETHODIMP JoinFilterGraph(IFilterGraph *g, LPCWSTR name)
    {
        m_graph = g;                    /* not AddRef'd, per DirectShow rules */
        if (name)
            lstrcpyn(m_name, name, 128);
        return S_OK;
    }
    STDMETHODIMP QueryVendorInfo(LPWSTR *p) { (void)p; return E_NOTIMPL; }
};

/* ---------- the stream LUN (see the top of this file) ---------- */

#define STREAM_SECTORS     8192
#define DISK_IOCTL_READ_2  2            /* what USBware accepts (IOCTL_DISK_READ gives error 31) */
#define DISK_IOCTL_WRITE_2 3
#define IO_CHUNK           (64 * 1024)  /* bytes per DeviceIoControl */
#define STAGE_BYTES        (512 * 1024) /* largest span read at once (and so the largest access unit accepted) */
#define LAG_BYTES          (384 * 1024) /* this far behind with a newer IDR waiting: skip to it */
#define REC_AU             1
#define REC_AUDIO          2            /* PCM; record flags = stream (0 media, 1 speech, 2 system) */
#define REC_BENCH          9
#define NEVENTS            16

typedef struct { PUCHAR sb_buf; DWORD sb_len; } SG_BUF;
typedef struct { DWORD sr_start; DWORD sr_num_sec; DWORD sr_num_sg; DWORD sr_status; void *sr_callback; SG_BUF sr_sglist[1]; } SG_REQ;
typedef HANDLE (WINAPI *PFN_FFS)(PSTOREINFO);
typedef BOOL   (WINAPI *PFN_FNS)(HANDLE, PSTOREINFO);
typedef BOOL   (WINAPI *PFN_FCS)(HANDLE);
typedef HANDLE (WINAPI *PFN_OS)(LPCTSTR);

typedef struct {                        /* sector 0, written by the Pi */
    DWORD magic, seq, session, wr, ring_sector, ring_bytes, idr_pos, width, height, flags, mic_gen;
} PI_HDR;

static HANDLE           g_stream = INVALID_HANDLE_VALUE;
static BOOL             g_ioErr;        /* the last header read failed at the I/O level */
static CRITICAL_SECTION g_ioLock;       /* one DeviceIoControl at a time on the stream store */
static volatile DWORD   g_worstIo, g_worstRecv;   /* the longest disk call / decoder call, ms (feeder's stall report) */
static BYTE            *g_stage;
static PI_HDR           g_hdr;
static volatile DWORD   g_pairKey;      /* the pairing code at header byte 52 (0: none yet; see the icon) */
static volatile DWORD   g_hdrTick;      /* when the Pi last rewrote its header (it does every 2 s, even idle) */
static volatile DWORD   g_hdrRead;      /* when we last read a whole header, rewritten or not */
#define HDR_STALE_MS    20000           /* a header we can read but the Pi has not rewritten for this long: its
                                         * receiver has stopped, and "projecting" in it means nothing any more */
static volatile BOOL    g_quit;
static BOOL             g_hold, g_keepClock;
static int              g_tsMode = 40;  /* -1 = no timestamps, else arrival + N ms */

/* our status sector */
static CRITICAL_SECTION g_stLock;
static DWORD            g_mySession, g_stSeq, g_rd, g_stFlags, g_nev;
static volatile LONG    g_fed, g_feedFails, g_decoded, g_dropped;
static BYTE             g_ev[NEVENTS][8];

static BOOL streamIo(DWORD code, DWORD sector, BYTE *buf, DWORD bytes)
{
    SG_REQ r;
    DWORD  got = 0, t;
    BOOL   ok;

    memset(&r, 0, sizeof(r));
    r.sr_start = sector;
    r.sr_num_sec = bytes / 512;
    r.sr_num_sg = 1;
    r.sr_sglist[0].sb_buf = buf;
    r.sr_sglist[0].sb_len = bytes;
    t = GetTickCount();
    EnterCriticalSection(&g_ioLock);
    ok = DeviceIoControl(g_stream, code, &r, sizeof(r), NULL, 0, &got, NULL) && r.sr_status == ERROR_SUCCESS;
    LeaveCriticalSection(&g_ioLock);
    t = GetTickCount() - t;
    if (t > g_worstIo)
        g_worstIo = t;
    return ok;
}

static BOOL streamRead(DWORD sector, BYTE *buf, DWORD bytes)
{
    while (bytes) {
        DWORD n = bytes > IO_CHUNK ? IO_CHUNK : bytes;
        if (!streamIo(DISK_IOCTL_READ_2, sector, buf, n))
            return FALSE;
        sector += n / 512;
        buf += n;
        bytes -= n;
    }
    return TRUE;
}

static BOOL streamOpen(void)
{
    HMODULE   cd = GetModuleHandle(L"coredll.dll");
    PFN_FFS   ffs = (PFN_FFS)GetProcAddress(cd, L"FindFirstStore");
    PFN_FNS   fns = (PFN_FNS)GetProcAddress(cd, L"FindNextStore");
    PFN_FCS   fcs = (PFN_FCS)GetProcAddress(cd, L"FindCloseStore");
    PFN_OS    os  = (PFN_OS)GetProcAddress(cd, L"OpenStore");
    STOREINFO si;
    HANDLE    f;

    if (!ffs || !fns || !fcs || !os)
        return FALSE;
    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    f = ffs(&si);
    if (f == INVALID_HANDLE_VALUE)
        return FALSE;
    do {
        if (si.snNumSectors == STREAM_SECTORS && si.dwBytesPerSector == 512) {
            g_stream = os(si.szDeviceName);
            logmsg(L"stream LUN: %s (%s), %u sectors: %s\r\n", si.szDeviceName, si.szStoreName, (DWORD)si.snNumSectors,
                   g_stream != INVALID_HANDLE_VALUE ? L"open" : L"OpenStore failed");
            break;
        }
        si.cbSize = sizeof(si);
    } while (fns(f, &si));
    fcs(f);
    return g_stream != INVALID_HANDLE_VALUE;
}

/* Sets the head unit's clock from the Pi's (header bytes 44..51). b is a whole, fresh header sector. */
#define CLOCK_SLACK_S   180             /* closer than this is left alone (LG's clock shows minutes) */
#define CLOCK_TRIES     3
#define CLOCK_EVERY_MS  10000           /* the header changes with every frame: look this often */
#define CLOCK_AGAIN_MS  60000           /* after a set, wait this long before judging (or trying) again */
#ifndef WM_TIMECHANGE
#define WM_TIMECHANGE   0x001E          /* missing from the CE SDK's winuser.h; this is the desktop value */
#endif

static void clockCheck(const BYTE *b)
{
    static DWORD s_looked, s_setAt;
    static int   s_tries;
    static BOOL  s_saidOk, s_saidGiveUp;
    DWORD        utc, now = GetTickCount();
    LONG         off;
    LONGLONG     pi, hu, diff;
    SYSTEMTIME   st, was, after;
    FILETIME     ft;
    BOOL         ok;

    if (s_looked && now - s_looked < CLOCK_EVERY_MS)
        return;
    s_looked = now;
    utc = *(const DWORD *)(b + 44);
    off = *(const LONG *)(b + 48);
    if (!utc || off < -14 * 3600 || off > 14 * 3600)
        return;                         /* the Pi has no time it trusts */
    if (s_tries && now - s_setAt < CLOCK_AGAIN_MS)
        return;

    pi = ((LONGLONG)utc + off) * 10000000 + 116444736000000000LL;     /* the Pi's local time as a FILETIME */
    GetLocalTime(&was);
    if (!SystemTimeToFileTime(&was, &ft))
        return;
    hu = ((LONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    diff = (hu - pi) / 10000000;        /* seconds the head unit is ahead */
    if (diff >= -CLOCK_SLACK_S && diff <= CLOCK_SLACK_S) {
        if (!s_saidOk) {
            s_saidOk = TRUE;
            logmsg(L"   %u s clock: the head unit's is within %d s of the Pi's (%s)\r\n", now / 1000, (int)diff,
                   s_tries ? L"after setting it" : L"nothing to do");
        }
        return;
    }
    s_saidOk = FALSE;
    if (s_tries >= CLOCK_TRIES) {
        if (!s_saidGiveUp) {
            s_saidGiveUp = TRUE;
            logw(L"   %u s clock: still %d s from the Pi's after %d tries: something sets it back; leaving it\r\n",
                 now / 1000, (int)diff, s_tries);
        }
        return;
    }
    ft.dwLowDateTime = (DWORD)pi;
    ft.dwHighDateTime = (DWORD)(pi >> 32);
    if (!FileTimeToSystemTime(&ft, &st))
        return;
    s_tries++;
    s_setAt = now;
    ok = SetLocalTime(&st);
    if (ok)
        PostMessage(HWND_BROADCAST, WM_TIMECHANGE, 0, 0);
    GetLocalTime(&after);
    logw(L"   %u s clock: the head unit said %04u-%02u-%02u %02u:%02u:%02u, %d s from the Pi's; set to "
         L"%04u-%02u-%02u %02u:%02u:%02u (UTC%+d min): %s, now %04u-%02u-%02u %02u:%02u:%02u (try %d of %d)\r\n",
         now / 1000, was.wYear, was.wMonth, was.wDay, was.wHour, was.wMinute, was.wSecond, (int)diff,
         st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, (int)(off / 60),
         ok ? L"ok" : L"SetLocalTime failed", after.wYear, after.wMonth, after.wDay, after.wHour, after.wMinute,
         after.wSecond, s_tries, CLOCK_TRIES);
}

static BOOL readHeader(void)
{
    static BYTE b[512];
    PI_HDR      h;

    if (!streamRead(0, b, 512)) {
        g_hdr.magic = 0;                /* not projecting while we can't read */
        g_ioErr = TRUE;
        return FALSE;
    }
    g_ioErr = FALSE;
    memcpy(&h, b, sizeof(h));
    if (h.magic != 0x31535641 /* "AVS1" */ || h.seq != *(DWORD *)(b + 504))
        return FALSE;                   /* not started yet, or caught half-written */
    if (h.ring_bytes == 0 || h.ring_bytes % 512 || h.ring_sector < 2 ||
        h.ring_sector + h.ring_bytes / 512 > STREAM_SECTORS)
        return FALSE;
    if (h.seq != g_hdr.seq || g_hdr.magic != h.magic) {
        g_hdrTick = GetTickCount();
        clockCheck(b);                 /* only a header the Pi has just written carries a fresh time */
    }
    g_hdrRead = GetTickCount();
    g_hdr = h;
    g_pairKey = (h.flags & 16) ? *(const DWORD *)(b + 52) : 0;
    {
        static const WCHAR *s_levels[] = { L"everything", L"no chatter", L"only what goes wrong" };
        LONG                level = (LONG)((h.flags >> 16) & 3);

        if (level > 2)
            level = 2;                  /* aaview has no separate class of errors */
        if (level != g_logLevel) {
            logw(L"   %u s log level: %s (set on the Pi)\r\n", GetTickCount() / 1000, s_levels[level]);
            g_logLevel = level;
        }
    }
    return TRUE;
}

static void writeStatus(void)
{
    static BYTE b[512];
    DWORD      *d = (DWORD *)b;

    EnterCriticalSection(&g_stLock);
    memset(b, 0, sizeof(b));
    g_stSeq++;
    d[0] = 0x31485641;                  /* "AVH1" */
    d[1] = g_stSeq;
    d[2] = g_mySession;
    d[3] = g_rd;
    d[4] = (DWORD)g_fed;
    d[5] = (DWORD)g_decoded;
    d[6] = (DWORD)g_dropped;
    d[7] = g_stFlags;
    d[8] = GetTickCount();
    d[9] = g_nev;
    memcpy(b + 40, g_ev, sizeof(g_ev));
    *(DWORD *)(b + 504) = g_stSeq;
    streamIo(DISK_IOCTL_WRITE_2, 1, b, 512);
    LeaveCriticalSection(&g_stLock);
}

/* The last lights message from MatMessage.dll (0 = none yet). Sent to the Pi again at the start of each
 * projection, so a Pi that started after the lights last changed still knows whether it is night. */
static DWORD g_lightCmd, g_lightData;

/* A touch on our window: queue it in the status sector. The Pi picks up each new one by its cumulative number. */
static void touchEvent(int action, int x, int y)
{
    BYTE *e;
    DWORD t = GetTickCount();

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    EnterCriticalSection(&g_stLock);
    e = g_ev[g_nev % NEVENTS];
    e[0] = (BYTE)x; e[1] = (BYTE)(x >> 8);
    e[2] = (BYTE)y; e[3] = (BYTE)(y >> 8);
    e[4] = (BYTE)action;
    e[5] = 0;
    e[6] = (BYTE)t; e[7] = (BYTE)(t >> 8);
    g_nev++;
    LeaveCriticalSection(&g_stLock);
    writeStatus();
}

/* Reads the ring bytes [pos, pos + len) into g_stage with sector-aligned reads (wrap-aware).
 * *head is where pos lands in g_stage. */
static BOOL ringFetch(DWORD pos, DWORD len, DWORD *head)
{
    DWORD rb = g_hdr.ring_bytes, off = pos % rb, a = off & ~511u, need = ((off - a) + len + 511) & ~511u;

    *head = off - a;
    if (need > STAGE_BYTES)
        return FALSE;
    if (a + need <= rb)
        return streamRead(g_hdr.ring_sector + a / 512, g_stage, need);
    return streamRead(g_hdr.ring_sector + a / 512, g_stage, rb - a) &&
           streamRead(g_hdr.ring_sector, g_stage + (rb - a), need - (rb - a));
}

class FeedPin : public IPin {
public:
    LONG           m_ref;
    IBaseFilter   *m_owner;
    IPin          *m_peer;
    IMemInputPin  *m_memin;
    IMemAllocator *m_alloc;
    AM_MEDIA_TYPE  m_mt;

    FeedPin() : m_ref(1), m_owner(NULL), m_peer(NULL), m_memin(NULL), m_alloc(NULL) { memset(&m_mt, 0, sizeof(m_mt)); }
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        if (!memcmp(&riid, &IID_IUnknown, sizeof(GUID)) || !memcmp(&riid, &IID_IPin, sizeof(GUID))) {
            *ppv = (IPin *)this;
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() { return InterlockedDecrement(&m_ref); }

    STDMETHODIMP Connect(IPin *rx, const AM_MEDIA_TYPE *pmt)
    {
        ALLOCATOR_PROPERTIES req, act, need;
        HRESULT hr;

        if (m_peer)
            return VFW_E_ALREADY_CONNECTED;
        if (!pmt)
            pmt = &g_feedType;
        hr = rx->ReceiveConnection(this, pmt);
        logh(hr, L"   decoder ReceiveConnection: 0x%08X\r\n", hr);
        if (FAILED(hr))
            return hr;
        m_peer = rx;
        rx->AddRef();
        copyMt(&m_mt, pmt);
        hr = rx->QueryInterface(IID_IMemInputPin, (void **)&m_memin);
        logh(hr, L"   decoder IMemInputPin: 0x%08X\r\n", hr);
        if (FAILED(hr))
            goto fail;
        memset(&need, 0, sizeof(need));
        hr = m_memin->GetAllocatorRequirements(&need);
        logh(hr, L"   decoder allocator requirements: 0x%08X (%d x %d bytes, align %d)\r\n",
               hr, need.cBuffers, need.cbBuffer, need.cbAlign);
        hr = m_memin->GetAllocator(&m_alloc);
        logh(hr, L"   decoder GetAllocator: 0x%08X\r\n", hr);
        if (FAILED(hr))
            goto fail;
        req.cBuffers = need.cBuffers > 8 ? need.cBuffers : 8;
        req.cbBuffer = need.cbBuffer > 256 * 1024 ? need.cbBuffer : 256 * 1024;
        req.cbAlign = need.cbAlign > 1 ? need.cbAlign : 1;
        req.cbPrefix = 0;
        memset(&act, 0, sizeof(act));
        hr = m_alloc->SetProperties(&req, &act);
        logh(hr, L"   allocator SetProperties: 0x%08X (got %d x %d bytes)\r\n", hr, act.cBuffers, act.cbBuffer);
        if (FAILED(hr))
            goto fail;
        hr = m_memin->NotifyAllocator(m_alloc, FALSE);
        logh(hr, L"   decoder NotifyAllocator: 0x%08X\r\n", hr);
        if (FAILED(hr))
            goto fail;
        return S_OK;
    fail:
        Disconnect();
        return FAILED(hr) ? hr : E_FAIL;
    }
    STDMETHODIMP ReceiveConnection(IPin *p, const AM_MEDIA_TYPE *mt) { (void)p; (void)mt; return E_UNEXPECTED; }
    STDMETHODIMP Disconnect()
    {
        if (m_alloc) { m_alloc->Release(); m_alloc = NULL; }
        if (m_memin) { m_memin->Release(); m_memin = NULL; }
        if (m_peer) { m_peer->Release(); m_peer = NULL; }
        freeMt(&m_mt);
        return S_OK;
    }
    STDMETHODIMP ConnectedTo(IPin **pp)
    {
        *pp = m_peer;
        if (!m_peer)
            return VFW_E_NOT_CONNECTED;
        m_peer->AddRef();
        return S_OK;
    }
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE *mt)
    {
        if (!m_peer)
            return VFW_E_NOT_CONNECTED;
        return copyMt(mt, &m_mt);
    }
    STDMETHODIMP QueryPinInfo(PIN_INFO *pi)
    {
        pi->pFilter = m_owner;
        m_owner->AddRef();
        pi->dir = PINDIR_OUTPUT;
        wcscpy(pi->achName, L"Out");
        return S_OK;
    }
    STDMETHODIMP QueryDirection(PIN_DIRECTION *d) { *d = PINDIR_OUTPUT; return S_OK; }
    STDMETHODIMP QueryId(LPWSTR *id)
    {
        *id = (LPWSTR)CoTaskMemAlloc(8 * sizeof(WCHAR));
        if (!*id)
            return E_OUTOFMEMORY;
        wcscpy(*id, L"Out");
        return S_OK;
    }
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE *mt) { return memcmp(&mt->majortype, &MEDIATYPE_Video, sizeof(GUID)) ? S_FALSE : S_OK; }
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes **pp)
    {
        *pp = new TypeEnum(&g_feedType);
        return *pp ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP QueryInternalConnections(IPin **a, ULONG *n) { (void)a; (void)n; return E_NOTIMPL; }
    STDMETHODIMP EndOfStream() { return E_UNEXPECTED; }
    STDMETHODIMP BeginFlush() { return E_UNEXPECTED; }
    STDMETHODIMP EndFlush() { return E_UNEXPECTED; }
    STDMETHODIMP NewSegment(REFERENCE_TIME a, REFERENCE_TIME b, double r) { (void)a; (void)b; (void)r; return S_OK; }
};

static FeedPin       g_feedPin;
static HANDLE        g_feedThread;
static DWORD         g_runTick;         /* GetTickCount() at IMediaControl::Run */
static volatile BOOL g_videoOn;         /* a decoder graph is running: give it the video records */
static volatile BOOL g_waitIdr;         /* graph just (re)started: skip video until an IDR */
static volatile BOOL g_rewind;          /* graph just started: go back to the newest IDR still in the ring */
static DWORD         g_audioUntil;      /* after a rewind: audio before this position was already played */
static CRITICAL_SECTION g_vidLock;      /* held while a record is being handed to the decoder */
static volatile LONG g_audioRecs, g_audioDrops;

/* ---------- audio: PCM records from the Pi, one waveOut per Android Auto stream ---------- */

#define AUD_STREAMS 3                   /* 0 media 48 kHz stereo, 1 speech and 2 system 16 kHz mono */
#define AUD_BUFS    48                  /* each holds ~40 ms (packed from the phone's small chunks) */
#define AUD_BUF     8192
static const DWORD s_audRate[AUD_STREAMS] = { 48000, 16000, 16000 };
static const WORD  s_audCh[AUD_STREAMS]   = { 2, 1, 1 };
static HWAVEOUT    g_wo[AUD_STREAMS];
static BOOL        g_woFailed[AUD_STREAMS];
static WAVEHDR     g_wh[AUD_STREAMS][AUD_BUFS];
static BYTE       *g_wbuf[AUD_STREAMS];
static int         g_wnext[AUD_STREAMS];
static BOOL        g_audPriming[AUD_STREAMS];   /* paused, filling the jitter buffer */

static BOOL g_grabAudio = TRUE;         /* "nograb": leave the head unit's audio source alone */
static DWORD g_mediaBuf = 600;          /* "abufN": media jitter buffer, ms (100..1500) */

/*
 * The head unit's audio clock. Its "48000 Hz" output plays about 0.6% fast. The phone's music arrives at 48000
 * frames/s by this unit's own clock (47963 and 47974 measured over 15-18 minutes), yet without correction the queue
 * loses about 40 ms every 5-6 s and runs dry every minute or so, whatever the Wi-Fi does. So every stream is
 * resampled by 1 + g_ratePpm/1e6 (4-point cubic), and the music queue's level, averaged over about 5 s of audio and
 * compared with the level the jitter buffer starts from, steers g_ratePpm.
 */
static BOOL g_rateFix = TRUE;           /* "noarate": off */
static LONG g_ratePpm = 7000;           /* "arateN": where it starts; then kept by rateServo */
static LONG g_rateInt = 7000;           /* the learned part: the device's measured clock (see audioClockTick) */
static BOOL  g_clockKnown;              /* g_rateInt is a measurement: this run's, or the last one (aaview_clock.txt) */
static WCHAR g_clockFile[MAX_PATH];     /* aaview_clock.txt next to the log */
static DWORD g_musicDry;                /* times music ran dry (each starts the clock measurement again) */
static volatile DWORD g_musicArrived;   /* when the last music record arrived */
#define RATE_MIN    (-6000)             /* what is applied: the learned part +- 6000 */
#define RATE_MAX    21000
#define RATE_INT_MIN 0                  /* the learned part */
#define RATE_INT_MAX 15000
#define RS_FRAMES   2048                /* input frames per resample() call */

static LONG clampL(LONG v, LONG lo, LONG hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* level: the music queued when a buffer was added, averaged over the last ~5 s, in ms. */
static void rateServo(DWORD level)
{
    static DWORD n;
    LONG         raw = (LONG)g_mediaBuf - 20 - (LONG)level;                         /* + = draining */
    LONG         err = clampL(raw, -150, 150);

    /* The level steers only the proportional part, 40 ppm per ms off the target and at most +-6000. The learned part
     * is the device's measured clock, and a few hundred ppm between the phone's clock and ours leaves the queue only a
     * few ms off the target. The level must not teach the learned part: after a Wi-Fi stall the held-back audio
     * arrives all at once, and the high queue would drag it down to 0. */
    g_ratePpm = clampL(g_rateInt + 40 * err, RATE_MIN, RATE_MAX);
    if (n++ % 12 == 0 || err >= 100 || err <= -100)
        logd(L"   %u s audio clock: %d ppm (the device's clock %d%s); music queued %u ms on average\r\n",
             GetTickCount() / 1000, g_ratePpm, g_rateInt, g_clockKnown ? L"" : L", not measured yet", level);
}

/* the music buffers queued on the device (each ~40 ms) */
static int musicQueued(void)
{
    int i, q = 0;

    for (i = 0; i < AUD_BUFS; i++)
        if ((g_wh[0][i].dwFlags & WHDR_PREPARED) && !(g_wh[0][i].dwFlags & WHDR_DONE))
            q++;
    return q;
}

static void clockSave(LONG ppm)
{
    HANDLE f = CreateFile(g_clockFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    char   b[32];
    DWORD  n;

    if (f == INVALID_HANDLE_VALUE)
        return;
    sprintf(b, "%ld\r\n", (long)ppm);
    WriteFile(f, b, (DWORD)strlen(b), &n, NULL);
    CloseHandle(f);
}

static void clockLoad(void)
{
    HANDLE f = CreateFile(g_clockFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    char   b[32];
    DWORD  n = 0;

    if (f == INVALID_HANDLE_VALUE)
        return;
    if (ReadFile(f, b, sizeof(b) - 1, &n, NULL) && n) {
        LONG v;
        b[n] = 0;
        v = atol(b);
        if (v > RATE_INT_MIN && v < RATE_INT_MAX) {
            g_ratePpm = g_rateInt = v;
            g_clockKnown = TRUE;
        }
    }
    CloseHandle(f);
}

/*
 * Runs on every pass of the feeder, which is also the thread that queues the music.
 *   Every 50 ms, while music arrives and plays, it samples the music queue's level and hands rateServo the average
 *   over 5 s. Sampling on a timer matters: read only when a buffer is queued, the level is always the one just after
 *   a burst.
 *   Every second it measures the device's own clock: its play position (waveOutGetPosition) against GetTickCount,
 *   over a stretch of uninterrupted playing at least a minute long. Running dry, an empty queue or the device
 *   starting over ends the stretch, because the position stands still then. After 10 minutes the stretch's start
 *   moves up to half way, so the measurement follows the device as it warms up.
 */
static void audioClockTick(void)
{
    static DWORD lastLevel, lastClock, lvlSum, lvlN, lvlDry, segPos, segTick, segDry, bad, saidAt;
    static LONG  saved = 0x7fffffff, said = 0x7fffffff;
    static BOOL  seg;
    DWORD        now = GetTickCount();

    if (!g_rateFix || !g_wo[0])
        return;
    if (now - lastLevel >= 50) {
        lastLevel = now;
        if (g_musicDry != lvlDry) {         /* a window with a dry-out in it says nothing about the clock */
            lvlDry = g_musicDry;
            lvlSum = lvlN = 0;
        }
        if (!g_audPriming[0] && now - g_musicArrived < 1000) {
            lvlSum += (DWORD)musicQueued() * 40;
            if (++lvlN >= 100) {
                rateServo(lvlSum / lvlN);
                lvlSum = lvlN = 0;
            }
        }
    }
    if (now - lastClock >= 1000) {
        MMTIME mt;
        DWORD  pos;

        lastClock = now;
        memset(&mt, 0, sizeof(mt));
        mt.wType = TIME_SAMPLES;
        if (waveOutGetPosition(g_wo[0], &mt, sizeof(mt)) != MMSYSERR_NOERROR) {
            seg = FALSE;
            return;
        }
        if (mt.wType == TIME_SAMPLES)
            pos = mt.u.sample;
        else if (mt.wType == TIME_BYTES)
            pos = mt.u.cb / 4;              /* 48 kHz stereo, 16 bit */
        else {
            if (bad++ == 0)
                logw(L"   %u s audio clock: the device gives its position only as time type %u: not measured\r\n",
                     now / 1000, mt.wType);
            seg = FALSE;
            return;
        }
        if (g_audPriming[0] || !musicQueued() || (seg && (g_musicDry != segDry || pos < segPos))) {
            seg = FALSE;
            return;
        }
        if (!seg) {
            seg = TRUE;
            segPos = pos;
            segTick = now;
            segDry = g_musicDry;
            return;
        }
        if (now - segTick >= 60000) {
            LONG ppm = (LONG)(((double)(pos - segPos) * 1000.0 / (double)(now - segTick) / 48000.0 - 1.0) * 1e6);

            if (ppm <= RATE_INT_MIN || ppm >= RATE_INT_MAX) {
                if (bad++ < 3)
                    logw(L"   %u s audio clock: the device's position says %d ppm over %u s: not believable, not "
                         L"used\r\n", now / 1000, ppm, (now - segTick) / 1000);
                seg = FALSE;
                return;
            }
            /* Logged the first time, on a change of 300 ppm or more, and every 10 minutes. The position moves in steps
             * of about 10 ms, so each second's value wobbles by +-60 ppm. */
            if (said == 0x7fffffff || ppm - said >= 300 || said - ppm >= 300 || now - saidAt >= 600000) {
                logmsg(L"   %u s audio clock: the head unit plays %d ppm fast (measured over %u s of music)\r\n",
                       now / 1000, ppm, (now - segTick) / 1000);
                said = ppm;
                saidAt = now;
            }
            g_rateInt = ppm;
            g_clockKnown = TRUE;
            if (ppm - saved >= 100 || saved - ppm >= 100) {
                saved = ppm;
                clockSave(ppm);             /* for the next start */
            }
            if (now - segTick >= 600000) {
                segPos += (pos - segPos) / 2;
                segTick += (now - segTick) / 2;
            }
        }
    }
}

/* frames (<= RS_FRAMES) of stream st's 16-bit PCM -> out; returns the frames written (at most frames * 1.025 + 2).
 * Catmull-Rom through 4 input frames; the last 3 are kept for the next call. */
static DWORD resample(int st, const BYTE *in, DWORD frames, short *out)
{
    static short work[(RS_FRAMES + 3) * 2];
    static short hist[AUD_STREAMS][3 * 2];
    static DWORD pos[AUD_STREAMS];      /* 12.20 fixed point, in frames from work[0] */
    DWORD        ch = s_audCh[st], total = frames + 3, p, i, c, n = 0;
    DWORD        step = (DWORD)(((ULONGLONG)1000000 << 20) / (ULONGLONG)(1000000 + g_ratePpm));

    if (!pos[st])
        pos[st] = 1 << 20;
    memcpy(work, hist[st], 3 * ch * sizeof(short));
    memcpy(work + 3 * ch, in, frames * ch * sizeof(short));
    for (p = pos[st]; (i = p >> 20) + 2 < total; p += step, n++) {
        int x = (int)((p >> 9) & 0x7FF);                /* 0..2047 between frames i and i+1 */

        for (c = 0; c < ch; c++) {
            int y0 = work[(i - 1) * ch + c], y1 = work[i * ch + c], y2 = work[(i + 1) * ch + c], y3 = work[(i + 2) * ch + c];
            int c1 = y2 - y0, c2 = 2 * y0 - 5 * y1 + 4 * y2 - y3, c3 = (y3 - y0) + 3 * (y1 - y2);
            int v = y1 + ((((((((c3 * x) >> 11) + c2) * x) >> 11) + c1) * x) >> 12);

            out[n * ch + c] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    }
    memcpy(hist[st], work + (total - 3) * ch, 3 * ch * sizeof(short));
    pos[st] = p - ((total - 3) << 20);
    return n;
}
static volatile DWORD g_audLow = 0xFFFFFFFF;    /* least music queued when a buffer was added, ms (stall report) */

/*
 * The head unit plays our waveOut audio only while its audio source is the CPU's (USB media). kronos switches
 * sources by sending phoenix RELEASE_SOUND {source, extra}; USB media is {3, 1}. We send
 * the same message, as kronos (IPC id 6):
 *   - when the Pi says Android Auto media started (play pressed; header flags bits 8..15 count those),
 *   - when the voice key is pressed (so the assistant is heard),
 *   - as a fallback, when media audio arrives after 1.5 s of silence (at most every 5 s).
 * A RADIO/FM press during a song isn't fought over: the Pi pauses the phone's music for those keys.
 */
static BOOL g_grabbed;                  /* we took the audio source since the phone connected */
static BOOL g_restore = TRUE;           /* "norestore": leave the source on USB when the phone goes */

/*
 * The car's own phone screens. Calls never go through Android Auto: the phone stays on the car's Bluetooth
 * hands-free, and LG's phone apps (s_phoneApps) show the call. From the moment one of their windows takes the
 * foreground until it closes or another full screen replaces it (g_phoneUp, kept by trackForeground):
 *   - the screen isn't taken back, or the call's buttons couldn't be reached. The floating icon shows meanwhile,
 *     and a tap on it brings Android Auto back.
 *   - the audio source isn't touched: no switch to USB media, and no "phone left -> Bluetooth audio". The Android
 *     Auto session can drop and come back several times during a call, and switching the source under the call
 *     sends the call back to the handset. What was held back is done 2 s after the phone screen goes.
 */
static const WCHAR *s_phoneApps[] = { L"hermes.exe", L"hyperion.exe", NULL };
static BOOL         g_phoneUp;          /* a phone app's window has the foreground (or a popup over it has) */
static HWND         g_phoneWnd;         /* that window */
static DWORD        g_phoneOver;        /* tick when g_phoneUp last ended */
static const WCHAR *g_grabWish;         /* held back meanwhile: a switch to USB media (the reason) */
/* Not every full screen of a phone app is a call: LG's Bluetooth Audio screen is hyperion's as well, and it is what
 * phoneLeft() asks for. Taken for a call's screen, it would keep Android Auto behind it, silent, after the phone
 * came back. It is known by its window: the phone app screen that comes up right after we asked for it, or that LG
 * puts back when the camera goes (its current mode's screen). */
static DWORD        g_modeAsked;        /* tick when phoneLeft() asked for Bluetooth Audio */
static HWND         g_modeWnd;          /* a phone app's window that is LG's mode screen, not a call */
/* Nor is a phone app's full screen that was simply there: under us when we hide (if we hide for a moment, the
 * screen underneath gets the foreground), or in front when the head unit starts up or projection begins. Every one
 * of these seen so far was LG's mode screen. So such a screen counts as the car's phone screen only when it takes
 * the screen from Android Auto (a call coming in, or the PHONE or MEDIA button). */
static BOOL         g_ourScreen;        /* the last full-screen foreground window was ours, and showing */
static BOOL         g_leftWish;         /* held back meanwhile: the phone left */

static void grabNow(const WCHAR *why)
{
    static DWORD lastGrab;
    DWORD        now = GetTickCount();
    HWND         pnx;

    if (!g_grabAudio)
        return;
    if (g_phoneUp) {
        if (!g_grabWish)
            logmsg(L"   %u s %s: the car's phone screen is up, leaving the audio source alone\r\n", now / 1000, why);
        g_grabWish = why;
        return;
    }
    if (lastGrab && now - lastGrab < 1500)
        return;
    lastGrab = now;
    g_grabbed = TRUE;
    pnx = FindWindow(NULL, L"phoenix");
    logmsg(L"   %u s %s: audio source -> USB media (phoenix 0x%08X): %d\r\n", now / 1000, why, (DWORD)pnx,
           pnx ? PostMessage(pnx, 0x8096, 0x00020602, 0x0103) : 0);
}

static void grabAudioSource(int st)
{
    static DWORD last[AUD_STREAMS], lastFallback;
    DWORD        now = GetTickCount();
    BOOL         starting = !last[st] || now - last[st] > 1500;

    last[st] = now;
    if (!starting)
        return;
    if (st != 0) {
        /* A chime or a voice prompt while no music has played since the phone (re)connected. After a session drop
         * phoneLeft() has put the head unit on Bluetooth Audio, and if only music took it back the phone's message
         * chimes would go nowhere. Not when we already hold the source, or when the user has since picked another
         * one such as the radio: that stays theirs. */
        if (!g_grabbed)
            grabNow(L"a sound from the phone with no music yet");
        return;
    }
    if (lastFallback && now - lastFallback < 5000)
        return;
    lastFallback = now;
    grabNow(L"media audio after a pause");
}

/* When the phone's session ends after we took the audio source: hand the head unit to Bluetooth audio (the phone,
 * still paired with the car, carries on there). That's cupid's "show phone music" to kronos, what LG's Media menu
 * sends for BT Audio: CUPID_CMD_KRONOS_SHOW_PH_MUSIC 0x3b, from cupid (IPC id 12). */
static void phoneLeft(void)
{
    HWND k = FindWindow(NULL, L"kronos");

    logmsg(L"   %u s the phone left: head unit -> Bluetooth audio (kronos 0x%08X): %d\r\n", GetTickCount() / 1000,
           (DWORD)k, k ? PostMessage(k, 0x8096, 0x003B0C00, 0) : 0);
    g_modeAsked = GetTickCount();
    g_grabbed = FALSE;
}

static void watchPhoneGone(void)
{
    static int last = -1;
    int        phone = (g_hdr.flags & 4) ? 1 : 0;

    if (g_hdr.magic != 0x31535641)
        return;
    if (last == 1 && phone == 0) {
        g_grabWish = NULL;
        if (g_grabbed && g_restore) {
            if (g_phoneUp) {
                g_leftWish = TRUE;
                logmsg(L"   %u s the phone left while the car's phone screen is up: leaving the audio source alone\r\n",
                       GetTickCount() / 1000);
            } else {
                phoneLeft();
            }
        }
    } else if (phone == 1) {
        g_leftWish = FALSE;             /* it came back meanwhile */
    }
    last = phone;
}

static BOOL projecting(void);

/* When a call ends while we hold the audio source, LG goes back to whatever it played before the call, which can
 * be the radio. The phone's music then has nowhere to go. So 2 s after hyperion says the call is over (and its
 * screen has gone), the source is switched back to USB media. */
static DWORD g_callEnded;               /* tick of hyperion's "call end" or "call reject", 0 = none pending */

static const WCHAR *phoneEventName(DWORD fnc)
{
    switch (fnc) {
    case 0x3c: return L"hands-free connected";
    case 0x3d: return L"Bluetooth audio connected";
    case 0x3e: return L"hands-free disconnected";
    case 0x3f: return L"Bluetooth audio disconnected";
    case 0x46: return L"call: dialling";
    case 0x47: return L"call: incoming";
    case 0x48: return L"call: ongoing";
    case 0x49: return L"call: ended";
    case 0x4a: return L"call: answered";
    case 0x4b: return L"call: rejected";
    case 0x4c: return L"call: missed";
    case 0x4d: return L"call: on the hands-free";
    case 0x50: return L"Bluetooth audio: started";
    case 0x51: return L"Bluetooth audio: stopped";
    case 0x52: return L"Bluetooth audio: playing";
    case 0x53: return L"Bluetooth audio: paused";
    }
    return L"?";
}

static void phoneEvent(DWORD fnc, DWORD data)
{
    logmsg(L"   %u s car's phone: %s (hyperion 0x%02X data 0x%X)\r\n", GetTickCount() / 1000, phoneEventName(fnc),
           fnc, data);
    if ((fnc == 0x49 || fnc == 0x4b) && g_grabbed)
        g_callEnded = GetTickCount() | 1;
}

/* The car's phone screen has gone (2 s ago, so LG's own switch back from the call is done): do what was held back. */
static void afterPhoneScreen(void)
{
    if (g_callEnded && !g_phoneUp && GetTickCount() - g_callEnded >= 2000 && GetTickCount() - g_phoneOver >= 2000) {
        g_callEnded = 0;
        if (projecting())
            grabNow(L"the call ended");
    }
    if (g_phoneUp || (!g_leftWish && !g_grabWish) || GetTickCount() - g_phoneOver < 2000)
        return;
    if (g_leftWish) {
        g_leftWish = FALSE;
        g_grabWish = NULL;
        if (g_grabbed && g_restore)
            phoneLeft();
    } else {
        const WCHAR *why = g_grabWish;
        g_grabWish = NULL;
        if (projecting())
            grabNow(why);
    }
}

/* From the main loop: the Pi's media-start counter (header flags bits 8..15). */
static void watchMediaStarts(void)
{
    static int last = -1;
    int        n;

    watchPhoneGone();
    afterPhoneScreen();
    if (g_hdr.magic != 0x31535641)
        return;
    n = (int)((g_hdr.flags >> 8) & 0xff);
    if (last >= 0 && n != last)
        grabNow(L"the phone started media");
    last = n;
}

static void audioQueue(int st, const BYTE *p, DWORD len);

static void audioPlay(int st, const BYTE *p, DWORD len)
{
    static short out[(RS_FRAMES + RS_FRAMES / 16 + 8) * 2];
    DWORD        blk;

    if (st < 0 || st >= AUD_STREAMS || g_woFailed[st])
        return;
    if (st == 0)
        g_musicArrived = GetTickCount();
    if (!g_rateFix) {
        audioQueue(st, p, len);
        return;
    }
    blk = s_audCh[st] * 2;
    while (len >= blk) {
        DWORD frames = len / blk > RS_FRAMES ? RS_FRAMES : len / blk;

        audioQueue(st, (const BYTE *)out, resample(st, p, frames, out) * blk);
        p += frames * blk;
        len -= frames * blk;
    }
}

static void audioQueue(int st, const BYTE *p, DWORD len)
{
    if (g_woFailed[st])
        return;
    grabAudioSource(st);
    if (!g_wo[st]) {
        WAVEFORMATEX wf;
        MMRESULT     mr;
        memset(&wf, 0, sizeof(wf));
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = s_audCh[st];
        wf.nSamplesPerSec = s_audRate[st];
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = (WORD)(wf.nChannels * 2);
        wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
        g_wbuf[st] = (BYTE *)malloc(AUD_BUFS * AUD_BUF);
        mr = g_wbuf[st] ? waveOutOpen(&g_wo[st], WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) : MMSYSERR_NOMEM;
        logd(L"   audio stream %d: waveOutOpen %u Hz x %u -> %u\r\n", st, wf.nSamplesPerSec, wf.nChannels, mr);
        if (mr != MMSYSERR_NOERROR) {
            g_wo[st] = NULL;
            g_woFailed[st] = TRUE;
            return;
        }
        memset(g_wh[st], 0, sizeof(g_wh[st]));
    }
    {
        /* Pack the phone's small chunks into ~40 ms buffers (fewer, fuller waveOut buffers), and keep a jitter
         * buffer: when nothing is queued (start, or the network ran it dry) pause the device and restart only once
         * enough is queued, so one late chunk doesn't turn into a gap every time: 300 ms for speech and system
         * sounds, g_mediaBuf (600 ms, "abufN") for media. Over Wi-Fi the phone's media audio arrives 160-535 ms
         * late several times a minute, more than 300 ms covers. */
        static DWORD fill[AUD_STREAMS], queuedBytes[AUD_STREAMS], underruns[AUD_STREAMS];
        DWORD        perSec = s_audRate[st] * s_audCh[st] * 2;
        DWORD        chunk = (perSec / 25) & ~3u, target = perSec / 1000 * (st == 0 ? g_mediaBuf : 300);
        int          i, queued = 0;

        if (chunk > AUD_BUF)
            chunk = AUD_BUF;
        while (len) {
            WAVEHDR *h = &g_wh[st][g_wnext[st]];
            DWORD    n;

            if (fill[st] == 0) {        /* starting a new buffer: is it free? */
                if ((h->dwFlags & WHDR_PREPARED) && !(h->dwFlags & WHDR_DONE)) {
                    InterlockedIncrement((LONG *)&g_audioDrops);   /* all queued: more than ~2 s behind */
                    return;
                }
                if (h->dwFlags & WHDR_PREPARED)
                    waveOutUnprepareHeader(g_wo[st], h, sizeof(*h));
            }
            n = chunk - fill[st];
            if (n > len)
                n = len;
            memcpy(g_wbuf[st] + g_wnext[st] * AUD_BUF + fill[st], p, n);
            fill[st] += n;
            p += n;
            len -= n;
            if (fill[st] < chunk)
                break;                  /* keep filling with the next chunk */

            queued = 0;
            for (i = 0; i < AUD_BUFS; i++)
                if ((g_wh[st][i].dwFlags & WHDR_PREPARED) && !(g_wh[st][i].dwFlags & WHDR_DONE))
                    queued++;
            if (st == 0 && !g_audPriming[st] && (DWORD)queued * 40 < g_audLow)
                g_audLow = (DWORD)queued * 40;
            if (!queued && !g_audPriming[st]) {
                if (st == 0)
                    g_musicDry++;
                /* counted per stream, so music can't use up the 200 lines and hide every chime's */
                if (underruns[st]++ < 200 || underruns[st] % 100 == 0)
                    logw(L"   %u s audio stream %d: ran dry (%u times); buffering %u ms\r\n", GetTickCount() / 1000, st,
                           underruns[st], st == 0 ? g_mediaBuf : 300);
                waveOutPause(g_wo[st]);
                g_audPriming[st] = TRUE;
                queuedBytes[st] = 0;
            }
            memset(h, 0, sizeof(*h));
            h->lpData = (LPSTR)(g_wbuf[st] + g_wnext[st] * AUD_BUF);
            h->dwBufferLength = fill[st];
            waveOutPrepareHeader(g_wo[st], h, sizeof(*h));
            waveOutWrite(g_wo[st], h, sizeof(*h));
            g_wnext[st] = (g_wnext[st] + 1) % AUD_BUFS;
            fill[st] = 0;
            if (g_audPriming[st] && (queuedBytes[st] += chunk) >= target) {
                g_audPriming[st] = FALSE;
                waveOutRestart(g_wo[st]);
            }
        }
    }
}

static HRESULT deliver(const BYTE *p, DWORD len, BOOL idr, BOOL disc)
{
    IMediaSample *s = NULL;
    BYTE         *dst = NULL;
    HRESULT       hr;
    long          cap;

    hr = g_feedPin.m_alloc->GetBuffer(&s, NULL, NULL, 0);
    if (FAILED(hr) || !s)
        return FAILED(hr) ? hr : E_FAIL;
    s->GetPointer(&dst);
    cap = s->GetSize();
    if ((long)len > cap) {
        /* skipped: cut to fit, it would give the decoder a broken frame */
        s->Release();
        return VFW_E_BUFFER_OVERFLOW;
    }
    memcpy(dst, p, len);
    s->SetActualDataLength((long)len);
    if (g_tsMode >= 0) {
        REFERENCE_TIME ts = (REFERENCE_TIME)(GetTickCount() - g_runTick + (DWORD)g_tsMode) * 10000, te = ts + FRAME_100NS;
        s->SetTime(&ts, &te);
    }
    s->SetSyncPoint(idr);
    s->SetDiscontinuity(disc);
    hr = g_feedPin.m_memin->Receive(s);
    s->Release();
    return hr;
}

/*
 * The feeder: follow the Pi's write position, read every complete record since our read position in one span,
 * give H.264 records to the decoder (or just count bench records), then publish the new read position.
 */
static DWORD WINAPI feedThread(LPVOID arg)
{
    DWORD session = 0, idle = 0, lastStat = GetTickCount(), bytes = 0, recs = 0, polls = 0, fails = 0;
    DWORD lastRec = 0, gaps = 0, lastLoop = 0, worstLoop = 0;
    BOOL  synced = FALSE, disc = TRUE;
    int   loglines = 0, quitSeen = -1;

    (void)arg;
    while (!g_quit) {
        DWORD avail, n, head, off = 0, now = GetTickCount();

        polls++;
        if (lastLoop && now - lastLoop > worstLoop)
            worstLoop = now - lastLoop;         /* one pass normally takes a few ms */
        lastLoop = now;
        audioClockTick();
        if (!readHeader()) {
            if (++fails == 1 || fails % 500 == 0)
                logw(L"   feeder: no valid Pi header (%s, %u tries)\r\n", g_ioErr ? L"read error" : L"not written yet", fails);
            if (g_ioErr && fails % 25 == 0) {
                /* The Pi re-plugged its gadget, so the old store handle is dead. Find it again. */
                EnterCriticalSection(&g_ioLock);
                CloseHandle(g_stream);
                g_stream = INVALID_HANDLE_VALUE;
                streamOpen();
                LeaveCriticalSection(&g_ioLock);
                synced = FALSE;
            }
            Sleep(20);
            continue;
        }
        fails = 0;
        if (quitSeen < 0)
            quitSeen = (int)(g_hdr.flags & 1);  /* a quit flag left over from before we started doesn't count */
        if ((g_hdr.flags & 1) && !quitSeen) {
            logmsg(L"   feeder: the Pi asked us to quit\r\n");
            g_quit = TRUE;
            break;
        }
        if (!(g_hdr.flags & 1))
            quitSeen = 0;
        if (!synced || g_hdr.session != session) {
            /* (Re)start at the newest IDR the Pi still holds. */
            if (g_hdr.wr - g_hdr.idr_pos > g_hdr.ring_bytes || g_hdr.wr == g_hdr.idr_pos) {
                if (++idle % 50 == 0)
                    writeStatus();
                Sleep(10);
                continue;
            }
            logmsg(L"   feeder: Pi session %08X, %ux%u, ring %u KB; starting at IDR %u (wr %u)\r\n", g_hdr.session,
                   g_hdr.width, g_hdr.height, g_hdr.ring_bytes / 1024, g_hdr.idr_pos, g_hdr.wr);
            session = g_hdr.session;
            g_rd = g_hdr.idr_pos;
            synced = TRUE;
            disc = TRUE;
            lastRec = 0;
            g_audioUntil = g_rd;
        }
        if (g_rewind) {
            /* The phone sends a keyframe only now and then (maybe once per session): start the new decoder at the
             * newest one the ring still holds, rather than waiting for the next. */
            g_rewind = FALSE;
            if (g_hdr.wr - g_hdr.idr_pos <= g_hdr.ring_bytes - 65536 && g_rd - g_hdr.idr_pos <= g_hdr.ring_bytes &&
                g_hdr.idr_pos != g_rd) {
                logd(L"   feeder: rewinding %u KB to the newest IDR\r\n", (g_rd - g_hdr.idr_pos) / 1024);
                g_audioUntil = g_rd;
                g_rd = g_hdr.idr_pos;
                writeStatus();         /* the Pi keeps that span until we've read it again */
                disc = TRUE;
            }
        }
        avail = g_hdr.wr - g_rd;
        if (avail > g_hdr.ring_bytes) {
            logw(L"   feeder: overrun (rd %u, wr %u); resyncing\r\n", g_rd, g_hdr.wr);
            synced = FALSE;
            continue;
        }
        if (avail > LAG_BYTES && g_hdr.idr_pos != g_rd && g_hdr.idr_pos - g_rd < avail) {
            logw(L"   feeder: %u KB behind; skipping to the newest IDR\r\n", avail / 1024);
            InterlockedIncrement((LONG *)&g_dropped);
            g_rd = g_hdr.idr_pos;
            avail = g_hdr.wr - g_rd;
            disc = TRUE;
        }
        if (avail < 16) {
            if (++idle % 50 == 0)
                writeStatus();         /* heartbeat for the Pi, several times a second when idle */
            Sleep(4);
        } else {
            idle = 0;
            n = avail > STAGE_BYTES - 1024 ? STAGE_BYTES - 1024 : avail;
            if (!ringFetch(g_rd, n, &head)) {
                logw(L"   feeder: ring read failed at %u (%u bytes)\r\n", g_rd, n);
                Sleep(50);
                continue;
            }
            while (off + 16 <= n) {
                const BYTE *r = g_stage + head + off;
                DWORD len = *(const DWORD *)r, type = *(const WORD *)(r + 4), flags = *(const WORD *)(r + 6);
                DWORD recNo = *(const DWORD *)(r + 12), total = (16 + len + 15) & ~15u;

                if (len > g_hdr.ring_bytes / 2 || (type != REC_AU && type != REC_BENCH && type != REC_AUDIO)) {
                    logw(L"   feeder: bad record at %u (len %u, type %u); resyncing\r\n", g_rd + off, len, type);
                    synced = FALSE;
                    break;
                }
                if (len > STAGE_BYTES - 2048) {
                    /* Too big for one read of the ring. Treated as a bad record, the resync would go back to the
                     * same IDR forever, so skip it unread once all of it is in the ring. */
                    if (off + total > avail)
                        break;          /* not all written yet: next time */
                    logw(L"   feeder: record %u of %u KB is bigger than a ring read (%u KB): skipped%s\r\n", recNo,
                         len / 1024, (STAGE_BYTES - 2048) / 1024, (type == REC_AU && (flags & 1)) ? L"; video waits "
                         L"for the next IDR" : L"");
                    if (type == REC_AU) {
                        InterlockedIncrement((LONG *)&g_dropped);
                        if (flags & 1)
                            g_waitIdr = TRUE;
                    }
                    lastRec = recNo;
                    off += total;
                    break;              /* past what was read: read again from there */
                }
                if (off + total > n)
                    break;              /* not all here yet: next time */
                if (lastRec && recNo != lastRec + 1)
                    gaps++;
                lastRec = recNo;
                if (type == REC_AUDIO) {
                    InterlockedIncrement((LONG *)&g_audioRecs);
                    if ((LONG)(g_rd + off - g_audioUntil) >= 0)    /* not a replay after a rewind */
                        audioPlay(flags, r + 16, len);
                } else if (type == REC_AU && g_videoOn && (!g_waitIdr || (flags & 1))) {
                    HRESULT hr = E_ABORT;
                    EnterCriticalSection(&g_vidLock);
                    if (g_videoOn) {
                        if (g_waitIdr)
                            disc = TRUE;
                        DWORD td = GetTickCount();
                        g_waitIdr = FALSE;
                        hr = deliver(r + 16, len, flags & 1, disc);
                        td = GetTickCount() - td;
                        if (td > g_worstRecv)
                            g_worstRecv = td;
                    }
                    LeaveCriticalSection(&g_vidLock);
                    disc = FALSE;
                    InterlockedIncrement((LONG *)&g_fed);
                    if (hr == VFW_E_BUFFER_OVERFLOW) {
                        /* bigger than the decoder's buffer: skip it, and if it was an IDR wait for the next */
                        InterlockedIncrement((LONG *)&g_dropped);
                        if (flags & 1)
                            g_waitIdr = TRUE;
                        disc = TRUE;
                        if (loglines++ < 20)
                            logw(L"   feeder: record %u of %u bytes is bigger than the decoder's buffer: skipped%s\r\n",
                                 recNo, len, (flags & 1) ? L"; video waits for the next IDR" : L"");
                    } else if (FAILED(hr)) {
                        InterlockedIncrement((LONG *)&g_feedFails);
                        if (loglines++ < 20)
                            logw(L"   feeder: Receive 0x%08X at record %u\r\n", hr, recNo);
                    } else if (g_fed <= 5) {
                        logd(L"   feeder: record %u (%u bytes%s) -> 0x%08X at %u ms\r\n", recNo, len,
                               (flags & 1) ? L", IDR" : L"", hr, GetTickCount() - g_runTick);
                    }
                }
                bytes += len;
                recs++;
                off += total;
            }
            if (!synced)
                continue;
            g_rd += off;
            writeStatus();
        }
        if (GetTickCount() - lastStat >= 5000) {
            DWORD ms = GetTickCount() - lastStat;
            if (recs)
                logd(L"   %u s: %u records, %u KB/s, %u polls, fed %d (%d failed), decoded %d, skips %d, gaps %u, "
                       L"behind %u KB, audio %d (%d dropped)\r\n",
                       GetTickCount() / 1000, recs, (DWORD)((__int64)bytes * 1000 / ms / 1024), polls, g_fed,
                       g_feedFails, g_decoded, g_dropped, gaps, (g_hdr.wr - g_rd) / 1024, g_audioRecs, g_audioDrops);
            /* What held the reader up? Music plays from this thread, so a stall here is a hiccup once it
             * outlasts what is queued. "loop" is the longest single pass; the rest say where it went. */
            if (recs && (worstLoop >= 150 || g_audLow < 150))
                logd(L"   %u s: longest stall %u ms (decoder %u, disk %u, log write %u); least music queued %d ms\r\n",
                       GetTickCount() / 1000, worstLoop, g_worstRecv, g_worstIo, g_worstLog,
                       g_audLow == 0xFFFFFFFF ? -1 : (int)g_audLow);
            worstLoop = 0;
            g_worstRecv = g_worstIo = g_worstLog = 0;
            g_audLow = 0xFFFFFFFF;
            lastStat = GetTickCount();
            bytes = recs = polls = 0;
        }
    }
    logmsg(L"   feeder: stopped (fed %d, %d failed)\r\n", g_fed, g_feedFails);
    return 0;
}

/* ---------- the microphone ----------
 * Android Auto's assistant listens through the head unit. When the phone opens its microphone channel the Pi sets
 * header flag bit 3 with a new mic_gen; we record wave-in device 1 at 16 kHz mono and write it to sectors 2-13 in
 * 62.5 ms chunks (layout at the top of this file) until the bit goes. Device 1 is the car's hands-free microphone:
 * measured on the head unit, device 0 delivers digital silence and device 1 a mono, voice-band signal that
 * picks up a tone played on the speakers.
 * Phone calls don't come this way; they stay on the car's Bluetooth.
 */
#define MIC_SECTOR      2
#define MIC_SLOTS       3
#define MIC_SLOT        2048
#define MIC_PCM         2000            /* bytes per chunk */
#define MIC_BUFS        8
#define HDR_MIC         8               /* Pi header flag: the phone wants the microphone */
#define ST_CAN_RECORD   4               /* our status flag: this aaview records when asked */

static int           g_micDev = 1;      /* "micN": the wave-in device; "nomic" (-1): never record */
static HANDLE        g_micThread;

static BOOL micWanted(DWORD *gen)
{
    PI_HDR h = g_hdr;                   /* copied without a lock; the caller asks twice before opening */

    *gen = h.mic_gen;
    return h.magic == 0x31535641 && (h.flags & HDR_MIC) && h.mic_gen && GetTickCount() - g_hdrTick < 5000;
}

static void micClose(HWAVEIN hwi, WAVEHDR *wh)
{
    int i;

    waveInReset(hwi);
    for (i = 0; i < MIC_BUFS; i++)
        if (wh[i].dwFlags & WHDR_PREPARED)
            waveInUnprepareHeader(hwi, &wh[i], sizeof(WAVEHDR));
    waveInClose(hwi);
}

static DWORD WINAPI micThread(LPVOID arg)
{
    static BYTE pcm[MIC_BUFS][MIC_PCM], slot[MIC_SLOT];
    WAVEHDR     wh[MIC_BUFS];
    HWAVEIN     hwi = NULL;
    DWORD       gen = 0, badGen = 0, seq = 0, fails = 0, t0 = 0, lastData = 0;
    int         k = 0, i, peak = 0;

    (void)arg;
    while (!g_quit) {
        DWORD wantGen, again;
        BOOL  want = micWanted(&wantGen);

        if (hwi && (!want || wantGen != gen || GetTickCount() - lastData > 3000)) {
            BOOL dead = want && wantGen == gen;

            micClose(hwi, wh);
            hwi = NULL;
            logmsg(L"%u s microphone: closed after %u ms: %u chunks sent to the Pi (%u writes failed), loudest sample "
                   L"%d of 32767%s\r\n", GetTickCount() / 1000, GetTickCount() - t0, seq, fails, peak,
                   dead ? L"; the device stopped delivering" : L"");
            if (dead)
                badGen = gen;           /* not again for this request */
            continue;
        }
        if (!hwi) {
            WAVEFORMATEX wf;
            MMRESULT     mr;

            if (!want || wantGen == badGen) {
                Sleep(40);
                continue;
            }
            Sleep(20);
            if (!micWanted(&again) || again != wantGen)
                continue;
            memset(&wf, 0, sizeof(wf));
            wf.wFormatTag = WAVE_FORMAT_PCM;
            wf.nChannels = 1;
            wf.nSamplesPerSec = 16000;
            wf.wBitsPerSample = 16;
            wf.nBlockAlign = 2;
            wf.nAvgBytesPerSec = 32000;
            memset(wh, 0, sizeof(wh));
            mr = waveInOpen(&hwi, (UINT)g_micDev, &wf, 0, 0, CALLBACK_NULL);
            if (mr == MMSYSERR_NOERROR) {
                for (i = 0; i < MIC_BUFS && mr == MMSYSERR_NOERROR; i++) {
                    wh[i].lpData = (LPSTR)pcm[i];
                    wh[i].dwBufferLength = MIC_PCM;
                    mr = waveInPrepareHeader(hwi, &wh[i], sizeof(WAVEHDR));
                    if (mr == MMSYSERR_NOERROR)
                        mr = waveInAddBuffer(hwi, &wh[i], sizeof(WAVEHDR));
                }
                if (mr == MMSYSERR_NOERROR)
                    mr = waveInStart(hwi);
            } else {
                hwi = NULL;
            }
            logmsg(L"%u s microphone: the phone is listening (request %u): wave-in device %d at 16 kHz mono -> %u%s\r\n",
                   GetTickCount() / 1000, wantGen, g_micDev, mr,
                   mr == MMSYSERR_NOERROR ? L"" : mr == MMSYSERR_ALLOCATED ? L" (in use by something else)" : L" (failed)");
            if (mr != MMSYSERR_NOERROR) {
                if (hwi)
                    micClose(hwi, wh);
                hwi = NULL;
                badGen = wantGen;
                continue;
            }
            gen = wantGen;
            seq = fails = 0;
            peak = k = 0;
            t0 = lastData = GetTickCount();
            continue;
        }
        if (!(wh[k].dwFlags & WHDR_DONE)) {
            Sleep(10);
            continue;
        }
        {
            DWORD        n = wh[k].dwBytesRecorded > MIC_PCM ? MIC_PCM : (wh[k].dwBytesRecorded & ~1u), j;
            const short *sp = (const short *)pcm[k];

            lastData = GetTickCount();
            for (j = 0; j < n / 2; j++) {
                int v = sp[j] < 0 ? -sp[j] : sp[j];
                if (v > peak)
                    peak = v;
            }
            seq++;
            memset(slot, 0, sizeof(slot));
            memcpy(slot, "AVM1", 4);
            memcpy(slot + 4, &gen, 4);
            memcpy(slot + 8, &seq, 4);
            memcpy(slot + 12, &n, 4);
            memcpy(slot + 16, pcm[k], n);
            memcpy(slot + MIC_SLOT - 4, &seq, 4);
            if (!streamIo(DISK_IOCTL_WRITE_2, MIC_SECTOR + (seq % MIC_SLOTS) * (MIC_SLOT / 512), slot, MIC_SLOT))
                fails++;
            wh[k].dwFlags &= ~WHDR_DONE;
            wh[k].dwBytesRecorded = 0;
            waveInAddBuffer(hwi, &wh[k], sizeof(WAVEHDR));
            k = (k + 1) % MIC_BUFS;
        }
    }
    if (hwi)
        micClose(hwi, wh);
    return 0;
}

class FeedSource : public FilterBase {
public:
    FeedSource() { wcscpy(m_name, L"AAView Source"); m_pin = &g_feedPin; g_feedPin.m_owner = this; }
    /* The ring reader runs all the time (audio plays without a graph); the graph only switches video delivery. */
    STDMETHODIMP Stop()
    {
        if (m_state != State_Stopped) {
            g_videoOn = FALSE;
            if (g_feedPin.m_alloc)
                g_feedPin.m_alloc->Decommit();      /* wakes a reader blocked in GetBuffer */
            EnterCriticalSection(&g_vidLock);       /* ... and waits until it has let go of the decoder */
            LeaveCriticalSection(&g_vidLock);
        }
        m_state = State_Stopped;
        return S_OK;
    }
    STDMETHODIMP Pause()
    {
        if (m_state == State_Stopped) {
            HRESULT hr = g_feedPin.m_alloc ? g_feedPin.m_alloc->Commit() : VFW_E_NOT_CONNECTED;
            logh(hr, L"   source Pause: allocator Commit 0x%08X\r\n", hr);
            if (FAILED(hr))
                return hr;
            g_fed = g_feedFails = 0;
            g_waitIdr = TRUE;
            g_rewind = TRUE;
            g_videoOn = TRUE;
        }
        m_state = State_Paused;
        return S_OK;
    }
    STDMETHODIMP Run(REFERENCE_TIME t)
    {
        (void)t;
        if (m_state == State_Stopped)
            Pause();
        m_state = State_Running;
        return S_OK;
    }
};

static FeedSource g_source;

/* ---------- window: full screen, black until video comes, every touch goes to the Pi ---------- */

static HWND  g_wnd;
static HWND  g_icon;                    /* the floating icon, shown while the phone isn't projecting */
static BOOL  g_down;
static DWORD g_lastMove;

static void onTouch(UINT msg, LPARAM lp)
{
    int x = (short)LOWORD(lp), y = (short)HIWORD(lp);

    if (msg == WM_LBUTTONDOWN) {
        if (g_down)
            return;                     /* the same tap again (via the renderer's message drain) */
        g_down = TRUE;
        touchEvent(0, x, y);
    } else if (msg == WM_LBUTTONUP) {
        if (g_down)
            touchEvent(1, x, y);
        g_down = FALSE;
    } else if (msg == WM_MOUSEMOVE && g_down && GetTickCount() - g_lastMove >= 15) {
        g_lastMove = GetTickCount();    /* at most ~60 moves a second */
        touchEvent(2, x, y);
    }
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT        r;
        HDC         dc = BeginPaint(hwnd, &ps);

        GetClientRect(hwnd, &r);
        FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        if (!g_decoded) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(0x80, 0x80, 0x80));
            DrawText(dc, L"aaview: waiting for video from the Pi", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MOUSEMOVE:
        onTouch(msg, lp);
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        /* Keys (hard keys / steering wheel, if the shell lets them through): to the Pi as events 3 (down) / 4 (up)
         * with x = virtual key, y = scan code; repeats are dropped. */
        BOOL down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        if (!down || !(lp & 0x40000000)) {
            logd(L"   key %s vk 0x%02X scan 0x%02X\r\n", down ? L"down" : L"up", (DWORD)wp, (DWORD)((lp >> 16) & 0xFF));
            touchEvent(down ? 3 : 4, (int)wp, (int)((lp >> 16) & 0xFF));
        }
        return 0;
    }
    case WM_APP + 0x20:                 /* MatMessage.dll (inside kronos): steering-wheel / remote key */
        logd(L"   %u s hard key %d (0x%X) data 0x%X\r\n", GetTickCount() / 1000, (int)wp, (DWORD)wp, (DWORD)lp);
        touchEvent(5, (int)wp, (int)lp);
        if (wp == 307 && projecting())
            grabNow(L"voice key");
        return 0;
    case WM_APP + 0x22:                 /* MatMessage.dll: phoenix's lights message (ILL_ON/OFF, NIGHT_MODE) */
        logmsg(L"   %u s lights: phoenix command 0x%02X data 0x%X\r\n", GetTickCount() / 1000, (DWORD)wp, (DWORD)lp);
        if (wp == 0x77 || wp == 0x78) {
            g_lightCmd = (DWORD)wp;
            g_lightData = (DWORD)lp;
        }
        touchEvent(8, (int)wp, (int)(lp & 0xffff));
        return 0;
    case WM_APP + 0x23:                 /* MatMessage.dll: hyperion's call and Bluetooth audio messages */
        phoneEvent((DWORD)wp, (DWORD)lp);
        return 0;
    case WM_APP + 0x21:                 /* MatMessage.dll: another ISV event (power, AV off, eject, ...) */
        logd(L"   %u s ISV event 0x%04X code %d\r\n", GetTickCount() / 1000, (DWORD)wp, (int)lp);
        touchEvent(6, (int)wp, (int)lp);
        return 0;
    case WM_CLOSE:                      /* not a way out (see the top of this file) */
        logw(L"   %u s WM_CLOSE ignored: aaview only ends on the Pi's quit flag\r\n", GetTickCount() / 1000);
        return 0;
    }
    if (msg >= WM_USER) {
        static int lines;
        if (lines++ < 200)
            logd(L"   message 0x%04X (0x%08X, 0x%08X)\r\n", msg, (DWORD)wp, (DWORD)lp);
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static void pump(void)
{
    MSG msg;

    logFlush(FALSE);
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        /* Touches that land on the renderer's own window: translate to our coordinates and report them too. */
        if (msg.hwnd != g_wnd && msg.hwnd != g_icon && msg.hwnd && (msg.message == WM_LBUTTONDOWN || msg.message == WM_LBUTTONUP ||
                                               msg.message == WM_MOUSEMOVE)) {
            POINT pt = { (short)LOWORD(msg.lParam), (short)HIWORD(msg.lParam) };
            ClientToScreen(msg.hwnd, &pt);
            ScreenToClient(g_wnd, &pt);
            onTouch(msg.message, MAKELPARAM(pt.x, pt.y));
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

/* The rear camera. Its picture is on an overlay plane of its own, but the guide lines are drawn by venus.exe in an
 * ordinary full-screen window: class 'Dialog', no title, a new one each time reverse is engaged. The windows titled
 * "layer_control" and "venus" are LG's IPC windows and never become visible, so watching only those, we would take
 * the screen back and our key-coloured window would hide the guide lines for as long as the car is in reverse.
 * So from the moment a window of one of the camera's apps takes the foreground until another full screen replaces
 * it or its windows are gone (g_camUp, kept by trackForeground), we don't raise or activate our window at all.
 * venus shows two windows within a second, and the second takes the foreground before it is visible, so following a
 * single camera window would mistake the switch for the camera closing. The camera counts as up while any of its
 * apps has the foreground or shows a full-screen window. */
static const WCHAR *s_cameraApps[] = { L"venus.exe", L"layer_control.exe", NULL };
static const WCHAR *s_cameraWindows[] = { L"layer_control", L"venus", NULL };
static BOOL         g_camUp;            /* a camera app's window has the foreground (or a popup over it has) */
static HWND         g_camWnd;           /* that window (the latest) */
static BOOL         g_fgCam;            /* the foreground window is a camera app's */
static DWORD        g_camSeen;          /* tick when a camera app last had the foreground or a full screen showing */
static DWORD        g_camOver;          /* tick when g_camUp last ended */
static BOOL         g_camOverPhone;     /* the car's phone screen was in front when the camera came up */

static BOOL cameraUp(void)
{
    static BOOL last = -1;
    BOOL        up = g_camUp;
    int         i;

    for (i = 0; s_cameraWindows[i]; i++) {
        HWND w = FindWindow(NULL, s_cameraWindows[i]);
        if (w && IsWindowVisible(w))
            up = TRUE;
    }
    if (up != last) {
        logd(L"   %u s camera windows %s\r\n", GetTickCount() / 1000, up ? L"VISIBLE (not taking the screen)" : L"hidden");
        last = up;
    }
    return up;
}

/* ---------- whose window is it ---------- */

#ifndef TH32CS_SNAPNOHEAPS
#define TH32CS_SNAPNOHEAPS  0x40000000
#endif
typedef HANDLE (WINAPI *PFN_SNAPSHOT)(DWORD, DWORD);
typedef BOOL   (WINAPI *PFN_PROCWALK)(HANDLE, LPPROCESSENTRY32);
typedef BOOL   (WINAPI *PFN_CLOSESNAP)(HANDLE);

/* The exe name of a process ("?" if it can't be found), through toolhelp.dll. Names are remembered, since the LG
 * apps keep their process ids for the whole boot. Main thread only. */
static void exeOf(DWORD pid, WCHAR *out, int n)
{
    static struct { DWORD pid; WCHAR exe[40]; } cache[32];
    static int            used, next;
    static HMODULE        th;
    static PFN_SNAPSHOT   pSnap;
    static PFN_PROCWALK   pFirst, pNext;
    static PFN_CLOSESNAP  pClose;
    PROCESSENTRY32        pe;
    HANDLE                s;
    BOOL                  found = FALSE;
    int                   i;

    for (i = 0; i < used; i++)
        if (cache[i].pid == pid) {
            lstrcpyn(out, cache[i].exe, n);
            return;
        }
    lstrcpyn(out, L"?", n);
    if (!th && (th = LoadLibrary(L"toolhelp.dll")) != NULL) {
        pSnap = (PFN_SNAPSHOT)GetProcAddress(th, L"CreateToolhelp32Snapshot");
        pFirst = (PFN_PROCWALK)GetProcAddress(th, L"Process32First");
        pNext = (PFN_PROCWALK)GetProcAddress(th, L"Process32Next");
        pClose = (PFN_CLOSESNAP)GetProcAddress(th, L"CloseToolhelp32Snapshot");
    }
    if (!pSnap || !pFirst || !pNext || !pClose)
        return;
    s = pSnap(TH32CS_SNAPPROCESS | TH32CS_SNAPNOHEAPS, 0);
    if (s == INVALID_HANDLE_VALUE)
        return;
    pe.dwSize = sizeof(pe);
    if (pFirst(s, &pe))
        do {
            if (pe.th32ProcessID == pid) {
                lstrcpyn(out, pe.szExeFile, n);
                found = TRUE;
                break;
            }
        } while (pNext(s, &pe));
    pClose(s);
    if (found) {                        /* a failed lookup isn't remembered */
        i = used < 32 ? used++ : next++ % 32;
        cache[i].pid = pid;
        lstrcpyn(cache[i].exe, out, 40);
    }
}

static BOOL isPhoneApp(const WCHAR *exe)
{
    int i;

    for (i = 0; s_phoneApps[i]; i++)
        if (!_wcsicmp(exe, s_phoneApps[i]))
            return TRUE;
    return FALSE;
}

static BOOL isCameraApp(const WCHAR *exe)
{
    int i;

    for (i = 0; s_cameraApps[i]; i++)
        if (!_wcsicmp(exe, s_cameraApps[i]))
            return TRUE;
    return FALSE;
}

/* At least 80% of the screen: another mode's screen, not a popup (volume, warnings). */
static BOOL bigWindow(HWND w)
{
    RECT r;

    return w && GetWindowRect(w, &r) &&
           (r.right - r.left) * (r.bottom - r.top) >= GetSystemMetrics(SM_CXSCREEN) * GetSystemMetrics(SM_CYSCREEN) * 8 / 10;
}

/* For the log: the phone apps' visible top-level windows, whenever that set changes. It is there to show whether
 * "a phone window is visible" tells a call from no call, whatever has the foreground. */
static WCHAR g_phoneWins[400];

static BOOL CALLBACK phoneWinsCb(HWND w, LPARAM lp)
{
    WCHAR exe[40], one[80];
    DWORD pid = 0;
    RECT  r;

    (void)lp;
    if (!IsWindowVisible(w) || w == g_wnd || w == g_icon)
        return TRUE;
    GetWindowThreadProcessId(w, &pid);
    exeOf(pid, exe, 40);
    if (isPhoneApp(exe) && GetWindowRect(w, &r)) {
        wsprintf(one, L" %s 0x%08X %dx%d", exe, (DWORD)w, r.right - r.left, r.bottom - r.top);
        if (wcslen(g_phoneWins) + wcslen(one) < 399)
            wcscat(g_phoneWins, one);
    }
    return TRUE;
}

static void logPhoneWindows(void)
{
    static WCHAR last[400];

    g_phoneWins[0] = 0;
    EnumWindows(phoneWinsCb, 0);
    if (wcscmp(g_phoneWins, last)) {
        logd(L"   %u s phone apps' visible windows:%s\r\n", GetTickCount() / 1000, g_phoneWins[0] ? g_phoneWins : L" none");
        wcscpy(last, g_phoneWins);
    }
}

/* The camera apps' visible top-level windows: how many are full-screen, the topmost of those (to show behind), and
 * for the log the whole set whenever it changes. */
static WCHAR g_camWins[300];
static int   g_camBig;
static HWND  g_camTop;

static BOOL CALLBACK camWinsCb(HWND w, LPARAM lp)
{
    WCHAR exe[40], one[80];
    DWORD pid = 0;
    RECT  r;

    (void)lp;
    if (!IsWindowVisible(w) || w == g_wnd || w == g_icon)
        return TRUE;
    GetWindowThreadProcessId(w, &pid);
    exeOf(pid, exe, 40);
    if (isCameraApp(exe) && GetWindowRect(w, &r)) {
        if (bigWindow(w) && !g_camBig++)
            g_camTop = w;
        wsprintf(one, L" %s 0x%08X %dx%d", exe, (DWORD)w, r.right - r.left, r.bottom - r.top);
        if (wcslen(g_camWins) + wcslen(one) < 299)
            wcscat(g_camWins, one);
    }
    return TRUE;
}

static int cameraWindows(void)
{
    static WCHAR last[300];

    g_camWins[0] = 0;
    g_camBig = 0;
    g_camTop = NULL;
    EnumWindows(camWinsCb, 0);
    if (wcscmp(g_camWins, last)) {
        logd(L"   %u s camera apps' visible windows:%s\r\n", GetTickCount() / 1000, g_camWins[0] ? g_camWins : L" none");
        wcscpy(last, g_camWins);
    }
    return g_camBig;
}

/* For the log: a phone app screen's child windows (class, id, place; never their text, which could name a caller).
 * It is there to learn what tells a call's screen from the phone menu and Bluetooth Audio, which are all hyperion
 * dialogs. */
static void logChildren(HWND w)
{
    WCHAR line[700] = L"", one[100], cls[32];
    HWND  c;
    int   n = 0;

    for (c = GetWindow(w, GW_CHILD); c && n < 40; c = GetWindow(c, GW_HWNDNEXT), n++) {
        RECT r = { 0, 0, 0, 0 };

        cls[0] = 0;
        GetClassName(c, cls, 32);
        GetWindowRect(c, &r);
        wsprintf(one, L" %s#%d%s@%d,%d+%dx%d", cls, GetDlgCtrlID(c), IsWindowVisible(c) ? L"" : L"(hidden)",
                 r.left, r.top, r.right - r.left, r.bottom - r.top);
        if (wcslen(line) + wcslen(one) < 699)
            wcscat(line, one);
    }
    logd(L"   %u s   its child windows (%d):%s\r\n", GetTickCount() / 1000, n, line);
}

static HWND  g_fg;                      /* the foreground window, as of the last trackForeground() */
static DWORD g_fgSince;

static void phoneScreenOver(const WCHAR *why)
{
    g_phoneUp = FALSE;
    g_phoneOver = GetTickCount();
    logmsg(L"   %u s the car's phone screen is over (%s)\r\n", g_phoneOver / 1000, why);
}

static void cameraOver(const WCHAR *why)
{
    g_camUp = FALSE;
    g_camOver = GetTickCount();
    logmsg(L"   %u s the rear camera's screen is over (%s)\r\n", g_camOver / 1000, why);
    planeAgain();
}

/* From both loops: logs every change of foreground window (with the window and exe that took it) and keeps
 * g_phoneUp (see s_phoneApps). */
static void trackForeground(void)
{
    HWND fg = GetForegroundWindow();

    if (g_modeWnd && !IsWindow(g_modeWnd))
        g_modeWnd = NULL;
    if (fg != g_fg) {
        WCHAR cls[64] = L"", title[64] = L"", exe[40] = L"";
        DWORD pid = 0;
        RECT  r = { 0, 0, 0, 0 };
        BOOL  phone = FALSE, cam = FALSE;
        BOOL  wasScreen = g_fg && g_fg != g_wnd && bigWindow(g_fg);    /* FALSE too if that window is gone */
        BOOL  fromUs = g_ourScreen;

        if (fg) {
            GetClassName(fg, cls, 64);
            GetWindowText(fg, title, 64);
            GetWindowThreadProcessId(fg, &pid);
            GetWindowRect(fg, &r);
            exeOf(pid, exe, 40);
            phone = fg != g_wnd && fg != g_icon && isPhoneApp(exe);
            cam = fg != g_wnd && fg != g_icon && isCameraApp(exe);
        }
        logd(L"   %u s foreground: 0x%08X %s class '%s' title '%s' pid 0x%08X at %d,%d %dx%d%s\r\n", GetTickCount() / 1000,
               (DWORD)fg, exe, cls, title, pid, r.left, r.top, r.right - r.left, r.bottom - r.top, fg == g_wnd ? L" (us)" : L"");
        g_fg = fg;
        g_fgSince = GetTickCount();
        g_fgCam = cam;
        if (fg == g_wnd)
            g_ourScreen = IsWindowVisible(g_wnd);
        else if (fg && bigWindow(fg))
            g_ourScreen = FALSE;        /* a popup over us leaves it as it is */
        if (fg == g_wnd && (wasScreen || g_phoneUp))
            planeAgain();              /* back in front of another screen */
        g_stFlags = (g_stFlags & ~2u) | (fg == g_wnd ? 0 : 2);
        /* Out of reverse, LG puts its current mode's screen back. When that is the phone menu, which can stay the
         * mode under Android Auto for a whole drive, it isn't a call: stay behind it only if the car's phone screen
         * was already in front when the camera came up. */
        if (phone && !g_phoneUp && !g_camOverPhone && (g_camUp || (g_camOver && GetTickCount() - g_camOver < 3000))) {
            logmsg(L"   %u s (%s after the camera: LG's own mode screen, not a call)\r\n", GetTickCount() / 1000, exe);
            if (bigWindow(fg))
                g_modeWnd = fg;
            phone = FALSE;
        }
        if (phone && bigWindow(fg)) {
            const WCHAR *why = fg == g_modeWnd ? L"seen before" : NULL;

            logChildren(fg);
            if (!g_phoneUp && g_modeAsked && GetTickCount() - g_modeAsked < 5000) {
                why = L"we asked for Bluetooth Audio";
                g_modeAsked = 0;
            } else if (!g_phoneUp && !fromUs && !why &&
                       !(g_camOverPhone && (g_camUp || (g_camOver && GetTickCount() - g_camOver < 3000)))) {
                why = L"it did not take the screen from Android Auto";
            }
            if (why) {
                g_modeWnd = fg;
                logmsg(L"   %u s (%s: LG's mode screen, not a call: %s)\r\n", GetTickCount() / 1000, exe, why);
                phone = FALSE;
            }
        }
        if (cam) {
            g_camWnd = fg;
            g_camSeen = GetTickCount();
            if (!g_camUp) {
                g_camUp = TRUE;
                g_camOverPhone = g_phoneUp;
                logmsg(L"   %u s the rear camera's screen is up (%s): leaving the screen alone\r\n", GetTickCount() / 1000, exe);
            }
        } else if (g_camUp && fg != g_wnd && bigWindow(fg)) {
            cameraOver(L"another screen took over");
        }
        if (phone) {
            g_phoneWnd = fg;
            if (!g_phoneUp) {
                g_phoneUp = TRUE;
                logmsg(L"   %u s the car's phone screen is up (%s): leaving the screen and the audio source alone\r\n",
                       GetTickCount() / 1000, exe);
            }
        } else if (g_phoneUp && (fg == g_wnd || bigWindow(fg))) {
            phoneScreenOver(fg == g_wnd ? L"Android Auto is in front" : L"another screen took over");
        }
        logPhoneWindows();
        cameraWindows();
    }
    /* Its window may also just close under a popup, or with nothing taking the foreground. */
    if (g_phoneUp && !(IsWindow(g_phoneWnd) && IsWindowVisible(g_phoneWnd))) {
        phoneScreenOver(L"its window closed");
        logPhoneWindows();
    }
    /* The camera is also over when its apps have shown no full-screen window for a second (a hidden window of
     * theirs in the foreground counts for 3 s: venus's second one is activated before it is shown), or when we
     * have had the foreground for 2 s (between venus's two windows it can fall to us for a moment). */
    if (g_camUp) {
        static DWORD checked;
        DWORD        now = GetTickCount();

        if (now - checked >= 100) {
            checked = now;
            if (cameraWindows() > 0 || (g_fgCam && now - g_fgSince < 3000))
                g_camSeen = now;
            else if (now - g_camSeen > 1000)
                cameraOver(L"its windows closed");
        }
        if (g_camUp && g_fg == g_wnd && now - g_fgSince > 2000)
            cameraOver(L"Android Auto is in front");
    }
}

/* While projecting: with "hold" (the default), takes the foreground back 300 ms after a full-screen LG
 * window took it, except while the camera or the car's phone screen is up. */
static void watchForeground(void)
{
    HWND fg;

    trackForeground();
    fg = g_fg;
    if (g_phoneUp || g_camUp)
        return;
    if (g_hold && fg != g_wnd && GetTickCount() - g_fgSince > 300) {
        g_fgSince = GetTickCount();
        /* Popups (volume, warnings: smaller than the screen) may sit on top of us; only a full-screen LG window
         * (another mode's screen) makes us take the screen back. */
        if (fg && !bigWindow(fg))
            return;
        if (!cameraUp()) {
            SetForegroundWindow(g_wnd);
            SetWindowPos(g_wnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        }
    } else if (fg == g_wnd) {
        cameraUp();                    /* keep logging its state changes */
    }
}

/* ---------- the graph ---------- */

static IPin *findPin(IBaseFilter *f, PIN_DIRECTION want)
{
    IEnumPins *en = NULL;
    IPin      *p = NULL, *found = NULL;
    ULONG      got;

    if (FAILED(f->EnumPins(&en)))
        return NULL;
    while (!found && en->Next(1, &p, &got) == S_OK && got == 1) {
        PIN_DIRECTION d;
        if (SUCCEEDED(p->QueryDirection(&d)) && d == want)
            found = p;
        else
            p->Release();
    }
    en->Release();
    return found;
}

/* The Pi sets header flag bit 1 while the phone is projecting (video focus on us), but a stale header doesn't count.
 * The Pi rewrites it every 2 s; one that still reads fine but has not changed for HDR_STALE_MS was left behind by a
 * receiver that stopped (crashed, or stopped by hand), and believing it would hold the screen with a frozen picture
 * for good. The age is measured between two of the feeder's own reads, so a feeder held up in a slow disk or decoder
 * call does not count as a silent Pi. */
static BOOL projecting(void)
{
    static BOOL said;

    if (g_hdr.magic != 0x31535641 || !(g_hdr.flags & 2))       /* a quit sets g_quit; see the feeder */
        return FALSE;
    if ((int)(g_hdrRead - g_hdrTick) >= HDR_STALE_MS) {
        if (!said)
            logw(L"   %u s the Pi's header says projecting but has not changed for %u s: its receiver has stopped; "
                 L"not projecting until it writes again\r\n", GetTickCount() / 1000, (g_hdrRead - g_hdrTick) / 1000);
        said = TRUE;
        return FALSE;
    }
    said = FALSE;
    return TRUE;
}

/* ---------- the floating icon ----------
 * A small always-on-top button over the LG screens while Android Auto isn't on screen. Its colour is the state:
 *   grey   the Pi isn't answering (stick gone, Pi off or still starting)
 *   blue   the Pi is there, no phone yet; a tap asks the Pi to connect to the phone now
 *   green  the phone is connected but not projecting (e.g. after AA's own exit button); a tap brings AA back
 *   amber  tapped, waiting (up to 6 s)
 *   red    the decoder has no memory to work with (see g_broken): no picture until the head unit restarts.
 *          A tap tries once more.
 *   purple Bluetooth pairing is open: hold the icon still for 3 s to open it (for 3 minutes), and again to
 *          close it. When a phone starts pairing, its 6-digit code shows on the icon: confirm on the phone only if
 *          the phone shows the same code. The Pi closes pairing 20 s after a phone pairs, or after the 3 minutes.
 * Drag it to move it (a finger that moves more than 30 px; less is still a tap or a hold); the position is kept in
 * aaview_icon.txt next to the log, and without one it sits in the middle of the top edge. Hidden while the camera is
 * up. */

#ifndef WS_EX_NOACTIVATE
#define WS_EX_NOACTIVATE 0x08000000L
#endif
#define ICON_SIZE 60

enum { IC_NOPI, IC_WAITING, IC_PHONE, IC_ASKED, IC_BROKEN, IC_PAIRING };
static const WCHAR *s_iconStates[] = { L"no Pi", L"waiting for the phone", L"phone connected", L"tapped",
                                       L"decoder memory missing", L"pairing open" };
#define HDR_PAIRING     16              /* Pi header flag: Bluetooth pairing is open */
#define ICON_HOLD_MS    3000            /* a hold this long, without dragging, opens or closes pairing */
#define ICON_HOLD_TIMER 1
#define ICON_DRAG_PX    30              /* a finger has to move this far (x + y) before a press is a drag: a resistive
                                         * screen's reading wanders under a finger held still */

/* The decoder said its memory pools are missing (g_memFail). That is what an aaview ended by force leaves behind,
 * and only a head unit restart brings them back. aaview stays running: it shows no picture (the sound still plays),
 * turns the icon red and builds the decoder again only when the icon is tapped. */
static BOOL  g_broken;

static BOOL  g_iconOn = TRUE;           /* "noicon" turns it off */
static int   g_iconState = -1;
static int   g_iconX = -1, g_iconY = -1;
static BOOL  g_iconDown, g_iconDrag;
static POINT g_iconGrab, g_iconPos0;
static DWORD g_iconAsked, g_iconTop;
static BOOL  g_iconHeld;                /* this press became a hold: no tap when it ends */
static DWORD g_pairAsked;               /* when a hold asked for pairing to open or close (shown at once) */
static BOOL  g_pairWant;                /* ... and which */
static DWORD g_iconKey;                 /* the pairing code the icon shows */
static DWORD g_iconDownAt;              /* when this press started (for the log) */
static int   g_iconWander;              /* the farthest (x + y) the finger got from where it pressed */
static WCHAR g_iconFile[MAX_PATH];

static int iconState(void)
{
    if (g_broken)
        return IC_BROKEN;
    if (g_ioErr || g_hdr.magic != 0x31535641 || GetTickCount() - g_hdrTick > 8000)
        return IC_NOPI;
    if (g_pairAsked && GetTickCount() - g_pairAsked < 6000 && !(g_hdr.flags & HDR_PAIRING) == g_pairWant)
        return g_pairWant ? IC_PAIRING : IC_ASKED;  /* asked: show it until the Pi's header agrees */
    if (g_hdr.flags & HDR_PAIRING)
        return IC_PAIRING;
    if (g_iconAsked && GetTickCount() - g_iconAsked < 6000)
        return IC_ASKED;
    return (g_hdr.flags & 4) ? IC_PHONE : IC_WAITING;
}

static void iconPaint(HDC dc)
{
    static const COLORREF fill[] = { RGB(0x55, 0x55, 0x55), RGB(0x2a, 0x5c, 0xa8), RGB(0x1e, 0x9a, 0x4c),
                                     RGB(0xd8, 0x92, 0x1a), RGB(0xb8, 0x2a, 0x2a), RGB(0x7a, 0x3c, 0xb0) };
    RECT    r = { 0, 0, ICON_SIZE, ICON_SIZE };
    POINT   arrow[4] = { { 30, 11 }, { 45, 47 }, { 30, 39 }, { 15, 47 } };    /* a plain navigation arrow */
    HBRUSH  b = CreateSolidBrush(fill[g_iconState < 0 ? 0 : g_iconState]);
    HPEN    pen = CreatePen(PS_SOLID, 2, RGB(0xe0, 0xe0, 0xe0));
    HGDIOBJ ob, op;

    FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    ob = SelectObject(dc, b);
    op = SelectObject(dc, pen);
    Ellipse(dc, 2, 2, ICON_SIZE - 2, ICON_SIZE - 2);
    if (g_iconState == IC_PAIRING) {
        /* "BT" while waiting for a phone, then its code in two rows of three digits */
        LOGFONT lf;
        HFONT   f, of;
        WCHAR   t[8];
        RECT    r1 = { 0, 9, ICON_SIZE, 31 }, r2 = { 0, 29, ICON_SIZE, 51 };

        memset(&lf, 0, sizeof(lf));
        lf.lfHeight = g_iconKey ? 20 : 24;
        lf.lfWeight = FW_BOLD;
        lstrcpyn(lf.lfFaceName, L"Tahoma", LF_FACESIZE);
        f = CreateFontIndirect(&lf);
        of = (HFONT)SelectObject(dc, f);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0xff, 0xff, 0xff));
        if (g_iconKey) {
            wsprintf(t, L"%03u", (g_iconKey / 1000) % 1000);
            DrawText(dc, t, -1, &r1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            wsprintf(t, L"%03u", g_iconKey % 1000);
            DrawText(dc, t, -1, &r2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            DrawText(dc, L"BT", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(dc, of);
        DeleteObject(f);
    } else {
        SelectObject(dc, GetStockObject(WHITE_BRUSH));
        SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, arrow, 4);
    }
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(pen);
}

static void iconPlace(int x, int y)
{
    int cx = GetSystemMetrics(SM_CXSCREEN), cy = GetSystemMetrics(SM_CYSCREEN);

    g_iconX = x < 0 ? 0 : x > cx - ICON_SIZE ? cx - ICON_SIZE : x;
    g_iconY = y < 0 ? 0 : y > cy - ICON_SIZE ? cy - ICON_SIZE : y;
    if (g_icon)
        SetWindowPos(g_icon, HWND_TOPMOST, g_iconX, g_iconY, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

static void iconLoad(void)
{
    HANDLE f = CreateFile(g_iconFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    char   b[32];
    DWORD  n = 0;

    if (f == INVALID_HANDLE_VALUE)
        return;
    if (ReadFile(f, b, sizeof(b) - 1, &n, NULL) && n) {
        char *sp;
        b[n] = 0;
        sp = strchr(b, ' ');
        if (sp) {
            g_iconX = atoi(b);
            g_iconY = atoi(sp + 1);
        }
    }
    CloseHandle(f);
}

static void iconSave(void)
{
    HANDLE f = CreateFile(g_iconFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    char   b[32];
    DWORD  n;

    if (f == INVALID_HANDLE_VALUE)
        return;
    sprintf(b, "%d %d\r\n", g_iconX, g_iconY);
    WriteFile(f, b, (DWORD)strlen(b), &n, NULL);
    CloseHandle(f);
    logd(L"   %u s icon moved to %d,%d\r\n", GetTickCount() / 1000, g_iconX, g_iconY);
}

static void iconTap(void)
{
    int s = iconState();

    logmsg(L"   %u s icon tapped (%s)\r\n", GetTickCount() / 1000, s_iconStates[s]);
    if (s == IC_BROKEN) {               /* the main loop builds the decoder once more, if the phone is projecting */
        g_memFail = FALSE;
        g_broken = FALSE;
        return;
    }
    if (g_phoneUp && projecting()) {    /* over the car's phone screen: back to Android Auto */
        phoneScreenOver(L"icon tapped");
        planeAgain();
        SetForegroundWindow(g_wnd);
        SetWindowPos(g_wnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        return;
    }
    if (s == IC_NOPI || s == IC_ASKED)
        return;                         /* nobody to tell / already asked */
    touchEvent(7, s, 0);               /* the Pi: connect to the phone (blue), or ask it to project (green) */
    g_iconAsked = GetTickCount();
}

static void iconUpdate(void);

/* The icon was held still for ICON_HOLD_MS: ask the Pi to open Bluetooth pairing, or to close it if it is open. */
static void iconHold(void)
{
    int  s = iconState();
    BOOL open = s != IC_PAIRING;

    if (s == IC_NOPI || s == IC_BROKEN)
        return;                         /* nobody to ask / the red icon's tap means something else */
    logmsg(L"   %u s icon held %u s: asking the Pi to %s Bluetooth pairing\r\n", GetTickCount() / 1000,
           ICON_HOLD_MS / 1000, open ? L"open" : L"close");
    touchEvent(9, open ? 1 : 0, 0);
    g_pairAsked = GetTickCount();
    g_pairWant = open;
    iconUpdate();
}

static LRESULT CALLBACK iconProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC         dc = BeginPaint(hwnd, &ps);
        iconPaint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        RECT r;
        ClientToScreen(hwnd, &pt);
        GetWindowRect(hwnd, &r);
        g_iconGrab = pt;
        g_iconPos0.x = r.left;
        g_iconPos0.y = r.top;
        g_iconDown = TRUE;
        g_iconDrag = FALSE;
        g_iconHeld = FALSE;
        g_iconDownAt = GetTickCount();
        g_iconWander = 0;
        SetCapture(hwnd);
        SetTimer(hwnd, ICON_HOLD_TIMER, ICON_HOLD_MS, NULL);
        return 0;
    }
    case WM_TIMER:
        if (wp == ICON_HOLD_TIMER) {
            KillTimer(hwnd, ICON_HOLD_TIMER);
            if (g_iconDown && !g_iconDrag) {
                g_iconHeld = TRUE;
                iconHold();
            }
        }
        return 0;
    case WM_MOUSEMOVE:
        if (g_iconDown) {
            ClientToScreen(hwnd, &pt);
            int d = abs(pt.x - g_iconGrab.x) + abs(pt.y - g_iconGrab.y);
            if (d > g_iconWander)
                g_iconWander = d;
            if (!g_iconDrag && !g_iconHeld && d > ICON_DRAG_PX) {
                g_iconDrag = TRUE;      /* a drag, not a tap (or a hold) */
                KillTimer(hwnd, ICON_HOLD_TIMER);
            }
            if (g_iconDrag)
                iconPlace(g_iconPos0.x + pt.x - g_iconGrab.x, g_iconPos0.y + pt.y - g_iconGrab.y);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_iconDown) {
            g_iconDown = FALSE;
            ReleaseCapture();
            KillTimer(hwnd, ICON_HOLD_TIMER);
            /* A press that lasted the hold time is a hold even when the timer's message has not come yet. CE sends
             * WM_TIMER only when nothing else is waiting, so a long press can end before it arrives. */
            if (!g_iconDrag && !g_iconHeld && GetTickCount() - g_iconDownAt >= ICON_HOLD_MS) {
                g_iconHeld = TRUE;
                iconHold();
            }
            logd(L"   %u s icon press: %u ms, finger wandered %d px: %s\r\n", GetTickCount() / 1000,
                 GetTickCount() - g_iconDownAt, g_iconWander, g_iconDrag ? L"a drag" : g_iconHeld ? L"a hold"
                 : L"a tap");
            if (g_iconDrag)
                iconSave();
            else if (!g_iconHeld)
                iconTap();
        }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static void iconCreate(HINSTANCE hInst)
{
    WNDCLASS wc;

    if (!g_iconOn)
        return;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = iconProc;
    wc.hInstance = hInst;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"aaview_icon";
    RegisterClass(&wc);
    if (g_iconX < 0 || g_iconY < 0) {   /* default: the middle of the top edge */
        g_iconX = (GetSystemMetrics(SM_CXSCREEN) - ICON_SIZE) / 2;
        g_iconY = 0;
    }
    g_icon = CreateWindowEx(WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"aaview_icon", L"aaview_icon", WS_POPUP, 0, 0,
                            ICON_SIZE, ICON_SIZE, NULL, NULL, hInst, NULL);
    iconPlace(g_iconX, g_iconY);
    logmsg(L"icon: %s at %d,%d\r\n", g_icon ? L"created" : L"CreateWindowEx failed", g_iconX, g_iconY);
}

/* From both loops: show the icon while the phone isn't projecting, or is but the car's phone screen is in front,
 * and the camera isn't up; keep it on top. */
static void iconUpdate(void)
{
    BOOL want;
    int  s;

    if (!g_icon)
        return;
    s = iconState();
    want = (!projecting() || g_phoneUp || g_broken || s == IC_PAIRING) && !cameraUp();
    if (s != g_iconState) {
        logmsg(L"   %u s icon: %s\r\n", GetTickCount() / 1000, s_iconStates[s]);
        g_iconState = s;
        InvalidateRect(g_icon, NULL, FALSE);
    }
    if (s == IC_PAIRING && g_pairKey != g_iconKey) {
        g_iconKey = g_pairKey;
        if (g_iconKey)
            logmsg(L"   %u s icon: a phone is pairing, code %06u\r\n", GetTickCount() / 1000, g_iconKey);
        InvalidateRect(g_icon, NULL, FALSE);
    }
    if (want != (IsWindowVisible(g_icon) ? TRUE : FALSE)) {
        ShowWindow(g_icon, want ? SW_SHOWNOACTIVATE : SW_HIDE);
        g_iconTop = 0;
        if (!want && g_iconDown) {
            g_iconDown = FALSE;
            ReleaseCapture();
        }
    }
    if (want && GetTickCount() - g_iconTop > 2000) {
        g_iconTop = GetTickCount();     /* LG's own windows may be topmost too: stay above them */
        SetWindowPos(g_icon, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

static void runLive(void)
{
    static const DWORD fourccs[] = { MAKEFOURCC('H', '2', '6', '4'), MAKEFOURCC('h', '2', '6', '4'),
                                     MAKEFOURCC('X', '2', '6', '4'), MAKEFOURCC('A', 'V', 'C', '1') };
    IGraphBuilder *gb = NULL;
    IBaseFilter   *dec = NULL, *rend = NULL;
    IPin          *decIn = NULL, *decOut = NULL, *rendIn = NULL;
    IMediaControl *mc = NULL;
    IMediaEvent   *me = NULL;
    IBaseFilter   *mf = NULL;     /* IMediaFilter part only */
    IVideoWindow  *vw = NULL;
    IVrend        *vrend = NULL;
    IQualProp     *qp = NULL;
    HRESULT        hr;
    DWORD          lastLog = 0;
    int            i, nev = 0, lastDrawn = -1;

    logmsg(L"\r\n== live: stream LUN -> Alchemy Video Decoder -> Alchemy Video Renderer\r\n");
    queryRegions();
    hookDecoderLog();
    hookDecoderIoctl();
    hr = CoCreateInstance(CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER, IID_IGraphBuilder, (void **)&gb);
    if (FAILED(hr)) {
        logh(hr, L"   CoCreateInstance(FilterGraph): 0x%08X\r\n", hr);
        return;
    }
    hr = CoCreateInstance(CLSID_AlchemyVideoDecoder, NULL, CLSCTX_INPROC_SERVER, IID_IBaseFilter, (void **)&dec);
    logh(hr, L"   create Alchemy Video Decoder: 0x%08X\r\n", hr);
    if (FAILED(hr))
        goto out;
    logd(L"   AddFilter source 0x%08X, decoder 0x%08X\r\n",
           gb->AddFilter(&g_source, L"AAView Source"), gb->AddFilter(dec, L"Alchemy Video Decoder"));
    decIn = findPin(dec, PINDIR_INPUT);
    decOut = findPin(dec, PINDIR_OUTPUT);
    if (!decIn || !decOut)
        goto out;
    for (i = 0; i < 4; i++) {
        makeFeedType(fourccs[i]);
        hr = gb->ConnectDirect(&g_feedPin, decIn, &g_feedType);
        if (SUCCEEDED(hr))
            break;
    }
    logh(hr, L"   connect source -> decoder: 0x%08X (type %d)\r\n", hr, i);
    if (FAILED(hr))
        goto out;

    hr = CoCreateInstance(CLSID_AlchemyVideoRenderer, NULL, CLSCTX_INPROC_SERVER, IID_IBaseFilter, (void **)&rend);
    logh(hr, L"   create Alchemy Video Renderer: 0x%08X\r\n", hr);
    if (SUCCEEDED(hr)) {
        logd(L"   AddFilter renderer 0x%08X\r\n", gb->AddFilter(rend, L"Alchemy Video Renderer"));
        rendIn = findPin(rend, PINDIR_INPUT);
        hr = rendIn ? gb->ConnectDirect(decOut, rendIn, NULL) : E_FAIL;
        logh(hr, L"   connect decoder -> renderer: 0x%08X\r\n", hr);
    }
    if (FAILED(hr))
        goto out;
    if (SUCCEEDED(gb->QueryInterface(IID_IVideoWindow, (void **)&vw)))
        logd(L"   IVideoWindow: owner 0x%08X, style 0x%08X, pos 0x%08X, drain 0x%08X\r\n",
               vw->put_Owner((LONG_PTR)g_wnd), vw->put_WindowStyle(WS_CHILD),
               vw->SetWindowPosition(0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)),
               vw->put_MessageDrain((LONG_PTR)g_wnd));
    if (SUCCEEDED(rend->QueryInterface(IID_IVrend, (void **)&vrend)))
        logd(L"   IVrend: found\r\n");
    if (SUCCEEDED(rend->QueryInterface(IID_IQualProp, (void **)&qp)))
        logd(L"   IQualProp: found\r\n");
    if (g_memFail) {
        logw(L"   decoder memory pools are missing; restart the head unit and run again\r\n");
        goto out;
    }
    if (FAILED(gb->QueryInterface(IID_IMediaControl, (void **)&mc)) || FAILED(gb->QueryInterface(IID_IMediaEvent, (void **)&me)))
        goto out;
    if (!g_keepClock && SUCCEEDED(gb->QueryInterface(IID_IMediaFilter, (void **)&mf))) {
        logd(L"   SetSyncSource(NULL) (render on arrival): 0x%08X\r\n", mf->SetSyncSource(NULL));
        mf->Release();
    }
    g_runTick = GetTickCount();
    hr = mc->Run();
    logh(hr, L"   Run: 0x%08X\r\n", hr);
    logd(L"   overlay %d enable: %d\r\n", MAE_PLANE, ovlEscape(OVL_ENABLE, MAE_PLANE));
    if (vrend)
        logd(L"   IVrend video on: 0x%08X\r\n", vrend->VideoOnOff(1));
    g_ovlAgain = 0;
    trackForeground();
    if (g_camUp || g_phoneUp) {         /* projection (re)starting in reverse or during a call: come up behind */
        HWND front = g_phoneWnd;

        if (g_camUp)
            front = cameraWindows() ? g_camTop : g_camWnd;

        logmsg(L"   the %s screen is up: showing behind it\r\n", g_camUp ? L"rear camera's" : L"car's phone");
        SetWindowPos(g_wnd, front, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (GetForegroundWindow() != front)
            SetForegroundWindow(front);
    } else {
        ShowWindow(g_wnd, SW_SHOW);
        SetForegroundWindow(g_wnd);
    }
    g_stFlags |= 1;                     /* status: showing */
    while (!g_quit && projecting()) {
        long     code;
        LONG_PTR p1, p2;

        pump();
        watchForeground();
        if (g_ovlAgain && !g_camUp && (int)(GetTickCount() - g_ovlAgain) >= 0) {
            static const DWORD s_wait[] = { 700, 2000 };

            logd(L"   %u s overlay %d enable again: %d\r\n", GetTickCount() / 1000, MAE_PLANE, ovlEscape(OVL_ENABLE, MAE_PLANE));
            g_ovlAgain = g_ovlTries < 2 ? (GetTickCount() + s_wait[g_ovlTries++]) | 1 : 0;
        }
        iconUpdate();                  /* shown over the car's phone screen */
        watchMediaStarts();
        while (me->GetEvent(&code, &p1, &p2, 0) == S_OK) {
            if (nev++ < 60)
                logd(L"   %5u ms event 0x%02X (0x%08X, 0x%08X)\r\n", GetTickCount() - g_runTick, code, (DWORD)p1, (DWORD)p2);
            me->FreeEventParams(code, p1, p2);
        }
        if (qp) {
            int drawn = 0;
            qp->get_FramesDrawn(&drawn);
            if (drawn != lastDrawn) {
                if (lastDrawn <= 0)
                    InvalidateRect(g_wnd, NULL, TRUE);   /* drop the "waiting" text */
                lastDrawn = drawn;
                g_decoded = drawn;
            }
        }
        if (g_memFail) {
            logw(L"   decoder memory pools are missing; stopping\r\n");
            break;
        }
        if (GetTickCount() - lastLog > 30000 && qp) {
            int dropped = -1, fps = -1;
            qp->get_FramesDroppedInRenderer(&dropped);
            qp->get_AvgFrameRate(&fps);
            logd(L"   %u s renderer: drawn %d, dropped %d, avg fps x100 %d\r\n", (GetTickCount() - g_runTick) / 1000,
                   lastDrawn, dropped, fps);
            lastLog = GetTickCount();
        }
        Sleep(15);
    }
    logmsg(L"   stopping after %u s: fed %d (%d failed), decoded/drawn %d\r\n", (GetTickCount() - g_runTick) / 1000, g_fed,
           g_feedFails, g_decoded);
    hr = mc->Stop();
    logh(hr, L"   Stop: 0x%08X\r\n", hr);
    if (vrend)
        logd(L"   IVrend video off: 0x%08X\r\n", vrend->VideoOnOff(0));
    logd(L"   overlay %d disable: %d / %d\r\n", MAE_PLANE, ovlEscape(OVL_PREDISABLE, MAE_PLANE),
           ovlEscape(OVL_DISABLE, MAE_PLANE));

out:
    ShowWindow(g_wnd, SW_HIDE);
    g_ourScreen = FALSE;
    g_stFlags &= ~1u;
    if (vw) {
        vw->put_Visible(0);
        vw->put_Owner(0);
        vw->Release();
    }
    if (qp) qp->Release();
    if (vrend) vrend->Release();
    if (me) me->Release();
    if (mc) mc->Release();
    if (rendIn) rendIn->Release();
    if (decIn) decIn->Release();
    if (decOut) decOut->Release();
    if (rend) rend->Release();
    if (dec) dec->Release();
    if (gb) gb->Release();
    g_feedPin.Disconnect();
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmdLine, int show)
{
    WCHAR      exeDir[MAX_PATH], args[MAX_PATH] = L"", *p, *tok;
    WNDCLASS   wc;
    SYSTEMTIME st;
    HANDLE     single;
    HRESULT    hr;
    int        tries;

    (void)hPrev;
    (void)show;
    InitializeCriticalSection(&g_logLock);
    InitializeCriticalSection(&g_ioLock);
    InitializeCriticalSection(&g_stLock);
    GetModuleFileName(NULL, exeDir, MAX_PATH);
    /* Run from a copy in \Temp. CE pages code in from the exe file, so a stick that goes away (Pi power, re-plugs)
     * would otherwise kill us mid-projection and leak the decoder's memory until the head unit restarts. The log
     * stays in the stick folder ("logdir=" argument). */
    if (_wcsicmp(exeDir, TEMP_AAVIEW)) {
        PROCESS_INFORMATION pi;
        WCHAR               cl[MAX_PATH * 2];
        DWORD               t0 = GetTickCount();
        BOOL                ok;

        single = CreateMutex(NULL, FALSE, L"aaview_single");
        if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(single);
            return 1;                   /* already running (from \Temp) */
        }
        if (single)
            CloseHandle(single);
        CreateDirectory(L"\\Temp", NULL);
        while (!(ok = CopyFile(exeDir, TEMP_AAVIEW, FALSE)) && GetTickCount() - t0 < 15000)
            Sleep(500);                 /* an old copy may still be exiting */
        p = wcsrchr(exeDir, L'\\');
        if (p) *p = 0;
        wsprintf(cl, L"logdir=\"%s\" %s", exeDir, cmdLine ? cmdLine : L"");
        if (!ok || !CreateProcess(TEMP_AAVIEW, cl, NULL, NULL, FALSE, 0, NULL, NULL, NULL, &pi))
            return 1;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 0;
    }
    p = wcsrchr(exeDir, L'\\');
    if (p) *p = 0;
    if (cmdLine && !_wcsnicmp(cmdLine, L"logdir=", 7)) {
        /* the folder may be quoted, since it can have spaces */
        WCHAR *s = cmdLine + 7, *e;
        BOOL   quoted = *s == L'"';

        if (quoted)
            s++;
        e = wcschr(s, quoted ? L'"' : L' ');
        lstrcpyn(exeDir, s, e && e - s < MAX_PATH ? (int)(e - s) + 1 : MAX_PATH);
        if (e && quoted)
            e = wcschr(e, L' ');
        cmdLine = e ? e + 1 : NULL;
    }
    wsprintf(g_logPath, L"%s\\aaview_log.txt", exeDir);
    wsprintf(g_logOld, L"%s\\aaview_log.old.txt", exeDir);
    wsprintf(g_iconFile, L"%s\\aaview_icon.txt", exeDir);
    wsprintf(g_clockFile, L"%s\\aaview_clock.txt", exeDir);
    iconLoad();
    clockLoad();
    logOpen();
    GetLocalTime(&st);
    logw(L"\r\n===== aaview %s  %04u-%02u-%02u %02u:%02u:%02u  [%s]\r\n", AAVIEW_VERSION, st.wYear, st.wMonth, st.wDay,
           st.wHour, st.wMinute, st.wSecond, cmdLine ? cmdLine : L"");

    single = CreateMutex(NULL, TRUE, L"aaview_single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        logw(L"aaview is already running; exiting\r\n");
        return 1;
    }

    g_hold = TRUE;                      /* keep the screen while projecting (not while the camera is up) */

    if (cmdLine)
        lstrcpyn(args, cmdLine, MAX_PATH);
    for (tok = wcstok(args, L" "); tok; tok = wcstok(NULL, L" ")) {
        if (!_wcsicmp(tok, L"/auto")) {
            /* what the launchers pass; nothing to do */
        } else if (!_wcsicmp(tok, L"hold"))
            g_hold = TRUE;
        else if (!_wcsicmp(tok, L"nohold"))
            g_hold = FALSE;
        else if (!_wcsicmp(tok, L"noicon"))
            g_iconOn = FALSE;
        else if (!_wcsnicmp(tok, L"icon=", 5) && wcschr(tok, L','))
            g_iconX = _wtoi(tok + 5), g_iconY = _wtoi(wcschr(tok, L',') + 1);
        else if (!_wcsicmp(tok, L"norestore"))
            g_restore = FALSE;
        else if (!_wcsicmp(tok, L"nograb"))
            g_grabAudio = FALSE;
        else if (!_wcsnicmp(tok, L"abuf", 4) && _wtoi(tok + 4) >= 100 && _wtoi(tok + 4) <= 1500)
            g_mediaBuf = (DWORD)_wtoi(tok + 4);
        else if (!_wcsicmp(tok, L"noarate"))
            g_rateFix = FALSE;
        else if (!_wcsnicmp(tok, L"arate", 5) && _wtoi(tok + 5) >= RATE_INT_MIN && _wtoi(tok + 5) <= RATE_INT_MAX)
            g_ratePpm = g_rateInt = _wtoi(tok + 5);
        else if (!_wcsicmp(tok, L"nomic"))
            g_micDev = -1;
        else if (!_wcsnicmp(tok, L"mic", 3) && tok[3] >= L'0' && tok[3] <= L'9')
            g_micDev = _wtoi(tok + 3);
        else if (!_wcsicmp(tok, L"clock"))
            g_keepClock = TRUE;
        else if (!_wcsnicmp(tok, L"ts", 2))
            g_tsMode = tok[2] == L'0' && !tok[3] ? -1 : (int)wcstoul(tok + 2, NULL, 10);
        else if (!_wcsnicmp(tok, L"yuvcam", 6))
            g_capYuv = wcstoul(tok + 6, NULL, 10) * 1024 * 1024;
        else
            logw(L"unknown option '%s'\r\n", tok);
    }
    logmsg(L"options: %s%s timestamps %s%d ms\r\n", g_hold ? L"hold " : L"", g_keepClock ? L"clock " : L"no-clock",
           g_tsMode < 0 ? L"off " : L"arrival + ", g_tsMode < 0 ? 0 : g_tsMode);

    InitializeCriticalSection(&g_vidLock);
    g_stage = (BYTE *)malloc(STAGE_BYTES);
    if (!g_stage)
        return 1;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"aaview";
    RegisterClass(&wc);
    /* Hidden until the phone projects; the LG screens stay usable meanwhile. */
    g_wnd = CreateWindow(L"aaview", L"aaview", WS_POPUP, 0, 0, GetSystemMetrics(SM_CXSCREEN),
                         GetSystemMetrics(SM_CYSCREEN), NULL, NULL, hInst, NULL);
    iconCreate(hInst);

    for (tries = 0; !streamOpen(); tries++) {
        DWORD t0 = GetTickCount();
        if (tries == 0)
            logmsg(L"no %u-sector stream LUN yet; waiting for the Pi\r\n", STREAM_SECTORS);
        while (GetTickCount() - t0 < 2000) {   /* the Pi may be off, or still bringing the USB gadget up */
            pump();
            iconUpdate();
            Sleep(50);
        }
    }
    g_mySession = GetTickCount() | 1;
    if (g_micDev >= 0)
        g_stFlags |= ST_CAN_RECORD;
    writeStatus();
    g_feedThread = CreateThread(NULL, 0, feedThread, NULL, 0, NULL);   /* the ring reader */
    if (g_micDev >= 0)
        g_micThread = CreateThread(NULL, 0, micThread, NULL, 0, NULL);

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    logh(hr, L"CoInitializeEx: 0x%08X\r\n", hr);
    while (!g_quit) {
        pump();
        trackForeground();
        iconUpdate();              /* hides the icon once projection starts */
        watchMediaStarts();
        if (projecting() && !g_broken) {
            logmsg(L"\r\n%u s: the phone is projecting\r\n", GetTickCount() / 1000);
            if (g_lightCmd) {
                logmsg(L"   lights: telling the Pi again (phoenix command 0x%02X)\r\n", g_lightCmd);
                touchEvent(8, (int)g_lightCmd, (int)(g_lightData & 0xffff));
            } else {
                logmsg(L"   lights: no message from the head unit yet\r\n");
            }
            g_decoded = 0;
            runLive();
            logmsg(L"%u s: projection ended\r\n", GetTickCount() / 1000);
            if (g_memFail) {
                g_broken = TRUE;
                logw(L"%u s: the decoder's memory is missing (an aaview that was ended by force leaves that "
                     L"behind): no picture until the head unit restarts. Still running; a tap on the red icon "
                     L"tries again\r\n", GetTickCount() / 1000);
            }
        }
        Sleep(50);
    }
    CoUninitialize();
    if (g_micThread && WaitForSingleObject(g_micThread, 2000) != WAIT_OBJECT_0)
        logw(L"   the microphone thread did not stop in 2 s\r\n");
    if (g_feedThread && WaitForSingleObject(g_feedThread, 3000) != WAIT_OBJECT_0)
        logw(L"   the feeder did not stop in 3 s\r\n");
    g_stFlags |= 0x80000000u;           /* tell the Pi we're gone */
    writeStatus();
    logmsg(L"\r\nDONE (the Pi's quit flag)\r\n");
    logFlush(TRUE);
    if (g_icon)
        DestroyWindow(g_icon);
    DestroyWindow(g_wnd);
    /* under the I/O lock, so a thread that did not stop in time fails its next call on INVALID_HANDLE_VALUE
     * rather than using a closed (or reused) handle */
    EnterCriticalSection(&g_ioLock);
    CloseHandle(g_stream);
    g_stream = INVALID_HANDLE_VALUE;
    LeaveCriticalSection(&g_ioLock);
    if (g_log != INVALID_HANDLE_VALUE)
        CloseHandle(g_log);
    CloseHandle(single);
    return 0;
}
