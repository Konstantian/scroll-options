// scroll-options: tray utility for scrolling on a Windows PC you stream to
// from a Mac with Moonlight/Sunshine.
//
//   native       leave Windows alone: no hook, no settings changed
//   passthrough  make the Mac's trackpad scroll the way it does on the Mac
//
// Moonlight on macOS sends one wheel notch (120) per ~10 pt of trackpad
// scrolling, and Windows apps scroll three lines (~100 px in a browser) per
// notch, so a slow swipe travels about ten times as far as on the Mac.
// Passthrough scales trackpad deltas back down and replays them evenly, once
// per display frame. Moonlight also caps each event at one notch, so every
// swipe above a modest speed arrives alike; flicks that stay at the cap
// longer get a longer momentum glide. A real mouse wheel (whole notches),
// Ctrl+wheel zoom and windows we can't inject into are left alone.
//
// Passthrough works through a low-level mouse hook, so it is only active
// while the tray app runs. Settings are saved under
// HKCU\Software\scroll-options and restored when the tray app starts.
//
//   scroll-options.exe                  run in the system tray
//   scroll-options.exe native           leave scrolling alone
//   scroll-options.exe passthrough      Mac trackpad over Moonlight/Sunshine
//   scroll-options.exe toggle           switch between native and passthrough
//   scroll-options.exe speed [percent]  print or set the passthrough speed
//   scroll-options.exe status           print current mode

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

#define APP_NAME L"scroll-options"
#define REG_KEY L"Software\\scroll-options"
#define WM_TRAY (WM_APP + 1)
#define WM_RELOAD (WM_APP + 2) // CLI saved new settings
#define ID_MODE 1              // menu ids ID_MODE + enum mode
#define ID_SPEED 10            // menu ids ID_SPEED + index into speeds
#define ID_EXIT 20

// Marks wheel events we inject so the hook lets them through.
#define INJECTED_TAG ((ULONG_PTR)0x5C0110F7)

// Passthrough feel: wheel units out per unit in, at 100% speed. Moonlight
// sends ~12 units per pt of Mac trackpad scrolling, which a browser turns
// into ~10 px; 0.2 brings slow swipes to ~2 px per pt and, since Moonlight
// clamps fast events to one notch, flicks with momentum to about 1:1.
#define GAIN 0.2
#define GLIDE_TAU 0.025   // seconds; smoothing of the replayed deltas
#define GESTURE_MS 150    // whole notches this soon after a trackpad delta
                          // belong to the same swipe

// Momentum stretch after a run of events at Moonlight's cap (see
// stretch_tail). Runs shorter than CAP_MIN_MS are left alone; beyond that,
// each extra second lets the tail run TAIL_GAIN more times ahead of the
// Mac's own momentum, up to TAIL_MAX, fading with TAIL_FADE.
#define CAP_MIN_MS 500
#define TAIL_GAIN 8.0
#define TAIL_MAX 10.0
#define TAIL_FADE 2.0     // seconds
#define DIP_MS 80         // dips under the cap this short don't end a run
#define SWIPE_GAP_MS 150  // a pause this long starts a new swipe
#define SPEED_DEFAULT 100 // percent
#define SPEED_MIN 10
#define SPEED_MAX 500

enum mode { MODE_NATIVE, MODE_PASSTHROUGH, MODE_COUNT };
enum axis { AXIS_V, AXIS_H };

static const wchar_t *mode_names[MODE_COUNT] = {L"native", L"passthrough"};
static const wchar_t *mode_labels[MODE_COUNT] = {
    L"Native Windows (no changes)",
    L"Passthrough (Mac trackpad over Moonlight)",
};
static const int speeds[] = {50, 75, 100, 150, 200}; // menu presets
#define SPEED_COUNT ((int)ARRAYSIZE(speeds))

static NOTIFYICONDATAW nid;
static UINT wm_taskbar_created;
static HHOOK hook;
static enum mode mode = MODE_NATIVE;
static int speed = SPEED_DEFAULT;
static DWORD own_integrity;

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
    return MODE_NATIVE;
}

