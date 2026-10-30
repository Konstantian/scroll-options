// scroll-options: tray utility for scrolling on a Windows PC you stream to
// from a Mac with Moonlight/Sunshine.
//
//   native       leave Windows alone: no hook, no settings changed
//   auto         passthrough for input from Sunshine, native for local mice
//   passthrough  make the Mac's trackpad scroll the way it does on the Mac
//
// Moonlight on macOS sends one wheel notch (120) per ~10 pt of trackpad
// scrolling, and Windows apps scroll three lines (~100 px in a browser) per
// notch, so a slow swipe travels about ten times as far as on the Mac.
// Passthrough scales trackpad deltas back down and replays them evenly, once
// per display frame. A real mouse wheel (whole notches), Ctrl+wheel zoom and
// windows we can't inject into are left alone.
//
// Auto does the same, but only for wheel events Sunshine produced, so mice
// plugged into the PC itself always scroll natively. Sunshine either injects
// the client's mouse with SendInput, which flags the events as injected, or,
// with its licensed virtual HID driver, through a virtual "libvirtualhid
// Mouse". That one looks like hardware to the hook, so Raw Input tracks
// whether it or a local mouse was used last.
//
// Passthrough and auto work through a low-level mouse hook, so it is only active
// while the tray app runs. Settings are saved under
// HKCU\Software\scroll-options and restored when the tray app starts.
//
//   scroll-options.exe                  run in the system tray
//   scroll-options.exe native           leave scrolling alone
//   scroll-options.exe auto             passthrough only for Sunshine input
//   scroll-options.exe passthrough      Mac trackpad over Moonlight/Sunshine
//   scroll-options.exe toggle           switch between native and passthrough
//   scroll-options.exe status           print current mode
//   scroll-options.exe version          print the version

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef VERSION
#define VERSION "dev"
#endif

#define APP_NAME L"scroll-options"
#define REG_KEY L"Software\\scroll-options"
#define WM_TRAY (WM_APP + 1)
#define WM_RELOAD (WM_APP + 2) // CLI saved new settings
#define ID_MODE 1              // menu ids ID_MODE + enum mode
#define ID_EXIT 20

// Marks wheel events we inject so the hook lets them through.
#define INJECTED_TAG ((ULONG_PTR)0x5C0110F7)

// Passthrough feel: wheel units out per unit in. Moonlight
// sends ~12 units per pt of Mac trackpad scrolling, which a browser turns
// into ~10 px; 0.2 brings slow swipes to ~2 px per pt and, since Moonlight
// clamps fast events to one notch, flicks with momentum to about 1:1.
#define GAIN 0.2
#define GLIDE_TAU 0.025   // seconds; smoothing of the replayed deltas
#define GESTURE_MS 150    // whole notches this soon after a trackpad delta
                          // belong to the same swipe

// Hardware ID of Sunshine's virtual mouse (libvirtualhid's mouse profile).
#define SUNSHINE_MOUSE_ID L"VID_1209&PID_0003"

enum mode { MODE_NATIVE, MODE_AUTO, MODE_PASSTHROUGH, MODE_COUNT };
enum axis { AXIS_V, AXIS_H };

static const wchar_t *mode_names[MODE_COUNT] = {L"native", L"auto",
                                                L"passthrough"};
static const wchar_t *mode_labels[MODE_COUNT] = {L"Native", L"Auto",
                                                 L"Passthrough"};

static NOTIFYICONDATAW nid;
static UINT wm_taskbar_created;
static HWND tray_hwnd;
static HHOOK hook;
static enum mode mode = MODE_AUTO;
static DWORD own_integrity;
static BOOL stream_mouse_last; // Sunshine's virtual mouse was used last

// ---- settings -------------------------------------------------------------

static enum mode load_mode(void)
{
    wchar_t buf[32];
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"Mode", RRF_RT_REG_SZ,
                     NULL, buf, &size) == ERROR_SUCCESS)
        for (int m = 0; m < MODE_COUNT; m++)
            if (!lstrcmpiW(buf, mode_names[m]))
                return m;
    return MODE_AUTO;
}

static BOOL save_mode(enum mode m)
{
    const wchar_t *s = mode_names[m];
    HKEY key;
    LSTATUS rc = RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0,
                                 KEY_SET_VALUE, NULL, &key, NULL);
    if (rc == ERROR_SUCCESS) {
        rc = RegSetValueExW(key, L"Mode", 0, REG_SZ, (const BYTE *)s,
                            (lstrlenW(s) + 1) * sizeof *s);
        RegCloseKey(key);
    }
    SetLastError(rc);
    return rc == ERROR_SUCCESS;
}

