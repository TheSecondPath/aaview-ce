/*
 * MatMessage.dll - our stand-in for the ISV hook kronos loads at boot (CE 6.0, MIPS).
 *
 * kronos (CtrlISV::ISVInit) loads \Storage Card\ISV\MatMessage.dll if it exists and looks up MatM_CopyData.
 * While the ISV flag is on, it calls MatM_CopyData(0x805a, code, data) for steering-wheel and remote keys
 * (PNX_Remocon, code = key + 300), dash keys, power on/off, AV off, eject and AUX events.
 * These calls are only notifications: kronos ignores the result and handles the key itself.
 *
 * To take the keys away from kronos, a thread started at load also subclasses kronos' IPC window, the window titled
 * "kronos" that LG's apps post 0x8096 messages to:
 *   - While aaview is on screen, phoenix's remote-key messages (PHOENIX_CMD_KRONOS_REMOCON, 0x6e) go to aaview and
 *     not to kronos, so the steering wheel drives Android Auto only, without LG voice recognition or a MODE source
 *     change. Off the AA screen everything goes to kronos as usual.
 *   - phoenix's lights messages (ILL_ON 0x77, ILL_OFF 0x78, NIGHT_MODE 0x7c) are copied to aaview for AA's night
 *     mode. The last one is sent again whenever a new aaview window appears.
 * Kill switch: if \Storage Card\ISV\nohook.txt exists at load, kronos' window is left alone.
 *
 * This runs inside kronos, the LG shell, so it does as little as possible and wraps everything so a fault here
 * can't take kronos down. It posts to aaview's window and keeps a short log in \Temp. The messages to aaview are:
 *   WM_APP+0x20   hard key: wParam = code, lParam = data
 *   WM_APP+0x21   any other ISV call: wParam = msg, lParam = code
 *   WM_APP+0x22   lights: wParam = phoenix command, lParam = data
 *   WM_APP+0x23   the car's phone (hyperion's calls and Bluetooth audio): wParam = function, lParam = data
 *
 * Install: copy to \Storage Card\ISV\MatMessage.dll (Explorer Mode) and restart the head unit. To undo, delete it.
 */
#include <windows.h>
#include <stdio.h>

#define WM_AA_HARDKEY  (WM_APP + 0x20)
#define WM_AA_ISVMSG   (WM_APP + 0x21)
#define WM_AA_LIGHTS   (WM_APP + 0x22)
#define WM_AA_PHONE    (WM_APP + 0x23)

#define IPC_MSG        0x8096
#define SRC_PHOENIX    2
#define SRC_HYPERION   24
#define PNX_REMOCON    0x6e
#define PNX_ILL_ON     0x77
#define PNX_ILL_OFF    0x78
#define PNX_NIGHT_MODE 0x7c

static LONG    g_lines;
static WNDPROC g_kronosProc;
static HWND    g_lastAa;
static WPARAM  g_lightCmd;              /* last lights message from phoenix (0 = none yet) */
static LPARAM  g_lightData;

