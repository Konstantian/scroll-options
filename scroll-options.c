// scroll-options: tray utility that slows down scrolling on a Windows PC you
// stream to with Moonlight/Sunshine.
//
// Native scrolling over Moonlight feels right but travels too far, even at
// one line per notch in Windows. scroll-options multiplies every wheel event
// from Sunshine by the speed setting (a percentage) and injects it right away,
// with no smoothing. Mice plugged into the PC itself, Ctrl+wheel zoom and
// windows we can't inject into are left alone.
//
// Sunshine either injects the client's mouse with SendInput, which flags the
// events as injected, or, with its licensed virtual HID driver, through a
// virtual "libvirtualhid Mouse". That one looks like hardware to the hook, so
// Raw Input tracks whether it or a local mouse was used last.
//
// It works through a low-level mouse hook, so it is only active while the
// tray app runs. The speed is saved under HKCU\Software\scroll-options and
// restored when the tray app starts. Off unhooks it without exiting. Autostart in the tray menu starts the
// exe with Windows.
//
//   scroll-options.exe                  run in the system tray
//   scroll-options.exe speed 60         set the speed in percent and turn on
//   scroll-options.exe off              stop scaling until a speed is set
//   scroll-options.exe status           print the speed, or off
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
#include <stdio.h>
#include <stdlib.h>

#ifndef VERSION
#define VERSION "dev"
#endif

#define APP_NAME L"scroll-options"
#define REG_KEY L"Software\\scroll-options"
#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define HOMEPAGE L"https://github.com/Konstantian/scroll-options"
#define WM_TRAY (WM_APP + 1)
#define WM_RELOAD (WM_APP + 2) // CLI saved new settings
#define ID_EXIT 20
#define ID_AUTOSTART 22
#define ID_ABOUT 23
#define ID_OFF 24
#define ID_SPEED 30            // menu ids ID_SPEED + index into speeds

// Marks wheel events we inject so the hook lets them through.
#define INJECTED_TAG ((ULONG_PTR)0x5C0110F7)

// Hardware ID of Sunshine's virtual mouse (libvirtualhid's mouse profile).
#define SUNSHINE_MOUSE_ID L"VID_1209&PID_0003"

// Speed in percent, the presets in the tray menu and the range the CLI
// accepts.
#define SPEED_DEFAULT 60
#define SPEED_MIN 5
#define SPEED_MAX 200
static const int speeds[] = {10, 20, 25, 30, 40, 50, 60, 70, 80, 90, 100};

enum axis { AXIS_V, AXIS_H };

static NOTIFYICONDATAW nid;
static UINT wm_taskbar_created;
static HWND tray_hwnd;
static HANDLE tray_mutex;
static HHOOK hook;
static int speed = SPEED_DEFAULT;
static BOOL enabled = TRUE;
static DWORD own_integrity;
static BOOL stream_mouse_last; // Sunshine's virtual mouse was used last

// ---- settings -------------------------------------------------------------

static DWORD load_dword(const wchar_t *name, DWORD def)
{
    DWORD v, size = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, name, RRF_RT_REG_DWORD,
                     NULL, &v, &size) == ERROR_SUCCESS)
        return v;
    return def;
}

static BOOL save_dword(const wchar_t *name, DWORD v)
{
    HKEY key;
    LSTATUS rc = RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0,
                                 KEY_SET_VALUE, NULL, &key, NULL);
    if (rc == ERROR_SUCCESS) {
        rc = RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&v,
                            sizeof v);
        RegCloseKey(key);
    }
    SetLastError(rc);
    return rc == ERROR_SUCCESS;
}

static int load_speed(void)
{
    DWORD v = load_dword(L"Speed", SPEED_DEFAULT);
    return v >= SPEED_MIN && v <= SPEED_MAX ? (int)v : SPEED_DEFAULT;
}

static BOOL load_enabled(void)
{
    return load_dword(L"Enabled", 1) != 0;
}

// Picking a speed also turns scaling back on.
static BOOL save_speed(int pct)
{
    return save_dword(L"Speed", pct) && save_dword(L"Enabled", 1);
}

// ---- autostart ------------------------------------------------------------

// "Autostart" registers the running exe under HKCU's Run key.

static BOOL autostart_enabled(void)
{
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME, RRF_RT_REG_SZ,
                        NULL, NULL, NULL) == ERROR_SUCCESS;
}

static LSTATUS set_autostart(BOOL on)
{
    HKEY key;
    LSTATUS rc = RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0,
                                 KEY_SET_VALUE, NULL, &key, NULL);
    if (rc != ERROR_SUCCESS)
        return rc;
    if (on) {
        wchar_t cmd[MAX_PATH + 2] = L"\"";
        GetModuleFileNameW(NULL, cmd + 1, MAX_PATH);
        lstrcatW(cmd, L"\"");
        rc = RegSetValueExW(key, APP_NAME, 0, REG_SZ, (const BYTE *)cmd,
                            (lstrlenW(cmd) + 1) * sizeof *cmd);
    } else {
        rc = RegDeleteValueW(key, APP_NAME);
    }
    RegCloseKey(key);
    return rc;
}