static int load_speed(void)
{
    DWORD v, size = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"Speed", RRF_RT_REG_DWORD,
                     NULL, &v, &size) == ERROR_SUCCESS &&
        v >= SPEED_MIN && v <= SPEED_MAX)
        return v;
    return SPEED_DEFAULT;
}

static BOOL save_value(const wchar_t *name, DWORD type, const void *data,
                       DWORD size)
{
    HKEY key;
    LSTATUS rc = RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0,
                                 KEY_SET_VALUE, NULL, &key, NULL);
    if (rc == ERROR_SUCCESS) {
        rc = RegSetValueExW(key, name, 0, type, data, size);
        RegCloseKey(key);
    }
    SetLastError(rc);
    return rc == ERROR_SUCCESS;
}

static BOOL save_mode(enum mode m)
{
    const wchar_t *s = mode_names[m];
    return save_value(L"Mode", REG_SZ, s, (lstrlenW(s) + 1) * sizeof *s);
}

static BOOL save_speed(int percent)
{
    DWORD v = percent;
    return save_value(L"Speed", REG_DWORD, &v, sizeof v);
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

// Moonlight caps each event at one notch, so how fast the fingers moved above
// that is lost; what survives is how long the stream stays at the cap, which
// grows with a harder flick. After a long capped run, the momentum tail that
// follows fades more slowly than the Mac's own, but never runs more than
// TAIL_MAX times ahead of it. It never speeds up, ends with the Mac's
// momentum, and stops when you touch the trackpad. Returns the delta to use.
static double stretch_tail(enum axis axis, int delta, DWORD time)
{
    static struct tail {
        DWORD last, run_start, run_end; // run_end 0: no capped run this swipe
        int prev;                       // previous delta, 0 at a swipe's start
        double held;                    // stretched size, 0 when not stretching
    } tails[2];
    struct tail *t = &tails[axis];
    DWORD dt = time - t->last;

    if (dt > SWIPE_GAP_MS || (t->prev && (delta < 0) != (t->prev < 0))) {
        t->run_end = 0; // a new swipe, or a reversal
        t->held = 0;
        t->prev = 0;
    }
    t->last = time;

    int size = abs(delta), prev = abs(t->prev);
    if (size >= WHEEL_DELTA) { // at the cap
        if (!t->run_end || time - t->run_end > DIP_MS)
            t->run_start = time;
        t->run_end = time;
        t->held = size;
    } else if (size > prev + max(6, prev / 10)) {
        t->held = 0; // rising again: that's fingers, not momentum
    } else if (t->held) {
        double run = (double)(t->run_end - t->run_start) - CAP_MIN_MS;
        double most = fmin(TAIL_MAX, 1 + TAIL_GAIN * run / 1000);
        double fade = exp(-(dt / 1000.0) / TAIL_FADE);
        t->held = most > 1 ? fmin(size * most, fmax(size, t->held * fade)) : 0;
    }
    t->prev = delta;
    return t->held > size ? copysign(t->held, delta) : delta;
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
    glide_push(axis, stretch_tail(axis, delta, ms->time) * GAIN * speed / 100);
    return TRUE;
}

static LRESULT CALLBACK mouse_hook(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && (wp == WM_MOUSEWHEEL || wp == WM_MOUSEHWHEEL)) {
        MSLLHOOKSTRUCT *ms = (MSLLHOOKSTRUCT *)lp;
        enum axis axis = wp == WM_MOUSEHWHEEL ? AXIS_H : AXIS_V;
        int delta = GET_WHEEL_DELTA_WPARAM(ms->mouseData);
        if (ms->dwExtraInfo != INJECTED_TAG && delta &&
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
              mode == MODE_PASSTHROUGH && !hook ? L" (hook failed)" : L"");
    lstrcpynW(nid.szTip, tip, ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void apply_mode(enum mode m)
{
    mode = m;
    glide_cancel();

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
    HMENU m = CreatePopupMenu(), speed_menu = CreatePopupMenu();
    for (int i = 0; i < SPEED_COUNT; i++) {
        wchar_t label[16];
        wsprintfW(label, L"%d%%", speeds[i]);
        AppendMenuW(speed_menu, MF_STRING, ID_SPEED + i, label);
    }
    for (int i = 0; i < SPEED_COUNT; i++)
        if (speeds[i] == speed)
            CheckMenuRadioItem(speed_menu, ID_SPEED, ID_SPEED + SPEED_COUNT - 1,
                               ID_SPEED + i, MF_BYCOMMAND);

    for (int i = 0; i < MODE_COUNT; i++)
        AppendMenuW(m, MF_STRING, ID_MODE + i, mode_labels[i]);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)speed_menu, L"Passthrough speed");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    CheckMenuRadioItem(m, ID_MODE, ID_MODE + MODE_COUNT - 1, ID_MODE + mode,
                       MF_BYCOMMAND);

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd); // required so the menu closes on outside click
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m); // also destroys speed_menu
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
        } else if (id >= ID_SPEED && id < ID_SPEED + SPEED_COUNT) {
            speed = speeds[id - ID_SPEED];
            save_speed(speed);
        } else if (id == ID_EXIT) {
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_RELOAD:
        speed = load_speed();
        apply_mode(load_mode());
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
    HWND hwnd = CreateWindowExW(0, APP_NAME, APP_NAME, 0, 0, 0, 0, 0,
                                NULL, NULL, inst, NULL);
    if (!hwnd)
        return 1;

    own_integrity = integrity_of(GetCurrentProcess());
    InitializeCriticalSection(&glide_lock);
    glide_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, glide_thread, NULL, 0, NULL));

    tray_add(hwnd);
    speed = load_speed();
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

