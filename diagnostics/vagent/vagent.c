/*
 * vagent - test agent for the Veloster head unit (Windows CE 6.0, MIPS).
 *
 * Start it once from Explorer at <stick>\vagent\vagent.exe. It copies itself to
 * \Temp (RAM) and keeps running from there, so the stick can be removed and
 * reinserted. Whenever the stick is present it runs the job files in
 * <stick>\vagent\jobs\*.job in name order, writes <stick>\vagent\results\<job>.txt,
 * and renames each finished job to <job>.job.done.
 *
 * Job file commands, one per line (# starts a comment):
 *   log <text>            write a line to the results file
 *   timeout <seconds>     time limit for the following run commands (default 300)
 *   run <exe> [args]      start a program and wait for it; killed at the time limit
 *   start <exe> [args]    start a program and don't wait
 *   kill [name]           terminate the last program this job started, or every process named <name>, for
 *                         example "kill cmd.exe". CE core processes, kronos and vagent itself are refused.
 *   ipc <title> <wParam hex> [lParam hex]   post one LG IPC message (0x8096) to the window with that title
 *   sleep <ms>            wait
 *   screenshot [label]    save the desktop as <job>_<label>.bmp in results
 *                         (the video overlay planes are not part of the desktop)
 *   procs                 list running processes
 *   collect <file>        copy a file into results as <job>_<file name>
 *   copy <src> <dst>      copy a file (quote paths with spaces); dst must be on the stick or under \Temp
 *   dir [path]            list a folder or pattern (names, sizes, dates, d/r/h attributes)
 *   type <file>           copy a text file (first 32 KB) into the results
 *   del | mkdir | rmdir <path>   file operations, only on the stick or under \Temp
 *   reg <HKLM\key> [value]  read-only: list a registry key's subkeys and values, or one value
 *   mem                   RAM and free space (stick, \Temp, Storage Card 1-3)
 *   waitreinsert <sec>    wait until the stick is removed and inserted again
 *   quit                  stop vagent after this job
 * Relative paths are relative to the stick root (the folder that holds vagent\).
 *
 * vagent never writes to \Storage Card or the registry. Tap its status window to quit.
 */
#include <windows.h>
#include <tlhelp32.h>
#include <storemgr.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* coredll has no lstrcpyn; bounded copy that always terminates. */
#define lstrcpyn(d, s, n)  (wcsncpy((d), (s), (n) - 1), (d)[(n) - 1] = 0, (d))

#define VAGENT_VERSION   L"0.6"
#define TEMP_DIR         L"\\Temp"
#define TEMP_EXE         L"\\Temp\\vagent.exe"
#define TEMP_LOG         L"\\Temp\\vagent_log.txt"
#define MAX_JOBS         64
#define MAX_LINE         512
#define POLL_MS          1000
#define HEARTBEAT_MS     30000
#define WM_STATUS        (WM_USER + 1)

#ifndef TH32CS_SNAPNOHEAPS
#define TH32CS_SNAPNOHEAPS  0x40000000
#endif

typedef HANDLE (WINAPI *PFN_SNAPSHOT)(DWORD, DWORD);
typedef BOOL   (WINAPI *PFN_PROCWALK)(HANDLE, LPPROCESSENTRY32);
typedef BOOL   (WINAPI *PFN_CLOSESNAP)(HANDLE);

static WCHAR  g_agentDir[MAX_PATH];     /* <stick>\vagent */
static WCHAR  g_stickRoot[MAX_PATH];    /* <stick> */
static WCHAR  g_resFile[MAX_PATH];      /* current job's results file, or "" */
static WCHAR  g_resPrefix[MAX_PATH];    /* <stick>\vagent\results\<job> */
static WCHAR  g_status[128] = L"vagent: starting";
static HWND   g_wnd;
static volatile BOOL g_quit;
static DWORD  g_heartbeat;

/* ---------- small helpers ---------- */

/* wsprintf that never writes past n characters. A string that does not fit is cut. Returns the characters
 * written, never more than n - 1. */
static int fmtn(WCHAR *buf, int n, const WCHAR *f, ...)
{
    va_list ap;
    int     r;

    if (n <= 0)
        return 0;
    va_start(ap, f);
    r = _vsnwprintf(buf, n - 1, f, ap);
    va_end(ap);
    buf[n - 1] = 0;
    return r < 0 || r > n - 1 ? (int)wcslen(buf) : r;
}