static void show_error(const wchar_t *what, DWORD err)
{
    wchar_t msg[512];
    int n = wsprintfW(msg, L"%s (%lu)\n\n", what, err);
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, err, 0, msg + n, ARRAYSIZE(msg) - n, NULL);
    MessageBoxW(NULL, msg, L"Scroll Options", MB_OK | MB_ICONERROR);
}

// ---- scaling --------------------------------------------------------------

// Remainders below one wheel unit carry over to the next event, so slow
// scrolls at a low speed still add up. Only the hook thread touches these.
static double carry[2];

// SendInput can't be called from inside the hook: the injected event goes
// through the same hook on the same thread, so all mouse input stalls until
// Windows times the hook out. A worker thread injects instead.
static CRITICAL_SECTION inject_lock;
static HANDLE inject_wake;
static int inject_out[2]; // whole wheel units waiting to be injected

static void inject_wheel(enum axis axis, int delta)
{
    INPUT in = {0};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = axis == AXIS_H ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
    in.mi.mouseData = (DWORD)delta;
    in.mi.dwExtraInfo = INJECTED_TAG;
    SendInput(1, &in, sizeof in);
}

static DWORD WINAPI inject_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        WaitForSingleObject(inject_wake, INFINITE);
        EnterCriticalSection(&inject_lock);
        int out[2] = {inject_out[0], inject_out[1]};
        inject_out[0] = inject_out[1] = 0;
        LeaveCriticalSection(&inject_lock);
        for (int a = 0; a < 2; a++)
            if (out[a])
                inject_wheel(a, out[a]);
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

// Returns TRUE if it took the event over (the hook then swallows it).
static BOOL scale_wheel(enum axis axis, int delta, const MSLLHOOKSTRUCT *ms)
{
    if (speed == 100 || GetAsyncKeyState(VK_CONTROL) < 0 || // zoom as is
        out_of_reach(ms->pt))
        return FALSE;
    if (carry[axis] * delta < 0) // reversed: drop the old remainder
        carry[axis] = 0;
    carry[axis] += delta * speed / 100.0;
    int out = (int)carry[axis];
    carry[axis] -= out;
    if (out) {
        EnterCriticalSection(&inject_lock);
        inject_out[axis] += out;
        LeaveCriticalSection(&inject_lock);
        SetEvent(inject_wake);
    }
    return TRUE;
}