// ---- passthrough ----------------------------------------------------------

static CRITICAL_SECTION glide_lock;
static HANDLE glide_wake;
static double glide_left[2];  // wheel units still to scroll, per axis
static double glide_carry[2]; // fraction scrolled but not yet injected

static void inject_wheel(enum axis axis, int delta)
{
    INPUT in = {0};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = axis == AXIS_H ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
    in.mi.mouseData = (DWORD)delta;
    in.mi.dwExtraInfo = INJECTED_TAG;
    SendInput(1, &in, sizeof in);
}

static void glide_cancel(void)
{
    EnterCriticalSection(&glide_lock);
    glide_left[0] = glide_left[1] = glide_carry[0] = glide_carry[1] = 0;
    LeaveCriticalSection(&glide_lock);
}

static void glide_push(enum axis axis, double delta)
{
    EnterCriticalSection(&glide_lock);
    if (glide_left[axis] * delta < 0) // reversed: drop the old glide
        glide_left[axis] = glide_carry[axis] = 0;
    glide_left[axis] += delta;
    LeaveCriticalSection(&glide_lock);
    SetEvent(glide_wake);
}

// Replays pending deltas one display frame at a time with an exponential
// ease-out, injecting the distance covered as high-resolution wheel deltas.
// This also evens out deltas that arrive in bursts from the network.
static DWORD WINAPI glide_thread(LPVOID arg)
{
    (void)arg;
    LARGE_INTEGER freq, last, now;
    QueryPerformanceFrequency(&freq);

    for (;;) {
        WaitForSingleObject(glide_wake, INFINITE);
        QueryPerformanceCounter(&last);
        BOOL active = TRUE;
        while (active) {
            if (FAILED(DwmFlush())) // wait for the next frame
                Sleep(8);
            QueryPerformanceCounter(&now);
            double dt = (double)(now.QuadPart - last.QuadPart) / freq.QuadPart;
            if (dt < 0.002) { // DwmFlush returned early (nothing to compose)
                Sleep(2);
                continue;
            }
            last = now;
            double k = 1 - exp(-fmin(dt, 0.05) / GLIDE_TAU);
            // Don't let the tail of a glide turn into Ctrl+wheel zoom.
            BOOL ctrl = GetAsyncKeyState(VK_CONTROL) < 0;
            int out[2];

            active = FALSE;
            EnterCriticalSection(&glide_lock);
            for (int a = 0; a < 2; a++) {
                if (ctrl)
                    glide_left[a] = glide_carry[a] = 0;
                double step = fabs(glide_left[a]) < 1 ? glide_left[a]
                                                      : glide_left[a] * k;
                glide_left[a] -= step;
                glide_carry[a] += step;
                // An idle axis keeps its rounding remainder for later; rounding
                // it every frame could flip a carry of 0.5 back and forth.
                out[a] = !step          ? 0
                         : glide_left[a] ? (int)glide_carry[a]
                                         : (int)lround(glide_carry[a]);
                glide_carry[a] -= out[a];
                if (glide_left[a])
                    active = TRUE;
            }
            LeaveCriticalSection(&glide_lock);

            for (int a = 0; a < 2; a++)
                if (out[a])
                    inject_wheel(a, out[a]);
        }
    }
    return 0;
}

static DWORD integrity_of(HANDLE process)
{
    DWORD rid = SECURITY_MANDATORY_SYSTEM_RID; // unknown: assume it's above us
    HANDLE token;
    if (process && OpenProcessToken(process, TOKEN_QUERY, &token)) {
        union { TOKEN_MANDATORY_LABEL label; BYTE buf[64]; } info;
        DWORD len;
        if (GetTokenInformation(token, TokenIntegrityLevel, &info, sizeof info,
                                &len)) {
            PSID sid = info.label.Label.Sid;
            rid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
        }
        CloseHandle(token);
    }
    return rid;
}

// SendInput can't reach windows of a higher integrity level (UIPI), such as
// an elevated Task Manager, so their wheel input has to be left alone.
static BOOL out_of_reach(POINT pt)
{
    static DWORD last_pid, last_rid;
    DWORD routing = MOUSEWHEEL_ROUTING_FOCUS, pid = 0;
    SystemParametersInfoW(SPI_GETMOUSEWHEELROUTING, 0, &routing, 0);
    HWND target = routing == MOUSEWHEEL_ROUTING_MOUSE_POS
                      ? WindowFromPoint(pt)
                      : GetForegroundWindow();
    if (!target || !GetWindowThreadProcessId(target, &pid))
        return FALSE;
    if (pid != last_pid) {
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        last_rid = integrity_of(p);
        if (p)
            CloseHandle(p);
        last_pid = pid;
    }
    return last_rid > own_integrity;
}