static void mlog(UINT msg, WPARAM w, LPARAM l)
{
    char   b[96];
    DWORD  n;
    HANDLE f;

    if (InterlockedIncrement(&g_lines) > 2000)
        return;
    f = CreateFile(L"\\Temp\\matmsg_log.txt", GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    SetFilePointer(f, 0, NULL, FILE_END);
    n = (DWORD)_snprintf(b, sizeof(b) - 1, "%u msg %04X code %d (0x%X) data 0x%X\r\n", GetTickCount(), msg, (int)w,
                         (DWORD)w, (DWORD)l);
    if ((int)n < 0)
        n = sizeof(b) - 1;
    WriteFile(f, b, n, &n, NULL);
    CloseHandle(f);
}

/* aaview's window, and a fresh copy of the lights state for each new aaview. */
static HWND aaview(void)
{
    HWND aa = FindWindow(L"aaview", NULL);

    if (aa && aa != g_lastAa && g_lightCmd)
        PostMessage(aa, WM_AA_LIGHTS, g_lightCmd, g_lightData);
    g_lastAa = aa;
    return aa;
}

static LRESULT CALLBACK kronosHook(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    __try {
        if (msg == IPC_MSG && ((wp >> 8) & 0xff) == SRC_HYPERION) {
            /* hyperion (the car's phone app) telling kronos about calls and Bluetooth audio: copied to aaview,
             * and kronos still gets them. Numbers from kronos' table for hyperion: 0x3c-0x3f hands-free and AVRCP
             * connected/disconnected, 0x46-0x4d the call (dialling, incoming, ongoing, end, accept, reject,
             * missed, hands-free), 0x50-0x53 Bluetooth audio start/stop/play/pause. */
            UINT fnc = (UINT)(wp >> 16) & 0xff;
            if ((fnc >= 0x3c && fnc <= 0x3f) || (fnc >= 0x46 && fnc <= 0x4d) || (fnc >= 0x50 && fnc <= 0x53)) {
                HWND aa = aaview();
                mlog(0x4000 | fnc, fnc, lp);
                if (aa)
                    PostMessage(aa, WM_AA_PHONE, fnc, lp);
            }
        } else if (msg == IPC_MSG && ((wp >> 8) & 0xff) == SRC_PHOENIX) {
            UINT fnc = (UINT)(wp >> 16) & 0xff;
            HWND aa = aaview();

            if (fnc == PNX_ILL_ON || fnc == PNX_ILL_OFF || fnc == PNX_NIGHT_MODE) {
                mlog(0x1000 | fnc, fnc, lp);
                g_lightCmd = fnc;
                g_lightData = lp;
                if (aa)
                    PostMessage(aa, WM_AA_LIGHTS, fnc, lp);
            } else if (fnc == PNX_REMOCON && aa && IsWindowVisible(aa)) {
                /* Android Auto is on screen: the key is for it alone. */
                mlog(0x2000 | fnc, (lp & 0xff) + 300, lp);
                PostMessage(aa, WM_AA_HARDKEY, (lp & 0xff) + 300, 0);
                return 0;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return CallWindowProc(g_kronosProc, hwnd, msg, wp, lp);
}

static DWORD WINAPI hookThread(LPVOID unused)
{
    int i;

    (void)unused;
    __try {
        if (GetFileAttributes(L"\\Storage Card\\ISV\\nohook.txt") != 0xFFFFFFFF) {
            mlog(0x3000, 0, 0);         /* kill switch present */
            return 0;
        }
        for (i = 0; i < 300; i++) {     /* kronos creates its window after loading us, so wait up to 5 min */
            HWND  k = FindWindow(NULL, L"kronos");
            DWORD pid = 0;
            if (k) {
                GetWindowThreadProcessId(k, &pid);
                if (pid == GetCurrentProcessId()) {
                    g_kronosProc = (WNDPROC)GetWindowLong(k, GWL_WNDPROC);
                    if (g_kronosProc) {
                        SetWindowLong(k, GWL_WNDPROC, (LONG)kronosHook);
                        mlog(0x3001, (WPARAM)k, (LPARAM)g_kronosProc);   /* hooked */
                        /* aaview usually starts after phoenix's lights message at boot, and the lights may not
                         * change again for the whole drive. So look for a new aaview window every 2 s and give it
                         * the current state, instead of waiting for the next message from phoenix. */
                        for (;;) {
                            Sleep(2000);
                            aaview();
                        }
                    }
                    return 0;
                }
                mlog(0x3002, (WPARAM)k, (LPARAM)pid);                   /* a "kronos" window in another process */
                return 0;
            }
            Sleep(1000);
        }
        mlog(0x3003, 0, 0);             /* no kronos window */
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

int MatM_CopyData(UINT msg, WPARAM w, LPARAM l)    /* exported undecorated by build.bat */
{
    __try {
        HWND aa = aaview();
        mlog(msg, w, l);
        if (aa) {
            if (msg == 0x805a)
                PostMessage(aa, WM_AA_HARDKEY, w, l);
            else
                PostMessage(aa, WM_AA_ISVMSG, msg, (LPARAM)w);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

BOOL WINAPI DllMain(HANDLE h, DWORD reason, LPVOID reserved)
{
    (void)h;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE t;
        mlog(0, 4, 0);                  /* "loaded" marker; the code is the version, 4 for 0.4 */
        t = CreateThread(NULL, 0, hookThread, NULL, 0, NULL);
        if (t)
            CloseHandle(t);
    }
    return TRUE;
}