static int set_speed_cli(const wchar_t *arg)
{
    wchar_t *end;
    long v = wcstol(arg, &end, 10);
    if (*end == L'%')
        end++;
    if (end == arg || *end || v < SPEED_MIN || v > SPEED_MAX) {
        fprintf(stderr, "error: speed must be a percentage from %d to %d\n",
                SPEED_MIN, SPEED_MAX);
        return 2;
    }
    if (!save_speed(v)) {
        fprintf(stderr, "error: failed to save setting (%lu)\n", GetLastError());
        return 1;
    }
    reload_tray();
    printf("%ld%%\n", v);
    return 0;
}

static int run_cli(int argc, wchar_t **argv)
{
    const wchar_t *cmd = argv[1];
    attach_console();
    for (int m = 0; m < MODE_COUNT; m++)
        if (!lstrcmpiW(cmd, mode_names[m]))
            return set_mode_cli(m);
    if (!lstrcmpiW(cmd, L"toggle"))
        return set_mode_cli(load_mode() == MODE_PASSTHROUGH ? MODE_NATIVE
                                                            : MODE_PASSTHROUGH);
    if (!lstrcmpiW(cmd, L"speed")) {
        if (argc > 2)
            return set_speed_cli(argv[2]);
        printf("%d%%\n", load_speed());
        return 0;
    }
    if (!lstrcmpiW(cmd, L"status")) {
        printf("%ls\n", mode_names[load_mode()]);
        return 0;
    }

    BOOL help = !lstrcmpiW(cmd, L"help") || !lstrcmpiW(cmd, L"-h") ||
                !lstrcmpiW(cmd, L"--help") || !lstrcmpiW(cmd, L"/?");
    fprintf(help ? stdout : stderr,
            "usage: scroll-options [native | passthrough | toggle | speed [percent] | status]\n"
            "  (no arguments)   run in the system tray\n"
            "  native           leave Windows scrolling alone\n"
            "  passthrough      Mac trackpad over Moonlight/Sunshine scrolls like on the Mac\n"
            "  toggle           switch between native and passthrough\n"
            "  speed [percent]  print or set the passthrough speed (default 100)\n"
            "  status           print current mode\n");
    return help ? 0 : 2;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)prev; (void)cmdline; (void)show;

    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int rc = (argv && argc > 1) ? run_cli(argc, argv) : run_tray(inst);
    LocalFree(argv);
    return rc;
}