// Moonlight forwards a mouse wheel as whole notches and a trackpad as
// arbitrary deltas, though fast swipes get clamped to whole notches too.
static BOOL from_trackpad(int delta, DWORD time)
{
    static DWORD last;
    if (delta % WHEEL_DELTA == 0 && time - last > GESTURE_MS)
        return FALSE;
    last = time;
    return TRUE;
}

// Returns TRUE if it took the event over (the hook then swallows it).
static BOOL passthrough_wheel(enum axis axis, int delta,
                              const MSLLHOOKSTRUCT *ms)
{
    if (!from_trackpad(delta, ms->time))
        return FALSE;
    if (GetAsyncKeyState(VK_CONTROL) < 0) { // leave Ctrl+wheel zoom as is
        glide_cancel();
        return FALSE;
    }
    if (out_of_reach(ms->pt))
        return FALSE;
    glide_push(axis, delta * GAIN);
    return TRUE;
}

// ---- auto -----------------------------------------------------------------

static BOOL is_stream_mouse(HANDLE dev)
{
    static HANDLE last_dev;
    static BOOL last_match;
    if (dev != last_dev) {
        wchar_t name[512];
        UINT len = ARRAYSIZE(name);
        UINT n = GetRawInputDeviceInfoW(dev, RIDI_DEVICENAME, name, &len);
        last_match = n && n < ARRAYSIZE(name) && // (UINT)-1 on error
                     wcsstr(CharUpperW(name), SUNSHINE_MOUSE_ID);
        last_dev = dev;
    }
    return last_match;
}

// Raw input from every mouse, even while we're in the background, to see
// which one was used last. Injected input has no device and doesn't count.
static void watch_mice(BOOL on)
{
    RAWINPUTDEVICE rid = {0x01, 0x02, on ? RIDEV_INPUTSINK : RIDEV_REMOVE,
                          on ? tray_hwnd : NULL};
    RegisterRawInputDevices(&rid, 1, sizeof rid);
    stream_mouse_last = FALSE;
}

static void raw_input(HRAWINPUT ri)
{
    RAWINPUTHEADER h;
    UINT size = sizeof h;
    if (GetRawInputData(ri, RID_HEADER, &h, &size, sizeof h) == sizeof h &&
        h.hDevice)
        stream_mouse_last = is_stream_mouse(h.hDevice);
}

// Whether a wheel event came from the Moonlight client. Windows may run the
// hook before or after the event's raw input arrives, so the first event
// after switching mice can be misjudged; local wheels send whole notches,
// which passthrough leaves alone anyway. Other programs that inject wheel
// input would count as Sunshine too.
static BOOL from_stream(const MSLLHOOKSTRUCT *ms)
{
    return (ms->flags & LLMHF_INJECTED) || stream_mouse_last;
}

