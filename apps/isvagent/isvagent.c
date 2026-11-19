/*
 * isvagent - launcher that lives in kronos' ISV agent slot (CE 6.0, MIPS).
 *
 * When Test Mode's "ISV On" is set (EngineeringInfo.cfg +0x20), kronos starts \Storage Card\ISV\ISVAgentKronos.exe
 * at boot, after LG's own apps. This program takes that slot and
 * changes nothing on the head unit.
 *
 * It waits up to 10 minutes for the Pi's stick (\MD), copies its log (\Temp\isvagent_log.txt) to
 * \MD\isv\isvagent_log.txt, and starts the program named on the first line of \MD\isv\launch.txt, with optional
 * arguments. The dev stick names \MD\vagent\vagent.exe, which then starts aaview from its autostart.txt. The
 * release stick names \MD\aaview\aaview.exe. Both of those copy themselves to \Temp and restart from there, so the
 * launcher copies nothing and the stick can be unplugged under them.
 *
 * A bar at the top left (vagent's is at the top right) shows what it has done. It is not topmost, so Android Auto
 * and other full-screen programs cover it. A line "bar=brief" in launch.txt hides it 30 s after the start. Tapping
 * the bar stops isvagent, which frees its file so a new version can be copied over it, and leaves running what it
 * started.
 *
 * A hidden window titled "isvAgent" logs any message sent to it, since kronos' CtrlISV forwards hard keys and power
 * events to the agent. The program keeps running so kronos sees the agent alive.
 *
 * Install it as both \Storage Card\ISV\ISVAgentKronos.exe (kronos' name) and ISVAgent.exe (vulcan's name). A named
 * mutex keeps it to one copy.
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

#define ISV_VERSION   L"0.3"
#define TEMP_LOG      L"\\Temp\\isvagent_log.txt"
#define STICK         L"\\MD"
#define STICK_DIR     L"\\MD\\isv"
#define STICK_LOG     L"\\MD\\isv\\isvagent_log.txt"
#define LAUNCH_TXT    L"\\MD\\isv\\launch.txt"
#define STRIP_MS      30000
#define WAIT_STICK_MS (10 * 60 * 1000)
#define LOGGED_MSGS   200
#define LOG_LINE      600               /* WCHARs in one log line, with its time stamp and line end */

/* coredll has no lstrcpyn; bounded copy that always terminates. */
#define lstrcpyn(d, s, n)  (wcsncpy((d), (s), (n) - 1), (d)[(n) - 1] = 0, (d))

static HWND             g_strip, g_msgWnd;
static CRITICAL_SECTION g_lock;
static LONG             g_msgCount;
static WCHAR            g_barText[96] = L"isvagent " ISV_VERSION L": waiting for the stick";
#define BAR_W         330
#define BAR_H         24
#define WM_BAR_BRIEF  (WM_APP + 1)
static DWORD            g_t0;           /* when we started */

/* What the bar says (from any thread). */
static void barSay(const WCHAR *what)
{
    EnterCriticalSection(&g_lock);
    _snwprintf(g_barText, 95, L"isvagent %s: %s", ISV_VERSION, what);
    g_barText[95] = 0;
    LeaveCriticalSection(&g_lock);
    if (g_strip)
        InvalidateRect(g_strip, NULL, TRUE);
}

