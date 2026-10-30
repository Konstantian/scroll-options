// scroll-options: tray utility that picks how Windows handles the mouse wheel.
//
//   discrete     native notch-by-notch scrolling (the Windows default);
//                high-resolution input such as a trackpad is rounded to
//                whole notches
//   smooth       macOS-like: each notch glides to a stop and quick flicks
//                accelerate; high-resolution input is left as is
//   passthrough  don't touch wheel input at all, e.g. so a Mac trackpad
//                streamed through Moonlight/Sunshine scrolls exactly as on
//                the Mac
//
// discrete and smooth work through a low-level mouse hook, so they are only
// active while the tray app runs. The mode is saved under
// HKCU\Software\scroll-options and restored when the tray app starts.
//
//   scroll-options.exe               run in the system tray
//   scroll-options.exe discrete      switch to discrete scrolling
//   scroll-options.exe smooth        switch to smooth scrolling
//   scroll-options.exe passthrough   leave wheel input alone
//   scroll-options.exe toggle        switch between smooth and discrete
//   scroll-options.exe status        print current mode

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

#define APP_NAME L"scroll-options"
#define REG_KEY L"Software\\scroll-options"
#define WM_TRAY (WM_APP + 1)
#define WM_RELOAD (WM_APP + 2) // CLI saved a new mode
#define WM_EMIT (WM_APP + 3)   // discrete mode: inject whole notches
#define ID_MODE 1              // menu ids ID_MODE + enum mode
#define ID_EXIT 10

// Marks wheel events we inject so the hook lets them through.
#define INJECTED_TAG ((ULONG_PTR)0x5C0110F7)

// Smooth scrolling feel.
#define GLIDE_TAU 0.09    // seconds; time constant of the ease-out
#define FLICK_MS 150      // notches closer than this accelerate
#define FLICK_GAIN 0.35   // extra distance per consecutive flick notch
#define FLICK_MAX 8       // cap on consecutive flick notches

enum mode { MODE_DISCRETE, MODE_SMOOTH, MODE_PASSTHROUGH, MODE_COUNT };
enum axis { AXIS_V, AXIS_H };

static const wchar_t *mode_names[MODE_COUNT] = {
    L"discrete", L"smooth", L"passthrough",
};
static const wchar_t *mode_labels[MODE_COUNT] = {
    L"Discrete (Windows default)",
    L"Smooth (macOS-like)",
    L"Passthrough (leave input alone)",
};

static NOTIFYICONDATAW nid;
static UINT wm_taskbar_created;
static HWND tray_hwnd;
static HHOOK hook;
static enum mode mode = MODE_PASSTHROUGH;

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
    return MODE_DISCRETE;
}

static BOOL save_mode(enum mode m)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE,
                        NULL, &key, NULL) != ERROR_SUCCESS)
        return FALSE;
    const wchar_t *s = mode_names[m];
    LSTATUS rc = RegSetValueExW(key, L"Mode", 0, REG_SZ, (const BYTE *)s,
                                (lstrlenW(s) + 1) * sizeof *s);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

// ---- wheel ----------------------------------------------------------------

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

static void glide_push(enum axis axis, int delta)
{
    static DWORD last_tick[2];
    static int last_sign[2], streak[2];
    DWORD now = GetTickCount();
    int sign = delta > 0 ? 1 : -1;

    if (sign != last_sign[axis] || now - last_tick[axis] > FLICK_MS)
        streak[axis] = 0;
    else if (streak[axis] < FLICK_MAX)
        streak[axis]++;
    last_sign[axis] = sign;
    last_tick[axis] = now;

    EnterCriticalSection(&glide_lock);
    if (glide_left[axis] * sign < 0) // reversed: drop the old glide
        glide_left[axis] = glide_carry[axis] = 0;
    glide_left[axis] += delta * (1 + FLICK_GAIN * streak[axis]);
    LeaveCriticalSection(&glide_lock);
    SetEvent(glide_wake);
}

// Animates pending glides one display frame at a time with an exponential
// ease-out, injecting the distance covered as high-resolution wheel deltas.
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
                out[a] = glide_left[a] ? (int)glide_carry[a]
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

// Each returns TRUE if it took the event over (the hook then swallows it).

static BOOL smooth_wheel(enum axis axis, int delta)
{
    if (delta % WHEEL_DELTA) // already high-resolution (trackpad, free-spin)
        return FALSE;
    if (GetAsyncKeyState(VK_CONTROL) < 0) { // keep zoom in whole steps
        glide_cancel();
        return FALSE;
    }
    glide_push(axis, delta);
    return TRUE;
}