// ---- Sunshine input -------------------------------------------------------

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
static void watch_mice(void)
{
    RAWINPUTDEVICE rid = {0x01, 0x02, RIDEV_INPUTSINK, tray_hwnd};
    RegisterRawInputDevices(&rid, 1, sizeof rid);
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
// after switching between Sunshine's virtual mouse and a local one can be
// misjudged. Other programs that inject wheel input would count as Sunshine
// too.
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
        if (ms->dwExtraInfo != INJECTED_TAG && delta && from_stream(ms) &&
            scale_wheel(axis, delta, ms))
            return 1;
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

// ---- tray -----------------------------------------------------------------

static void tray_update(void)
{
    wchar_t tip[64];
    if (!enabled)
        lstrcpyW(tip, L"Scroll Options: off");
    else
        wsprintfW(tip, L"Scroll Options: %d%%%s", speed,
                  !hook ? L" (hook failed)" : L"");
    lstrcpynW(nid.szTip, tip, ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void apply_settings(void)
{
    speed = load_speed();
    enabled = load_enabled();
    carry[0] = carry[1] = 0;
    if (!enabled && hook) {
        UnhookWindowsHookEx(hook);
        hook = NULL;
    } else if (enabled && !hook) {
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
    nid.hIcon = LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1),
                           IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                           GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    Shell_NotifyIconW(NIM_ADD, &nid);
    tray_update();
}

static void show_about(void)
{
    if (MessageBoxW(NULL,
                    L"Scroll Options " TEXT(VERSION) L"\n\n"
                    L"Slows down scrolling on a PC you stream to with "
                    L"Moonlight/Sunshine.\n\n" HOMEPAGE L"\n\n"
                    L"Open the project page?",
                    L"About Scroll Options",
                    MB_YESNO | MB_ICONINFORMATION) == IDYES)
        ShellExecuteW(NULL, L"open", HOMEPAGE, NULL, NULL, SW_SHOWNORMAL);
}

static void show_menu(HWND hwnd)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Speed for Sunshine input");
    // A speed set from the CLI that isn't a preset shows up as the checked
    // item at the end.
    BOOL preset = FALSE;
    wchar_t label[32];
    for (int i = 0; i < (int)ARRAYSIZE(speeds); i++) {
        wsprintfW(label, L"%d%%", speeds[i]);
        AppendMenuW(m, MF_STRING | (enabled && speeds[i] == speed
                                        ? MF_CHECKED : 0),
                    ID_SPEED + i, label);
        preset |= speeds[i] == speed;
    }
    if (!preset && enabled) {
        wsprintfW(label, L"%d%% (custom)", speed);
        AppendMenuW(m, MF_STRING | MF_CHECKED | MF_GRAYED, 0, label);
    }
    AppendMenuW(m, MF_STRING | (!enabled ? MF_CHECKED : 0), ID_OFF, L"Off");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (autostart_enabled() ? MF_CHECKED : 0),
                ID_AUTOSTART, L"Autostart");
    AppendMenuW(m, MF_STRING, ID_ABOUT, L"About");
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Version " TEXT(VERSION));

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
        if (id >= ID_SPEED && id < ID_SPEED + (int)ARRAYSIZE(speeds)) {
            if (!save_speed(speeds[id - ID_SPEED]))
                show_error(L"Could not save the speed", GetLastError());
            apply_settings();
        } else if (id == ID_OFF) {
            if (!save_dword(L"Enabled", 0))
                show_error(L"Could not save the setting", GetLastError());
            apply_settings();
        } else if (id == ID_AUTOSTART) {
            LSTATUS rc = set_autostart(!autostart_enabled());
            if (rc != ERROR_SUCCESS)
                show_error(L"Could not change autostart", rc);
        } else if (id == ID_ABOUT) {
            show_about();
        } else if (id == ID_EXIT) {
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_RELOAD:
        apply_settings();
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
    tray_mutex = CreateMutexW(NULL, TRUE, L"Local\\scroll-options-tray");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    wm_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = APP_NAME;
    RegisterClassW(&wc);

    // Hidden top-level window so the CLI can find it by class name.
    HWND hwnd = CreateWindowExW(0, APP_NAME, APP_NAME, 0, 0, 0, 0, 0,
                                NULL, NULL, inst, NULL);
    if (!hwnd)
        return 1;
    tray_hwnd = hwnd;

    own_integrity = integrity_of(GetCurrentProcess());
    InitializeCriticalSection(&inject_lock);
    inject_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, inject_thread, NULL, 0, NULL));

    watch_mice();
    tray_add(hwnd);
    apply_settings();

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

// Tells a running tray app to pick up saved settings.
static void reload_tray(void)
{
    HWND tray = FindWindowW(APP_NAME, NULL);
    if (tray)
        PostMessageW(tray, WM_RELOAD, 0, 0);
    else
        fprintf(stderr, "note: takes effect once the tray app is running\n");
}

static int run_cli(wchar_t **argv)
{
    const wchar_t *cmd = argv[1];
    attach_console();
    if (!lstrcmpiW(cmd, L"speed") && argv[2]) {
        int pct = _wtoi(argv[2]);
        if (pct < SPEED_MIN || pct > SPEED_MAX) {
            fprintf(stderr, "error: speed must be %d to %d (percent)\n",
                    SPEED_MIN, SPEED_MAX);
            return 2;
        }
        if (!save_speed(pct)) {
            fprintf(stderr, "error: failed to save setting (%lu)\n",
                    GetLastError());
            return 1;
        }
        printf("%d%%\n", pct);
        reload_tray();
        return 0;
    }
    if (!lstrcmpiW(cmd, L"off")) {
        if (!save_dword(L"Enabled", 0)) {
            fprintf(stderr, "error: failed to save setting (%lu)\n",
                    GetLastError());
            return 1;
        }
        printf("off\n");
        reload_tray();
        return 0;
    }
    if (!lstrcmpiW(cmd, L"version") || !lstrcmpiW(cmd, L"--version")) {
        printf("scroll-options %s\n", VERSION);
        return 0;
    }
    if (!lstrcmpiW(cmd, L"status")) {
        if (load_enabled())
            printf("%d%%\n", load_speed());
        else
            printf("off\n");
        return 0;
    }

    BOOL help = !lstrcmpiW(cmd, L"help") || !lstrcmpiW(cmd, L"-h") ||
                !lstrcmpiW(cmd, L"--help") || !lstrcmpiW(cmd, L"/?");
    fprintf(help ? stdout : stderr,
            "usage: scroll-options [speed N | off | status | version]\n"
            "  (no arguments)   run in the system tray\n"
            "  speed N          scroll speed for Sunshine input, in percent (5-200)\n"
            "  off              stop scaling until a speed is set again\n"
            "  status           print the speed, or off\n"
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