static LRESULT CALLBACK mouse_hook(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && (wp == WM_MOUSEWHEEL || wp == WM_MOUSEHWHEEL)) {
        MSLLHOOKSTRUCT *ms = (MSLLHOOKSTRUCT *)lp;
        enum axis axis = wp == WM_MOUSEHWHEEL ? AXIS_H : AXIS_V;
        int delta = GET_WHEEL_DELTA_WPARAM(ms->mouseData);
        if (ms->dwExtraInfo != INJECTED_TAG && delta &&
            (mode != MODE_AUTO || from_stream(ms)) &&
            passthrough_wheel(axis, delta, ms))
            return 1;
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

// ---- tray -----------------------------------------------------------------

static void tray_update(void)
{
    wchar_t tip[64];
    wsprintfW(tip, L"Scroll Options: %s%s", mode_names[mode],
              mode != MODE_NATIVE && !hook ? L" (hook failed)" : L"");
    lstrcpynW(nid.szTip, tip, ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void apply_mode(enum mode m)
{
    mode = m;
    glide_cancel();
    watch_mice(m == MODE_AUTO);

    if (m == MODE_NATIVE) {
        if (hook)
            UnhookWindowsHookEx(hook);
        hook = NULL;
    } else if (!hook) {
        hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_hook,
                                 GetModuleHandleW(NULL), 0);
    }
    tray_update();
}

static void tray_add(HWND hwnd)
{
    nid.cbSize = sizeof nid;
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    Shell_NotifyIconW(NIM_ADD, &nid);
    tray_update();
}

static void show_menu(HWND hwnd)
{
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < MODE_COUNT; i++)
        AppendMenuW(m, MF_STRING, ID_MODE + i, mode_labels[i]);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Version " TEXT(VERSION));
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    CheckMenuRadioItem(m, ID_MODE, ID_MODE + MODE_COUNT - 1, ID_MODE + mode,
                       MF_BYCOMMAND);

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd); // required so the menu closes on outside click
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TRAY:
        if (lp == WM_LBUTTONUP || lp == WM_RBUTTONUP)
            show_menu(hwnd);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id >= ID_MODE && id < ID_MODE + MODE_COUNT) {
            save_mode(id - ID_MODE);
            apply_mode(id - ID_MODE);
        } else if (id == ID_EXIT) {
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_RELOAD:
        apply_mode(load_mode());
        return 0;
    case WM_INPUT:
        raw_input((HRAWINPUT)lp);
        break; // DefWindowProc frees the input
    case WM_DESTROY:
        if (hook)
            UnhookWindowsHookEx(hook);
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == wm_taskbar_created) { // explorer restarted
        tray_add(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int run_tray(HINSTANCE inst)
{
    CreateMutexW(NULL, TRUE, L"Local\\scroll-options-tray");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    wm_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = APP_NAME;
    RegisterClassW(&wc);

    // Hidden top-level window so the CLI can find it by class name.
    HWND hwnd = CreateWindowExW(0, APP_NAME, APP_NAME, 0, 0, 0, 0, 0,
                                NULL, NULL, inst, NULL);
    if (!hwnd)
        return 1;
    tray_hwnd = hwnd;

    own_integrity = integrity_of(GetCurrentProcess());
    InitializeCriticalSection(&glide_lock);
    glide_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, glide_thread, NULL, 0, NULL));

    tray_add(hwnd);
    apply_mode(load_mode());

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

// ---- command line ---------------------------------------------------------

// The exe uses the GUI subsystem so the tray mode never flashes a console;
// attach to the parent's console so CLI output is still visible.
static void attach_console(void)
{
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        printf("\n");
    }
}

// Tells a running tray app to pick up saved settings; returns its window.
static HWND reload_tray(void)
{
    HWND tray = FindWindowW(APP_NAME, NULL);
    if (tray)
        PostMessageW(tray, WM_RELOAD, 0, 0);
    return tray;
}

static int set_mode_cli(enum mode m)
{
    if (!save_mode(m)) {
        fprintf(stderr, "error: failed to save setting (%lu)\n", GetLastError());
        return 1;
    }
    HWND tray = reload_tray();
    printf("%ls\n", mode_names[m]);
    if (!tray && m != MODE_NATIVE)
        fprintf(stderr, "note: takes effect once the tray app is running\n");
    return 0;
}

static int run_cli(wchar_t **argv)
{
    const wchar_t *cmd = argv[1];
    attach_console();
    for (int m = 0; m < MODE_COUNT; m++)
        if (!lstrcmpiW(cmd, mode_names[m]))
            return set_mode_cli(m);
    if (!lstrcmpiW(cmd, L"toggle"))
        return set_mode_cli(load_mode() != MODE_NATIVE ? MODE_NATIVE
                                                       : MODE_PASSTHROUGH);
    if (!lstrcmpiW(cmd, L"version") || !lstrcmpiW(cmd, L"--version")) {
        printf("scroll-options %s\n", VERSION);
        return 0;
    }
    if (!lstrcmpiW(cmd, L"status")) {
        printf("%ls\n", mode_names[load_mode()]);
        return 0;
    }

    BOOL help = !lstrcmpiW(cmd, L"help") || !lstrcmpiW(cmd, L"-h") ||
                !lstrcmpiW(cmd, L"--help") || !lstrcmpiW(cmd, L"/?");
    fprintf(help ? stdout : stderr,
            "usage: scroll-options [native | auto | passthrough | toggle | status | version]\n"
            "  (no arguments)   run in the system tray\n"
            "  native           leave Windows scrolling alone\n"
            "  auto             passthrough for Sunshine input, local mice untouched\n"
            "  passthrough      Mac trackpad over Moonlight/Sunshine scrolls like on the Mac\n"
            "  toggle           switch to native, or from native to passthrough\n"
            "  status           print current mode\n"
            "  version          print the version\n");
    return help ? 0 : 2;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)prev; (void)cmdline; (void)show;

    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int rc = (argv && argc > 1) ? run_cli(argv) : run_tray(inst);
    LocalFree(argv);
    return rc;
}