static BOOL dirExists(const WCHAR *p)
{
    DWORD a = GetFileAttributes(p);
    return a != 0xFFFFFFFF && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL stickPresent(void)
{
    WCHAR p[MAX_PATH];
    fmtn(p, MAX_PATH, L"%s\\jobs", g_agentDir);
    return dirExists(p);
}

/* Appends UTF-8 text to a file, opening and closing it each time so no handle is
 * held on the stick between writes (it may be ejected at any moment). */
static void appendFile(const WCHAR *path, const WCHAR *text)
{
    char   a[3 * MAX_LINE + 8];
    int    n = WideCharToMultiByte(CP_UTF8, 0, text, -1, a, sizeof(a), NULL, NULL);
    DWORD  w;
    HANDLE h;

    if (n <= 1)
        return;
    h = CreateFile(path, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    SetFilePointer(h, 0, NULL, FILE_END);
    WriteFile(h, a, n - 1, &w, NULL);
    FlushFileBuffers(h);
    CloseHandle(h);
}

static void writeFile(const WCHAR *path, const WCHAR *text)
{
    DeleteFile(path);
    appendFile(path, text);
}

static void stamp(WCHAR *buf)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    fmtn(buf, 32, L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

/* Agent log: \Temp always, and the stick's copy when it is present. */
static void alog(const WCHAR *fmt, ...)
{
    WCHAR   line[MAX_LINE], ts[32], p[MAX_PATH];
    va_list ap;
    int     n;

    stamp(ts);
    n = fmtn(line, MAX_LINE, L"%s  ", ts);
    va_start(ap, fmt);
    _vsnwprintf(line + n, MAX_LINE - n - 3, fmt, ap);
    va_end(ap);
    line[MAX_LINE - 3] = 0;
    wcscat(line, L"\r\n");
    appendFile(TEMP_LOG, line);
    if (stickPresent()) {
        fmtn(p, MAX_PATH, L"%s\\vagent_log.txt", g_agentDir);
        appendFile(p, line);
    }
}

/* Results line for the current job (dropped while the stick is out). */
static void res(const WCHAR *fmt, ...)
{
    WCHAR   line[MAX_LINE];
    va_list ap;

    if (!g_resFile[0] || !stickPresent())
        return;
    va_start(ap, fmt);
    _vsnwprintf(line, MAX_LINE - 3, fmt, ap);
    va_end(ap);
    line[MAX_LINE - 3] = 0;
    wcscat(line, L"\r\n");
    appendFile(g_resFile, line);
}

static void setStatus(const WCHAR *fmt, ...)
{
    WCHAR   s[128], ts[32], p[MAX_PATH], body[256];
    va_list ap;

    va_start(ap, fmt);
    _vsnwprintf(s, 127, fmt, ap);
    va_end(ap);
    s[127] = 0;
    fmtn(g_status, 128, L"vagent: %s", s);
    if (g_wnd)
        PostMessage(g_wnd, WM_STATUS, 0, 0);
    if (stickPresent()) {
        stamp(ts);
        fmtn(body, 256, L"%s\r\nupdated %s\r\nheartbeat %u\r\n", s, ts, g_heartbeat);
        fmtn(p, MAX_PATH, L"%s\\status.txt", g_agentDir);
        writeFile(p, body);
    }
}

/* Resolves a job path: absolute paths stay, relative ones are under the stick root. */
static void resolve(const WCHAR *in, WCHAR *out)
{
    if (in[0] == L'\\')
        lstrcpyn(out, in, MAX_PATH);
    else
        fmtn(out, MAX_PATH, L"%s\\%s", g_stickRoot, in);
}

static const WCHAR *baseName(const WCHAR *p)
{
    const WCHAR *s = wcsrchr(p, L'\\');
    return s ? s + 1 : p;
}

/* ---------- job commands ---------- */

static HANDLE g_lastProc;

static void closeLast(void)
{
    if (g_lastProc) {
        CloseHandle(g_lastProc);
        g_lastProc = NULL;
    }
}

/* Splits "exe args" (exe may be quoted) and starts it. */
static BOOL startProgram(const WCHAR *spec, PROCESS_INFORMATION *pi)
{
    WCHAR exe[MAX_PATH], path[MAX_PATH];
    const WCHAR *args;
    int   i = 0;

    if (*spec == L'"') {
        spec++;
        while (*spec && *spec != L'"' && i < MAX_PATH - 1)
            exe[i++] = *spec++;
        if (*spec == L'"')
            spec++;
    } else {
        while (*spec && *spec != L' ' && i < MAX_PATH - 1)
            exe[i++] = *spec++;
    }
    exe[i] = 0;
    while (*spec == L' ')
        spec++;
    args = spec;
    resolve(exe, path);
    memset(pi, 0, sizeof(*pi));
    if (!CreateProcess(path, *args ? args : NULL, NULL, NULL, FALSE, 0, NULL, NULL, NULL, pi)) {
        res(L"   CreateProcess(%s) failed: %u", path, GetLastError());
        return FALSE;
    }
    res(L"   started %s %s (pid %08X)", path, args, pi->dwProcessId);
    return TRUE;
}

/*
 * ipc <window title> <wParam hex> <lParam hex>: posts one LG IPC message (0x8096) the way IPC.dll does.
 * wParam = cmd<<24 | fnc<<16 | src<<8 | len; lParam holds up to 4 data bytes. For example
 * "ipc phoenix 00020602 00000103" is RELEASE_SOUND (fnc 2) from kronos (6) with 2 data bytes: source 3, extra 1.
 */
static BOOL nextArg(const WCHAR **p, WCHAR *out);

static void cmdIpc(const WCHAR *arg)
{
    WCHAR title[MAX_PATH], ws[MAX_PATH], ls[MAX_PATH] = L"0";   /* nextArg writes up to MAX_PATH */
    DWORD wp, lp;
    HWND  w;

    if (!nextArg(&arg, title) || !nextArg(&arg, ws)) {
        res(L"   usage: ipc <window title> <wParam hex> [lParam hex]");
        return;
    }
    nextArg(&arg, ls);
    wp = wcstoul(ws, NULL, 16);
    lp = wcstoul(ls, NULL, 16);
    w = FindWindow(NULL, title);
    if (!w) {
        res(L"   no window titled '%s'", title);
        return;
    }
    res(L"   PostMessage(0x%08X '%s', 0x8096, 0x%08X, 0x%08X): %d", (DWORD)w, title, wp, lp,
        PostMessage(w, 0x8096, (WPARAM)wp, (LPARAM)lp));
}

/*
 * <agent folder>\autostart.txt lists programs to start once vagent is up, one "exe [args]" per line. Blank lines
 * and lines starting with # are skipped. Programs that keep a single instance, like aaview, exit if they are
 * already running, so a vagent hand-over does not double them.
 */
static void autostart(void)
{
    WCHAR  path[MAX_PATH], line[MAX_PATH];
    char   buf[2048];
    DWORD  got = 0, i, n = 0;
    HANDLE f;

    fmtn(path, MAX_PATH, L"%s\\autostart.txt", g_agentDir);
    f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    ReadFile(f, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(f);
    buf[got] = 0;
    for (i = 0; i <= got; i++) {
        if (buf[i] == '\r' || buf[i] == '\n' || buf[i] == 0) {
            line[n] = 0;
            while (n && line[n - 1] == L' ')
                line[--n] = 0;
            if (n && line[0] != L'#') {
                PROCESS_INFORMATION pi;
                if (startProgram(line, &pi)) {
                    alog(L"autostart: %s (pid %08X)", line, pi.dwProcessId);
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                } else {
                    alog(L"autostart: %s failed: %u", line, GetLastError());
                }
            }
            n = 0;
        } else if (n < MAX_PATH - 1) {
            line[n++] = (WCHAR)(BYTE)buf[i];
        }
    }
}

static void cmdRun(const WCHAR *spec, DWORD timeout_s, BOOL wait)
{
    PROCESS_INFORMATION pi;
    DWORD t0 = GetTickCount(), rc, code = 0;

    closeLast();
    if (!startProgram(spec, &pi))
        return;
    CloseHandle(pi.hThread);
    if (!wait) {
        g_lastProc = pi.hProcess;
        return;
    }
    rc = WaitForSingleObject(pi.hProcess, timeout_s * 1000);
    if (rc == WAIT_TIMEOUT) {
        res(L"   time limit %u s reached; terminating", timeout_s);
        TerminateProcess(pi.hProcess, 0xDEAD);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    GetExitCodeProcess(pi.hProcess, &code);
    res(L"   exited with code %u (0x%08X) after %u ms", code, code, GetTickCount() - t0);
    CloseHandle(pi.hProcess);
}

static void cmdScreenshot(const WCHAR *label)
{
    int        w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    int        stride = (w * 3 + 3) & ~3;
    HDC        sdc = GetDC(NULL), mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi;
    void      *bits = NULL;
    HBITMAP    bmp;
    WCHAR      path[MAX_PATH];

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;              /* bottom-up, the order a .bmp file stores */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) {
        res(L"   screenshot: CreateDIBSection failed: %u", GetLastError());
    } else {
        HGDIOBJ old = SelectObject(mdc, bmp);
        BITMAPFILEHEADER fh;
        HANDLE f;
        DWORD  wr;

        BitBlt(mdc, 0, 0, w, h, sdc, 0, 0, SRCCOPY);
        SelectObject(mdc, old);
        fmtn(path, MAX_PATH, L"%s_%s.bmp", g_resPrefix, *label ? label : L"screen");
        memset(&fh, 0, sizeof(fh));
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
        fh.bfSize = fh.bfOffBits + stride * h;
        f = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) {
            res(L"   screenshot: cannot create %s: %u", path, GetLastError());
        } else {
            WriteFile(f, &fh, sizeof(fh), &wr, NULL);
            WriteFile(f, &bi.bmiHeader, sizeof(BITMAPINFOHEADER), &wr, NULL);
            WriteFile(f, bits, stride * h, &wr, NULL);
            FlushFileBuffers(f);
            CloseHandle(f);
            res(L"   screenshot %d x %d -> %s", w, h, path);
        }
        DeleteObject(bmp);
    }
    DeleteDC(mdc);
    ReleaseDC(NULL, sdc);
}

/* CE's own core processes and LG's kronos.exe, the head unit's system manager that starts the other LG programs
 * and the USB-stick upgrade. Killing any of these takes the whole head unit down. */
static const WCHAR *s_protected[] = { L"nk.exe", L"udevice.exe", L"servicesd.exe", L"device.exe", L"filesys.exe",
                                      L"gwes.exe", L"services.exe", L"kronos.exe", L"vagent.exe", NULL };

/* Calls fn for every running process, through toolhelp.dll. FALSE (and a results line) if there is no toolhelp. */
static BOOL forEachProcess(void (*fn)(const PROCESSENTRY32 *pe, void *ctx), void *ctx)
{
    HMODULE        th = LoadLibrary(L"toolhelp.dll");
    PFN_SNAPSHOT   pSnap;
    PFN_PROCWALK   pFirst, pNext;
    PFN_CLOSESNAP  pClose;
    HANDLE         s;
    PROCESSENTRY32 pe;

    if (!th) {
        res(L"   toolhelp.dll not available: %u", GetLastError());
        return FALSE;
    }
    pSnap = (PFN_SNAPSHOT)GetProcAddress(th, L"CreateToolhelp32Snapshot");
    pFirst = (PFN_PROCWALK)GetProcAddress(th, L"Process32First");
    pNext = (PFN_PROCWALK)GetProcAddress(th, L"Process32Next");
    pClose = (PFN_CLOSESNAP)GetProcAddress(th, L"CloseToolhelp32Snapshot");
    if (pSnap && pFirst && pNext && pClose) {
        s = pSnap(TH32CS_SNAPPROCESS | TH32CS_SNAPNOHEAPS, 0);
        if (s != INVALID_HANDLE_VALUE) {
            pe.dwSize = sizeof(pe);
            if (pFirst(s, &pe))
                do
                    fn(&pe, ctx);
                while (pNext(s, &pe));
            pClose(s);
        }
    }
    FreeLibrary(th);
    return TRUE;
}

typedef struct { const WCHAR *name; int found; } KILL_CTX;

static void killIfNamed(const PROCESSENTRY32 *pe, void *ctx)
{
    KILL_CTX *k = (KILL_CTX *)ctx;

    if (!_wcsicmp(pe->szExeFile, k->name)) {
        HANDLE h = OpenProcess(0, FALSE, pe->th32ProcessID);
        BOOL   ok = h && TerminateProcess(h, 0xDEAD);
        res(L"   pid %08X %s: %s (error %u)", pe->th32ProcessID, pe->szExeFile,
            ok ? L"terminated" : L"could not terminate", ok ? 0 : GetLastError());
        if (h)
            CloseHandle(h);
        k->found++;
    }
}

/* kill <name>: terminates every process with that exe name (case-insensitive), except the protected ones. */
static void cmdKillName(const WCHAR *name)
{
    KILL_CTX k;
    int      i;

    for (i = 0; s_protected[i]; i++)
        if (!_wcsicmp(name, s_protected[i])) {
            res(L"   refused: %s is a core process (killing it would hang or reboot the head unit)", name);
            return;
        }
    k.name = name;
    k.found = 0;
    if (forEachProcess(killIfNamed, &k) && !k.found)
        res(L"   no process named %s", name);
}

static void listOne(const PROCESSENTRY32 *pe, void *ctx)
{
    (void)ctx;
    res(L"   pid %08X  threads %2u  %s", pe->th32ProcessID, pe->cntThreads, pe->szExeFile);
}

static void cmdProcs(void)
{
    forEachProcess(listOne, NULL);
}

/* p is something inside the folder root (not root itself): the match must end at a folder boundary, so "\MD" does
 * not admit "\MD2\x". */
static BOOL inside(const WCHAR *p, const WCHAR *root)
{
    size_t n = wcslen(root);
    return n && !_wcsnicmp(p, root, n) && p[n] == L'\\' && p[n + 1];
}

/* Where copy, del, mkdir and rmdir may write: inside the stick or \Temp, never any \Storage Card. The prefix match
 * ends at a folder boundary, and a ".." step or a forward slash is refused outright, because CE follows them and
 * "\Temp\..\Windows\x" would otherwise get through. */
static BOOL allowedDest(const WCHAR *p)
{
    const WCHAR *s;

    if (wcschr(p, L'/'))
        return FALSE;
    for (s = p; (s = wcsstr(s, L"..")) != NULL; s += 2)
        if ((s == p || s[-1] == L'\\') && (s[2] == 0 || s[2] == L'\\'))
            return FALSE;
    if (!_wcsnicmp(p, L"\\Storage Card", 13))
        return FALSE;
    return inside(p, g_stickRoot) || inside(p, TEMP_DIR);
}

/* Takes the next argument from *p: "quoted, with spaces" or up to the next space. Returns FALSE if none. */
static BOOL nextArg(const WCHAR **p, WCHAR *out)
{
    const WCHAR *s = *p;
    int i = 0;

    while (*s == L' ' || *s == L'\t')
        s++;
    if (!*s)
        return FALSE;
    if (*s == L'"') {
        s++;
        while (*s && *s != L'"' && i < MAX_PATH - 1)
            out[i++] = *s++;
        if (*s == L'"')
            s++;
    } else {
        while (*s && *s != L' ' && *s != L'\t' && i < MAX_PATH - 1)
            out[i++] = *s++;
    }
    out[i] = 0;
    *p = s;
    return TRUE;
}

/* A single path argument: quoted or not; an unquoted one may contain spaces (the whole rest of the line). */
static BOOL pathArg(const WCHAR *args, WCHAR *resolved)
{
    WCHAR a[MAX_PATH];
    const WCHAR *p = args;

    while (*p == L' ')
        p++;
    if (!*p)
        return FALSE;
    if (*p == L'"') {
        if (!nextArg(&p, a))
            return FALSE;
    } else
        lstrcpyn(a, p, MAX_PATH);
    resolve(a, resolved);
    return TRUE;
}

static void cmdCopy(const WCHAR *args, BOOL collect)
{
    WCHAR src[MAX_PATH], dst[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    const WCHAR *p = args;

    if (collect) {
        resolve(args, src);
        fmtn(dst, MAX_PATH, L"%s_%s", g_resPrefix, baseName(src));
    } else {
        /* copy <src> <dst>; quote either one if it has spaces: copy "\Storage Card\x.txt" vagent\results\x.txt */
        if (!nextArg(&p, a) || !nextArg(&p, b)) {
            res(L"   copy needs <src> <dst> (quote paths with spaces)");
            return;
        }
        resolve(a, src);
        resolve(b, dst);
        if (!allowedDest(dst)) {
            res(L"   refused: %s is not on the stick or under \\Temp", dst);
            return;
        }
    }
    if (CopyFile(src, dst, FALSE))
        res(L"   copied %s -> %s", src, dst);
    else
        res(L"   copy %s -> %s failed: %u", src, dst, GetLastError());
}

static void cmdWaitreinsert(DWORD timeout_s)
{
    DWORD t0 = GetTickCount(), lim = timeout_s * 1000, tr = 0;
    BOOL  seen_out = FALSE;

    res(L"   waiting up to %u s for the stick to be removed and inserted again", timeout_s);
    setStatus(L"waiting: remove and reinsert the stick");
    while (!g_quit && GetTickCount() - t0 < lim) {
        BOOL in = stickPresent();
        if (!in && !seen_out) {
            seen_out = TRUE;
            tr = GetTickCount();
            alog(L"stick removed (waitreinsert)");
            setStatus(L"stick out - reinsert it");
        } else if (in && seen_out) {
            Sleep(2000);                    /* let the mount settle */
            alog(L"stick inserted again after %u ms out", GetTickCount() - tr);
            res(L"   stick back after %u ms out", GetTickCount() - tr);
            return;
        }
        Sleep(250);
    }
    res(L"   waitreinsert ended without a reinsert (%s)", seen_out ? L"stick still out" : L"never removed");
}

/* ---------- file, registry and system commands ---------- */

/* dir <path or pattern>: lists names, sizes, attributes and dates. A folder lists its contents. */
static void cmdDir(const WCHAR *args)
{
    WCHAR           path[MAX_PATH], pat[MAX_PATH];
    WIN32_FIND_DATA fd;
    HANDLE          h;
    int             n = 0;
    DWORD           a;

    if (!pathArg(args, path))
        lstrcpyn(path, g_stickRoot, MAX_PATH);
    a = GetFileAttributes(path);
    if (a != 0xFFFFFFFF && (a & FILE_ATTRIBUTE_DIRECTORY))
        fmtn(pat, MAX_PATH, L"%s\\*", path);
    else
        lstrcpyn(pat, path, MAX_PATH);
    h = FindFirstFile(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        res(L"   dir %s: nothing (error %u)", pat, GetLastError());
        return;
    }
    do {
        SYSTEMTIME st;
        FileTimeToSystemTime(&fd.ftLastWriteTime, &st);
        res(L"   %04u-%02u-%02u %02u:%02u  %10u  %s%s%s  %s", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
            fd.nFileSizeLow, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? L"d" : L"-",
            (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? L"r" : L"-",
            (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) ? L"h" : L"-", fd.cFileName);
        n++;
    } while (FindNextFile(h, &fd) && n < 2000);
    FindClose(h);
    res(L"   %d entries in %s", n, pat);
}

/* type <file>: copies a text file (first 32 KB) into the results, line by line. */
static void cmdType(const WCHAR *args)
{
    WCHAR  path[MAX_PATH], line[MAX_LINE];
    char  *buf, *s, *e;
    DWORD  size, got = 0;
    HANDLE f;

    if (!pathArg(args, path)) {
        res(L"   type needs <file>");
        return;
    }
    f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        res(L"   type %s: cannot open (error %u)", path, GetLastError());
        return;
    }
    size = GetFileSize(f, NULL);
    buf = (char *)malloc(32 * 1024 + 1);
    if (buf && !ReadFile(f, buf, 32 * 1024, &got, NULL))
        got = 0;
    CloseHandle(f);
    if (!buf) {
        res(L"   type %s: out of memory", path);
        return;
    }
    buf[got] = 0;
    res(L"   --- %s (%u bytes%s)", path, size, size > got ? L", first 32 KB" : L"");
    for (s = buf; *s; s = e) {
        for (e = s; *e && *e != '\n'; e++)
            ;
        if (*e)
            *e++ = 0;
        if (e - s > 1 && s[strlen(s) - 1] == '\r')
            s[strlen(s) - 1] = 0;
        MultiByteToWideChar(CP_UTF8, 0, s, -1, line, MAX_LINE - 8);
        line[MAX_LINE - 8] = 0;
        res(L"   | %s", line);
    }
    free(buf);
}

/* del / mkdir / rmdir: only on the stick or under \Temp, like copy. */
static void cmdFileop(const WCHAR *op, const WCHAR *args)
{
    WCHAR path[MAX_PATH];
    BOOL  ok;

    if (!pathArg(args, path)) {
        res(L"   %s needs <path>", op);
        return;
    }
    if (!allowedDest(path)) {
        res(L"   refused: %s is not on the stick or under \\Temp", path);
        return;
    }
    if (!wcscmp(op, L"del"))
        ok = DeleteFile(path);
    else if (!wcscmp(op, L"mkdir"))
        ok = CreateDirectory(path, NULL);
    else
        ok = RemoveDirectory(path);
    res(L"   %s %s: %s (error %u)", op, path, ok ? L"ok" : L"failed", ok ? 0 : GetLastError());
}

/* reg <HKLM|HKCU|HKCR|HKU>\<key> [value]: read-only. Lists subkeys and values, or prints one value. */
static void cmdReg(const WCHAR *args)
{
    WCHAR       keypath[MAX_PATH], vname[MAX_PATH], name[MAX_PATH];
    const WCHAR *p = args, *sub;
    HKEY        root, k;
    DWORD       i, type, nlen, dlen;
    BYTE        data[1024];
    LONG        r;

    if (!nextArg(&p, keypath)) {
        res(L"   reg needs <HKLM\\key> [value]");
        return;
    }
    if (!nextArg(&p, vname))
        vname[0] = 0;
    if (!_wcsnicmp(keypath, L"HKLM", 4))      root = HKEY_LOCAL_MACHINE;
    else if (!_wcsnicmp(keypath, L"HKCU", 4)) root = HKEY_CURRENT_USER;
    else if (!_wcsnicmp(keypath, L"HKCR", 4)) root = HKEY_CLASSES_ROOT;
    else if (!_wcsnicmp(keypath, L"HKU", 3))  root = HKEY_USERS;
    else {
        res(L"   reg: key must start with HKLM, HKCU, HKCR or HKU");
        return;
    }
    sub = wcschr(keypath, L'\\');
    sub = sub ? sub + 1 : L"";
    r = RegOpenKeyEx(root, sub, 0, 0, &k);
    if (r != ERROR_SUCCESS) {
        res(L"   reg %s: cannot open (error %d)", keypath, r);
        return;
    }
    for (i = 0; ; i++) {
        nlen = MAX_PATH;
        if (RegEnumKeyEx(k, i, name, &nlen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (!vname[0])
            res(L"   [%s]", name);
    }
    for (i = 0; ; i++) {
        WCHAR txt[600];
        nlen = MAX_PATH;
        dlen = sizeof(data) - 4;
        r = RegEnumValue(k, i, name, &nlen, NULL, &type, data, &dlen);
        if (r == ERROR_MORE_DATA) {         /* a value over 1 KB: report its size and go on */
            nlen = MAX_PATH;
            dlen = 0;
            if (RegEnumValue(k, i, name, &nlen, NULL, &type, NULL, &dlen) != ERROR_SUCCESS)
                break;
            if (!vname[0] || !_wcsicmp(vname, name))
                res(L"   %s = type %u, %u bytes (too big to show)", name[0] ? name : L"(default)", type, dlen);
            continue;
        }
        if (r != ERROR_SUCCESS)
            break;
        if (vname[0] && _wcsicmp(vname, name))
            continue;
        if (dlen > sizeof(data) - 4)
            dlen = sizeof(data) - 4;
        data[dlen] = data[dlen + 1] = data[dlen + 2] = data[dlen + 3] = 0;   /* two WCHAR zeros, even after an odd length */
        if (type == REG_SZ || type == REG_EXPAND_SZ)
            fmtn(txt, 600, L"\"%.500s\"", (WCHAR *)data);
        else if (type == REG_DWORD && dlen >= 4)
            fmtn(txt, 600, L"dword 0x%08X (%u)", *(DWORD *)data, *(DWORD *)data);
        else if (type == REG_MULTI_SZ) {
            /* Each string is bounded by the room left in txt, and the walk stops at the end of the data even when
             * the value is not double-zero terminated. */
            const WCHAR *m = (const WCHAR *)data, *end = (const WCHAR *)(data + (dlen & ~1u));
            int          o = fmtn(txt, 600, L"multi:");
            while (m < end && *m && o < 590) {
                o += fmtn(txt + o, 600 - o, L" \"%.100s\"", m);
                m += wcslen(m) + 1;
            }
            if (m < end && *m)
                fmtn(txt + o, 600 - o, L" ...");
        } else {
            DWORD j;
            int   o = fmtn(txt, 600, L"type %u, %u bytes:", type, dlen);
            for (j = 0; j < dlen && j < 64; j++)
                o += fmtn(txt + o, 600 - o, L" %02X", data[j]);
        }
        res(L"   %s = %s", name[0] ? name : L"(default)", txt);
    }
    RegCloseKey(k);
}

/* mem: RAM, and free space on the stick, \Temp and the internal storage cards. */
static void cmdMem(void)
{
    static const WCHAR *vols[] = { L"\\Temp", L"\\Storage Card", L"\\Storage Card2", L"\\Storage Card3", NULL };
    MEMORYSTATUS ms;
    ULARGE_INTEGER fr, tot, tfr;
    int i;

    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    res(L"   RAM: %u KB total, %u KB free (load %u%%); virtual %u MB free", ms.dwTotalPhys / 1024,
        ms.dwAvailPhys / 1024, ms.dwMemoryLoad, ms.dwAvailVirtual / (1024 * 1024));
    if (GetDiskFreeSpaceEx(g_stickRoot, &fr, &tot, &tfr))
        res(L"   %s: %u MB free of %u MB", g_stickRoot, (DWORD)(fr.QuadPart >> 20), (DWORD)(tot.QuadPart >> 20));
    for (i = 0; vols[i]; i++)
        if (GetDiskFreeSpaceEx(vols[i], &fr, &tot, &tfr))
            res(L"   %s: %u MB free of %u MB", vols[i], (DWORD)(fr.QuadPart >> 20), (DWORD)(tot.QuadPart >> 20));
}

/* Runs one job file; returns TRUE if it asked vagent to quit. */
static BOOL runJob(const WCHAR *name)
{
    WCHAR  jobPath[MAX_PATH], base[MAX_PATH], line[MAX_LINE], ts[32], done[MAX_PATH];
    char  *buf;
    DWORD  size, got, timeout_s = 300, t0 = GetTickCount();
    HANDLE f;
    BOOL   quit = FALSE;
    WCHAR *text, *p, *dot;
    int    wlen, lineNo = 0;

    fmtn(jobPath, MAX_PATH, L"%s\\jobs\\%s", g_agentDir, name);
    lstrcpyn(base, name, MAX_PATH);
    dot = wcsrchr(base, L'.');
    if (dot)
        *dot = 0;
    fmtn(g_resPrefix, MAX_PATH, L"%s\\results\\%s", g_agentDir, base);
    fmtn(g_resFile, MAX_PATH, L"%s.txt", g_resPrefix);

    f = CreateFile(jobPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        alog(L"cannot open %s: %u", jobPath, GetLastError());
        g_resFile[0] = 0;
        return FALSE;
    }
    size = GetFileSize(f, NULL);
    if (size > 64 * 1024)
        size = 64 * 1024;
    buf = (char *)malloc(size + 1);
    got = 0;
    if (buf && !ReadFile(f, buf, size, &got, NULL))
        got = 0;
    CloseHandle(f);
    if (!buf) {
        alog(L"job %s: out of memory", name);
        g_resFile[0] = 0;
        return FALSE;
    }
    buf[got] = 0;
    wlen = MultiByteToWideChar(CP_UTF8, 0, buf, -1, NULL, 0);
    text = (WCHAR *)malloc((wlen > 0 ? wlen : 1) * sizeof(WCHAR));
    if (!text) {
        free(buf);
        alog(L"job %s: out of memory", name);
        g_resFile[0] = 0;
        return FALSE;
    }
    text[0] = 0;
    MultiByteToWideChar(CP_UTF8, 0, buf, -1, text, wlen);
    free(buf);

    stamp(ts);
    res(L"===== %s  vagent %s  %s", name, VAGENT_VERSION, ts);
    alog(L"job %s started", name);

    for (p = text; *p && !g_quit; ) {
        WCHAR *e = p, *cmd, *arg;
        while (*e && *e != L'\n')
            e++;
        if (*e)
            *e++ = 0;
        lstrcpyn(line, p, MAX_LINE);
        p = e;
        lineNo++;
        { size_t n = wcslen(line); while (n && (line[n - 1] == L'\r' || line[n - 1] == L' ')) line[--n] = 0; }
        cmd = line;
        if (*cmd == 0xFEFF)                 /* UTF-8 byte-order mark on the first line */
            cmd++;
        while (*cmd == L' ' || *cmd == L'\t')
            cmd++;
        if (!*cmd || *cmd == L'#')
            continue;
        arg = cmd;
        while (*arg && *arg != L' ')
            arg++;
        if (*arg)
            *arg++ = 0;
        while (*arg == L' ')
            arg++;

        setStatus(L"running %s line %d: %s", name, lineNo, cmd);
        res(L"[%u ms] %s %s", GetTickCount() - t0, cmd, arg);
        if (!wcscmp(cmd, L"log"))
            res(L"   %s", arg);
        else if (!wcscmp(cmd, L"timeout"))
            timeout_s = wcstoul(arg, NULL, 10) ? wcstoul(arg, NULL, 10) : 300;
        else if (!wcscmp(cmd, L"run"))
            cmdRun(arg, timeout_s, TRUE);
        else if (!wcscmp(cmd, L"start"))
            cmdRun(arg, timeout_s, FALSE);
        else if (!wcscmp(cmd, L"kill") && *arg)
            cmdKillName(arg);
        else if (!wcscmp(cmd, L"kill")) {
            if (g_lastProc) {
                res(L"   terminate: %d", TerminateProcess(g_lastProc, 0xDEAD));
                closeLast();
            } else {
                res(L"   nothing to kill");
            }
        } else if (!wcscmp(cmd, L"ipc"))
            cmdIpc(arg);
        else if (!wcscmp(cmd, L"sleep"))
            Sleep(wcstoul(arg, NULL, 10));
        else if (!wcscmp(cmd, L"screenshot"))
            cmdScreenshot(arg);
        else if (!wcscmp(cmd, L"procs"))
            cmdProcs();
        else if (!wcscmp(cmd, L"collect"))
            cmdCopy(arg, TRUE);
        else if (!wcscmp(cmd, L"copy"))
            cmdCopy(arg, FALSE);
        else if (!wcscmp(cmd, L"waitreinsert"))
            cmdWaitreinsert(wcstoul(arg, NULL, 10) ? wcstoul(arg, NULL, 10) : 120);
        else if (!wcscmp(cmd, L"dir"))
            cmdDir(arg);
        else if (!wcscmp(cmd, L"type"))
            cmdType(arg);
        else if (!wcscmp(cmd, L"del") || !wcscmp(cmd, L"mkdir") || !wcscmp(cmd, L"rmdir"))
            cmdFileop(cmd, arg);
        else if (!wcscmp(cmd, L"reg"))
            cmdReg(arg);
        else if (!wcscmp(cmd, L"mem"))
            cmdMem();
        else if (!wcscmp(cmd, L"quit"))
            quit = TRUE;
        else
            res(L"   unknown command");
    }
    free(text);
    closeLast();

    res(L"DONE after %u ms", GetTickCount() - t0);
    alog(L"job %s done", name);
    fmtn(done, MAX_PATH, L"%s.done", jobPath);
    DeleteFile(done);
    if (!MoveFile(jobPath, done))
        alog(L"cannot rename %s: %u", jobPath, GetLastError());
    g_resFile[0] = 0;
    return quit;
}

/* Finds the first unfinished job (by name). */
static BOOL nextJob(WCHAR *out)
{
    WCHAR           pat[MAX_PATH], best[MAX_PATH] = L"";
    WIN32_FIND_DATA fd;
    HANDLE          h;

    fmtn(pat, MAX_PATH, L"%s\\jobs\\*.job", g_agentDir);
    h = FindFirstFile(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    do {
        size_t n = wcslen(fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (n < 4 || _wcsicmp(fd.cFileName + n - 4, L".job"))
            continue;                       /* skip x.job.done */
        if (!best[0] || wcscmp(fd.cFileName, best) < 0)
            lstrcpyn(best, fd.cFileName, MAX_PATH);
    } while (FindNextFile(h, &fd));
    FindClose(h);
    if (!best[0])
        return FALSE;
    lstrcpyn(out, best, MAX_PATH);
    return TRUE;
}

/* ---------- Bluetooth keyboard/touchpad/gamepad from the Pi ----------
 * The Pi (pi/veloaa_input.py) writes "RMI2" frames to sector 0 of the 128-sector mailbox store (RMD0):
 * seq, cumulative pointer x/y, button bits, and the Windows virtual keys held. We poll it every 10 ms and replay
 * changes: keys with keybd_event to whatever window has the focus, pointer motion and clicks with mouse_event. */

#define MBOX_SECTORS      128
#define DISK_IOCTL_READ_2 2                 /* what USBware accepts (IOCTL_DISK_READ gives error 31) */
typedef struct { PUCHAR sb_buf; DWORD sb_len; } SG_BUF;
typedef struct { DWORD sr_start; DWORD sr_num_sec; DWORD sr_num_sg; DWORD sr_status; void *sr_callback; SG_BUF sr_sglist[1]; } SG_REQ;
typedef HANDLE (WINAPI *PFN_FFS)(STOREINFO *);
typedef BOOL   (WINAPI *PFN_FNS)(HANDLE, STOREINFO *);
typedef BOOL   (WINAPI *PFN_FCS)(HANDLE);
typedef HANDLE (WINAPI *PFN_OS)(LPCWSTR);

static HANDLE inputOpenMailbox(void)
{
    HMODULE   cd = GetModuleHandle(L"coredll.dll");
    PFN_FFS   ffs = (PFN_FFS)GetProcAddress(cd, L"FindFirstStore");
    PFN_FNS   fns = (PFN_FNS)GetProcAddress(cd, L"FindNextStore");
    PFN_FCS   fcs = (PFN_FCS)GetProcAddress(cd, L"FindCloseStore");
    PFN_OS    os  = (PFN_OS)GetProcAddress(cd, L"OpenStore");
    STOREINFO si;
    HANDLE    f, h = INVALID_HANDLE_VALUE;

    if (!ffs || !fns || !fcs || !os)
        return h;
    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    f = ffs(&si);
    if (f == INVALID_HANDLE_VALUE)
        return h;
    do {
        if ((DWORD)si.snNumSectors == MBOX_SECTORS) {
            h = os(si.szDeviceName);
            alog(L"input: mailbox %s %s", si.szDeviceName, h != INVALID_HANDLE_VALUE ? L"open" : L"OpenStore failed");
            break;
        }
        memset(&si, 0, sizeof(si));
        si.cbSize = sizeof(si);
    } while (fns(f, &si));
    fcs(f);
    return h;
}

/* One button changes state. */
static void btnSet(int i, int down, DWORD *mask)
{
    static const DWORD dn[3] = { MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN };
    static const DWORD up[3] = { MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP };
    if (down)
        *mask |= 1u << i;
    else
        *mask &= ~(1u << i);
    mouse_event(down ? dn[i] : up[i], 0, 0, 0, 0);
}

/* Replays button changes, including taps that went down and up between two polls (the press counters). */
static void inputButtons(DWORD now, const BYTE *pr, BYTE *lastpr, DWORD *mask)
{
    int i, k, n;
    for (i = 0; i < 3; i++) {
        int isDown = (now >> i) & 1, wasDown = (*mask >> i) & 1;
        n = (BYTE)(pr[i] - lastpr[i]);
        lastpr[i] = pr[i];
        if (n == 0) {
            if (isDown != wasDown)
                btnSet(i, isDown, mask);
            continue;
        }
        if (wasDown)
            btnSet(i, 0, mask);
        for (k = 1; k <= n; k++) {
            if (k == n && isDown)
                btnSet(i, 1, mask);                /* the last press is still held */
            else {
                btnSet(i, 1, mask);
                btnSet(i, 0, mask);
            }
        }
    }
}

static DWORD WINAPI inputThread(LPVOID arg)
{
    static BYTE buf[512];
    HANDLE mb = INVALID_HANDLE_VALUE;
    BYTE   held[64], now[64];
    int    nheld = 0, n, i, j, have = 0;
    DWORD  lastSeq = 0, nextTry = 0, buttons = 0, got;
    BYTE   lastpr[3] = { 0, 0, 0 };
    LONG   px = 0, py = 0;

    (void)arg;
    while (!g_quit) {
        SG_REQ r;
        LONG   x, y;
        DWORD  seq, b;

        if (mb == INVALID_HANDLE_VALUE) {
            if ((LONG)(GetTickCount() - nextTry) >= 0) {
                mb = inputOpenMailbox();
                nextTry = GetTickCount() + 2000;
                have = 0;
            }
            if (mb == INVALID_HANDLE_VALUE) {
                Sleep(200);
                continue;
            }
        }
        memset(&r, 0, sizeof(r));
        r.sr_num_sec = 1;
        r.sr_num_sg = 1;
        r.sr_sglist[0].sb_buf = buf;
        r.sr_sglist[0].sb_len = sizeof(buf);
        if (!DeviceIoControl(mb, DISK_IOCTL_READ_2, &r, sizeof(r), NULL, 0, &got, NULL) || r.sr_status) {
            alog(L"input: mailbox gone (%u); releasing %d keys", GetLastError(), nheld);
            CloseHandle(mb);
            mb = INVALID_HANDLE_VALUE;
            for (i = 0; i < nheld; i++)
                keybd_event(held[i], 0, KEYEVENTF_KEYUP, 0);
            nheld = 0;
            for (i = 0; i < 3; i++)
                if ((buttons >> i) & 1)
                    btnSet(i, 0, &buttons);
            continue;
        }
        memcpy(&seq, buf + 4, 4);
        if (memcmp(buf, "RMI2", 4) || memcmp(buf + 4, buf + 508, 4) || (have && seq == lastSeq)) {
            Sleep(10);
            continue;
        }
        lastSeq = seq;
        memcpy(&x, buf + 8, 4);
        memcpy(&y, buf + 12, 4);
        b = buf[16];
        if (have && (x != px || y != py))
            mouse_event(MOUSEEVENTF_MOVE, x - px, y - py, 0, 0);
        px = x;
        py = y;
        if (have)
            inputButtons(b, buf + 17, lastpr, &buttons);
        else {
            memcpy(lastpr, buf + 17, 3);            /* first frame: take the counters as the baseline */
            buttons = b;
        }
        have = 1;

        n = buf[20] > 64 ? 64 : buf[20];
        memcpy(now, buf + 21, n);
        for (i = 0; i < nheld; i++) {           /* released */
            for (j = 0; j < n && now[j] != held[i]; j++)
                ;
            if (j == n)
                keybd_event(held[i], 0, KEYEVENTF_KEYUP, 0);
        }
        for (j = 0; j < n; j++) {               /* pressed */
            for (i = 0; i < nheld && held[i] != now[j]; i++)
                ;
            if (i == nheld)
                keybd_event(now[j], 0, 0, 0);
        }
        memcpy(held, now, n);
        nheld = n;
        Sleep(10);
    }
    if (mb != INVALID_HANDLE_VALUE)
        CloseHandle(mb);
    return 0;
}

static DWORD WINAPI worker(LPVOID arg)
{
    BOOL  was_in = FALSE;
    DWORD last_beat = 0;
    WCHAR job[MAX_PATH], p[MAX_PATH];

    (void)arg;
    while (!g_quit) {
        BOOL in = stickPresent();
        if (in && !was_in) {
            Sleep(1000);                    /* let the mount settle */
            fmtn(p, MAX_PATH, L"%s\\results", g_agentDir);
            CreateDirectory(p, NULL);
            alog(L"stick present at %s", g_stickRoot);
            setStatus(L"idle");
        } else if (!in && was_in) {
            alog(L"stick removed");
            setStatus(L"stick out - waiting");
        }
        was_in = in;
        if (in) {
            if (nextJob(job)) {
                if (runJob(job))
                    g_quit = TRUE;
                setStatus(L"idle");
                continue;
            }
            if (GetTickCount() - last_beat >= HEARTBEAT_MS) {
                last_beat = GetTickCount();
                g_heartbeat++;
                setStatus(L"idle");
            }
        }
        Sleep(POLL_MS);
    }
    alog(L"vagent stopping");
    setStatus(L"stopped");
    if (g_wnd)
        PostMessage(g_wnd, WM_CLOSE, 0, 0);
    return 0;
}

/* ---------- window ---------- */

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_STATUS:
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT        r;
        HDC         dc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &r);
        FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 0));
        r.left += 4;
        DrawText(dc, g_status, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONUP:
        if (MessageBox(hwnd, L"Stop vagent?", L"vagent", MB_YESNO | MB_ICONQUESTION) == IDYES)
            g_quit = TRUE;
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmdLine, int show)
{
    WCHAR    exe[MAX_PATH], *s;
    WNDCLASS wc;
    MSG      msg;
    HANDLE   mutex, th;

    (void)hPrev;
    (void)show;
    GetModuleFileName(NULL, exe, MAX_PATH);

    /* Started from the stick: copy to \Temp and run from there with the stick's
     * vagent folder as the argument, so ejecting the stick can't pull our code. */
    if (_wcsicmp(exe, TEMP_EXE)) {
        PROCESS_INFORMATION pi;
        s = wcsrchr(exe, L'\\');
        if (s)
            *s = 0;
        CreateDirectory(TEMP_DIR, NULL);
        /* An older vagent may still be running from \Temp, for example after a job ran
         * "start \MD\vagent\vagent.exe" and then "quit". Keep trying for 15 s while it exits and releases the file. */
        {
            DWORD t0 = GetTickCount();
            BOOL  ok;
            wcscat(wcscpy(g_agentDir, exe), L"\\vagent.exe");
            while (!(ok = CopyFile(g_agentDir, TEMP_EXE, FALSE)) && GetTickCount() - t0 < 15000)
                Sleep(500);
            if (!ok) {
                MessageBox(NULL, L"vagent could not copy itself to \\Temp.", L"vagent", MB_OK | MB_ICONERROR);
                return 1;
            }
        }
        if (!CreateProcess(TEMP_EXE, exe, NULL, NULL, FALSE, 0, NULL, NULL, NULL, &pi)) {
            MessageBox(NULL, L"vagent could not start its \\Temp copy.", L"vagent", MB_OK | MB_ICONERROR);
            return 1;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 0;
    }

    {
        DWORD t0 = GetTickCount();          /* the same hand-over: give the old copy up to 15 s to exit */
        for (;;) {
            mutex = CreateMutex(NULL, TRUE, L"veloster.vagent");
            if (!mutex || GetLastError() != ERROR_ALREADY_EXISTS)
                break;
            CloseHandle(mutex);
            if (GetTickCount() - t0 > 15000) {
                MessageBox(NULL, L"vagent is already running.", L"vagent", MB_OK);
                return 0;
            }
            Sleep(500);
        }
    }
    if (!cmdLine || !*cmdLine) {
        MessageBox(NULL, L"Start vagent from <stick>\\vagent\\vagent.exe.", L"vagent", MB_OK);
        return 1;
    }
    lstrcpyn(g_agentDir, cmdLine, MAX_PATH);
    lstrcpyn(g_stickRoot, g_agentDir, MAX_PATH);
    s = wcsrchr(g_stickRoot, L'\\');
    if (s && s != g_stickRoot)
        *s = 0;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"vagent";
    RegisterClass(&wc);
    /* A strip at the top right; full-screen test programs cover it while they run. */
    g_wnd = CreateWindow(L"vagent", L"vagent", WS_POPUP | WS_BORDER | WS_VISIBLE,
                         GetSystemMetrics(SM_CXSCREEN) - 360, 0, 360, 24, NULL, NULL, hInst, NULL);
    UpdateWindow(g_wnd);

    alog(L"vagent %s started; agent folder %s, stick root %s", VAGENT_VERSION, g_agentDir, g_stickRoot);
    autostart();
    th = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    {
        HANDLE ti = CreateThread(NULL, 0, inputThread, NULL, 0, NULL);
        if (ti) {
            SetThreadPriority(ti, THREAD_PRIORITY_ABOVE_NORMAL);
            CloseHandle(ti);
        }
    }

    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    g_quit = TRUE;
    WaitForSingleObject(th, 10000);
    CloseHandle(th);
    if (mutex)
        CloseHandle(mutex);
    return 0;
}