static void appendUtf8(const WCHAR *path, const WCHAR *text)
{
    char   a[3 * LOG_LINE];             /* UTF-8 takes up to 3 bytes per WCHAR */
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

static BOOL dirExists(const WCHAR *p)
{
    DWORD a = GetFileAttributes(p);
    return a != 0xFFFFFFFF && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static volatile BOOL g_stickLog;        /* also log to the stick once it has been seen */

static void ilog(const WCHAR *fmt, ...)
{
    WCHAR      line[LOG_LINE];
    SYSTEMTIME st;
    va_list    ap;
    int        n;

    GetLocalTime(&st);
    n = wsprintf(line, L"%04u-%02u-%02u %02u:%02u:%02u  +%6u ms  ", st.wYear, st.wMonth, st.wDay, st.wHour,
                 st.wMinute, st.wSecond, GetTickCount());           /* at most 43 characters */
    /* A line that does not fit is cut. _vsnwprintf leaves no terminator then, so one is set here. */
    va_start(ap, fmt);
    _vsnwprintf(line + n, LOG_LINE - n - 3, fmt, ap);
    va_end(ap);
    line[LOG_LINE - 3] = 0;
    wcscat(line, L"\r\n");
    EnterCriticalSection(&g_lock);
    appendUtf8(TEMP_LOG, line);
    if (g_stickLog && dirExists(STICK_DIR))
        appendUtf8(STICK_LOG, line);
    LeaveCriticalSection(&g_lock);
}

/* Copies the \Temp log to the stick (the stick may appear long after boot). */
static void copyLogToStick(void)
{
    CreateDirectory(STICK_DIR, NULL);
    EnterCriticalSection(&g_lock);
    CopyFile(TEMP_LOG, STICK_LOG, FALSE);
    g_stickLog = TRUE;
    LeaveCriticalSection(&g_lock);
}

/* launch.txt's first line: the program, then optionally its arguments.
 *   \MD\aaview\aaview.exe
 *   \MD\aaview\aaview.exe /auto
 *   "\Storage Card\some folder\tool.exe" one two
 * Without quotes the program ends at the first ".exe" followed by a space or the end of the line, since folder
 * names here have spaces in them. If there is none, the whole line is the program. */
static void launchFromStick(void)
{
    char   a[MAX_PATH * 2];
    WCHAR  line[MAX_PATH * 2], *path, *args = NULL, *p;
    DWORD  got = 0, i;
    HANDLE h = CreateFile(LAUNCH_TXT, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    PROCESS_INFORMATION pi;

    if (h == INVALID_HANDLE_VALUE) {
        ilog(L"no %s; nothing to launch", LAUNCH_TXT);
        barSay(L"no launch.txt, nothing started");
        return;
    }
    ReadFile(h, a, sizeof(a) - 1, &got, NULL);
    CloseHandle(h);
    a[got] = 0;
    for (i = 0; i + 9 <= got; i++) {    /* a "bar=brief" line anywhere in the file */
        if ((i == 0 || a[i - 1] == '\n' || a[i - 1] == '\r') && !_strnicmp(a + i, "bar=brief", 9)) {
            if (g_strip)                /* the timer has to be set on the window's own thread */
                PostMessage(g_strip, WM_BAR_BRIEF, 0, 0);
            ilog(L"launch.txt says bar=brief: the bar goes 30 s after the start");
            break;
        }
    }
    for (i = 0; i < got && a[i] != '\r' && a[i] != '\n' && a[i]; i++)
        ;
    a[i] = 0;
    line[0] = 0;
    MultiByteToWideChar(CP_UTF8, 0, a, -1, line, MAX_PATH * 2);
    line[MAX_PATH * 2 - 1] = 0;
    path = line;
    if (*path == 0xFEFF)
        path++;                         /* Notepad's byte order mark */
    while (*path == L' ' || *path == L'\t')
        path++;
    for (p = path + wcslen(path); p > path && (p[-1] == L' ' || p[-1] == L'\t'); )
        *--p = 0;
    if (*path == L'"') {
        path++;
        for (p = path; *p && *p != L'"'; p++)
            ;
        if (*p) {
            *p++ = 0;
            args = p;
        }
    } else {
        for (p = path; *p; p++) {
            if (!_wcsnicmp(p, L".exe", 4) && (p[4] == L' ' || p[4] == L'\t')) {
                p[4] = 0;
                args = p + 5;
                break;
            }
        }
    }
    while (args && (*args == L' ' || *args == L'\t'))
        args++;
    if (args && !*args)
        args = NULL;
    if (!path[0]) {
        ilog(L"%s is empty; nothing to launch", LAUNCH_TXT);
        barSay(L"launch.txt is empty");
        return;
    }
    if (wcslen(path) >= MAX_PATH) {
        ilog(L"%s: the program's path is too long; nothing launched", LAUNCH_TXT);
        barSay(L"launch.txt: path too long");
        return;
    }
    if (CreateProcess(path, args, NULL, NULL, FALSE, 0, NULL, NULL, NULL, &pi)) {
        WCHAR said[80];
        const WCHAR *name = wcsrchr(path, L'\\');

        ilog(L"launched %s%s%s (pid %08X)", path, args ? L" " : L"", args ? args : L"", pi.dwProcessId);
        _snwprintf(said, 79, L"started %s", name ? name + 1 : path);
        said[79] = 0;
        barSay(said);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        DWORD e = GetLastError();
        WCHAR said[80];

        ilog(L"CreateProcess %s failed: %u", path, e);
        _snwprintf(said, 79, L"could not start it (error %u)", e);
        said[79] = 0;
        barSay(said);
    }
}

static DWORD WINAPI stickThread(LPVOID arg)
{
    DWORD t0 = GetTickCount();

    (void)arg;
    while (!dirExists(STICK)) {
        if (GetTickCount() - t0 > WAIT_STICK_MS) {
            ilog(L"no %s after %u s; giving up on the stick", STICK, WAIT_STICK_MS / 1000);
            barSay(L"no stick after 10 min");
            return 0;
        }
        Sleep(1000);
    }
    Sleep(2000);                        /* let the mount settle */
    copyLogToStick();
    ilog(L"stick %s present after %u ms of waiting", STICK, GetTickCount() - t0);
    launchFromStick();
    return 0;
}

static LRESULT CALLBACK stripProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT        r;
        HDC         dc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &r);
        FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0, 255, 255));
        r.left += 4;
        {
            WCHAR text[96];

            EnterCriticalSection(&g_lock);
            wcscpy(text, g_barText);
            LeaveCriticalSection(&g_lock);
            DrawText(dc, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONUP:                  /* a tap stops the launcher so its file can be replaced */
        if (MessageBox(hwnd, L"Stop isvagent?\r\n\r\nWhat it started keeps running. It starts again when the head "
                       L"unit next starts.", L"isvagent", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) == IDYES) {
            ilog(L"stopped from its bar");
            DestroyWindow(hwnd);
            g_strip = NULL;
            if (g_msgWnd)
                DestroyWindow(g_msgWnd);   /* its WM_DESTROY ends the message loop */
        }
        return 0;
    case WM_BAR_BRIEF: {
        DWORD up = GetTickCount() - g_t0;

        SetTimer(hwnd, 1, up < STRIP_MS ? STRIP_MS - up : 1, NULL);
        return 0;
    }
    case WM_TIMER:
        KillTimer(hwnd, 1);
        DestroyWindow(hwnd);
        g_strip = NULL;
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* Hidden window titled "isvAgent": logs whatever kronos (or anyone) sends it. */
static LRESULT CALLBACK msgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CLOSE) {
        ilog(L"WM_CLOSE received; exiting");
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    if (msg >= WM_USER || msg == WM_COPYDATA) {
        if (InterlockedIncrement(&g_msgCount) <= LOGGED_MSGS) {
            if (msg == 0x8096)          /* LG IPC: wParam = cmd<<24 | fnc<<16 | src<<8 | len */
                ilog(L"IPC msg: src %u cmd %u fnc %u len %u data %08X", (wp >> 8) & 0xFF, wp >> 24,
                     (wp >> 16) & 0xFF, wp & 0xFF, lp);
            else if (msg == WM_COPYDATA && lp) {
                COPYDATASTRUCT *cd = (COPYDATASTRUCT *)lp;
                ilog(L"WM_COPYDATA from %08X: dwData %08X, %u bytes, first dword %08X", wp, cd->dwData, cd->cbData,
                     (cd->cbData >= 4 && cd->lpData) ? *(DWORD *)cd->lpData : 0);
            } else
                ilog(L"msg %04X wParam %08X lParam %08X", msg, wp, lp);
        }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmdLine, int show)
{
    WCHAR    exe[MAX_PATH];
    WNDCLASS wc;
    MSG      msg;
    HANDLE   mutex, th;

    (void)hPrev;
    (void)show;
    g_t0 = GetTickCount();
    InitializeCriticalSection(&g_lock);
    mutex = CreateMutex(NULL, TRUE, L"veloster.isvagent");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;                       /* both names installed: only the first one runs */

    GetModuleFileName(NULL, exe, MAX_PATH);
    ilog(L"===== isvagent %s started from %s, command line [%s], uptime %u s", ISV_VERSION, exe,
         cmdLine ? cmdLine : L"", GetTickCount() / 1000);

    memset(&wc, 0, sizeof(wc));
    wc.hInstance = hInst;
    wc.lpfnWndProc = msgProc;
    wc.lpszClassName = L"isvAgentMsg";
    RegisterClass(&wc);
    g_msgWnd = CreateWindow(L"isvAgentMsg", L"isvAgent", WS_POPUP, 0, 0, 1, 1, NULL, NULL, hInst, NULL);

    wc.lpfnWndProc = stripProc;
    wc.lpszClassName = L"isvAgentStrip";
    RegisterClass(&wc);
    /* Top left, since vagent's bar is top right. Not topmost, so full screens cover it. */
    g_strip = CreateWindow(L"isvAgentStrip", L"isvagent", WS_POPUP | WS_BORDER | WS_VISIBLE,
                           0, 0, BAR_W, BAR_H, NULL, NULL, hInst, NULL);
    if (g_strip)
        UpdateWindow(g_strip);
    ilog(L"windows: msg %08X, strip %08X", g_msgWnd, g_strip);

    th = CreateThread(NULL, 0, stickThread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);

    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    ilog(L"isvagent exiting");
    if (mutex)
        CloseHandle(mutex);
    return 0;
}