static BOOL discrete_wheel(enum axis axis, int delta)
{
    static int acc[2];
    static DWORD last_tick[2];
    DWORD now = GetTickCount();

    // After a pause or a reversal, drop the leftover partial notch.
    if (now - last_tick[axis] > 400 || (acc[axis] ^ delta) < 0)
        acc[axis] = 0;
    last_tick[axis] = now;
    if (!acc[axis] && delta % WHEEL_DELTA == 0) // a real notch
        return FALSE;

    acc[axis] += delta;
    int notches = acc[axis] / WHEEL_DELTA;
    acc[axis] -= notches * WHEEL_DELTA;
    if (notches) // inject outside the hook callback
        PostMessageW(tray_hwnd, WM_EMIT, axis, notches * WHEEL_DELTA);
    return TRUE;
}

static LRESULT CALLBACK mouse_hook(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && (wp == WM_MOUSEWHEEL || wp == WM_MOUSEHWHEEL)) {
        MSLLHOOKSTRUCT *ms = (MSLLHOOKSTRUCT *)lp;
        enum axis axis = wp == WM_MOUSEHWHEEL ? AXIS_H : AXIS_V;
        int delta = GET_WHEEL_DELTA_WPARAM(ms->mouseData);
        if (ms->dwExtraInfo != INJECTED_TAG && delta &&
            (mode == MODE_SMOOTH ? smooth_wheel(axis, delta)
                                 : discrete_wheel(axis, delta)))
            return 1;
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

// ---- tray -----------------------------------------------------------------

static void tray_update(void)
{
    wchar_t tip[64];
    wsprintfW(tip, L"Scroll Options: %s%s", mode_names[mode],
              mode != MODE_PASSTHROUGH && !hook ? L" (hook failed)" : L"");
    lstrcpynW(nid.szTip, tip, ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void apply_mode(enum mode m)
{
    mode = m;
    glide_cancel();

    if (m == MODE_PASSTHROUGH) {
        if (hook)
            UnhookWindowsHookEx(hook);
        hook = NULL;
    } else {
        if (!hook)
            hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_hook,
                                     GetModuleHandleW(NULL), 0);
        // Match the "Smooth-scroll list boxes" visual effect to the mode.
        BOOL want = m == MODE_SMOOTH, on = FALSE;
        SystemParametersInfoW(SPI_GETLISTBOXSMOOTHSCROLLING, 0, &on, 0);
        if (!on != !want)
            SystemParametersInfoW(SPI_SETLISTBOXSMOOTHSCROLLING, 0,
                                  (PVOID)(INT_PTR)want,
                                  SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
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
    case WM_EMIT:
        inject_wheel(wp, (int)lp);
        return 0;
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
    tray_hwnd = CreateWindowExW(0, APP_NAME, APP_NAME, 0, 0, 0, 0, 0,
                                NULL, NULL, inst, NULL);
    if (!tray_hwnd)
        return 1;

    InitializeCriticalSection(&glide_lock);
    glide_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, glide_thread, NULL, 0, NULL));

    tray_add(tray_hwnd);
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

static int set_mode_cli(enum mode m)
{
    if (!save_mode(m)) {
        fprintf(stderr, "error: failed to save setting (%lu)\n", GetLastError());
        return 1;
    }
    HWND tray = FindWindowW(APP_NAME, NULL);
    if (tray)
        PostMessageW(tray, WM_RELOAD, 0, 0);
    printf("%ls\n", mode_names[m]);
    if (!tray && m != MODE_PASSTHROUGH)
        fprintf(stderr, "note: takes effect once the tray app is running\n");
    return 0;
}

static int run_cli(const wchar_t *cmd)
{
    attach_console();
    for (int m = 0; m < MODE_COUNT; m++)
        if (!lstrcmpiW(cmd, mode_names[m]))
            return set_mode_cli(m);
    if (!lstrcmpiW(cmd, L"toggle"))
        return set_mode_cli(load_mode() == MODE_SMOOTH ? MODE_DISCRETE
                                                       : MODE_SMOOTH);
    if (!lstrcmpiW(cmd, L"status")) {
        printf("%ls\n", mode_names[load_mode()]);
        return 0;
    }

    BOOL help = !lstrcmpiW(cmd, L"help") || !lstrcmpiW(cmd, L"-h") ||
                !lstrcmpiW(cmd, L"--help") || !lstrcmpiW(cmd, L"/?");
    fprintf(help ? stdout : stderr,
            "usage: scroll-options [discrete | smooth | passthrough | toggle | status]\n"
            "  (no arguments)  run in the system tray\n"
            "  discrete        notch-by-notch scrolling (Windows default)\n"
            "  smooth          macOS-like smooth scrolling\n"
            "  passthrough     leave wheel input alone (e.g. Moonlight/Sunshine)\n"
            "  toggle          switch between smooth and discrete\n"
            "  status          print current mode\n");
    return help ? 0 : 2;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)prev; (void)cmdline; (void)show;

    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int rc = (argv && argc > 1) ? run_cli(argv[1]) : run_tray(inst);
    LocalFree(argv);
    return rc;
}
