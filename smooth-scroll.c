// smooth-scroll: tray utility that switches Windows between smooth and
// discrete (native, line-by-line) scrolling.
//
// The toggle is the system-wide "Smooth-scroll list boxes" setting
// (SPI_SETLISTBOXSMOOTHSCROLLING), persisted to the user profile.
//
//   smooth-scroll.exe            run in the system tray
//   smooth-scroll.exe smooth     enable smooth scrolling
//   smooth-scroll.exe discrete   enable discrete scrolling
//   smooth-scroll.exe toggle     switch between the two
//   smooth-scroll.exe status     print current mode

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>

#define APP_NAME L"smooth-scroll"
#define WM_TRAY (WM_APP + 1)
#define ID_SMOOTH 1
#define ID_DISCRETE 2
#define ID_EXIT 3

static NOTIFYICONDATAW nid;
static UINT wm_taskbar_created;

static BOOL get_smooth(void)
{
    BOOL on = FALSE;
    SystemParametersInfoW(SPI_GETLISTBOXSMOOTHSCROLLING, 0, &on, 0);
    return on;
}

static BOOL set_smooth(BOOL on)
{
    return SystemParametersInfoW(SPI_SETLISTBOXSMOOTHSCROLLING, 0,
                                 (PVOID)(INT_PTR)on,
                                 SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
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

static int apply(BOOL smooth)
{
    if (!set_smooth(smooth)) {
        fprintf(stderr, "error: failed to change setting (%lu)\n", GetLastError());
        return 1;
    }
    printf("%s\n", smooth ? "smooth" : "discrete");
    return 0;
}

static int run_cli(const wchar_t *cmd)
{
    attach_console();
    if (!lstrcmpiW(cmd, L"smooth"))
        return apply(TRUE);
    if (!lstrcmpiW(cmd, L"discrete"))
        return apply(FALSE);
    if (!lstrcmpiW(cmd, L"toggle"))
        return apply(!get_smooth());
    if (!lstrcmpiW(cmd, L"status")) {
        printf("%s\n", get_smooth() ? "smooth" : "discrete");
        return 0;
    }

    BOOL help = !lstrcmpiW(cmd, L"help") || !lstrcmpiW(cmd, L"-h") ||
                !lstrcmpiW(cmd, L"--help") || !lstrcmpiW(cmd, L"/?");
    fprintf(help ? stdout : stderr,
            "usage: smooth-scroll [smooth | discrete | toggle | status]\n"
            "  (no arguments)  run in the system tray\n"
            "  smooth          enable smooth scrolling\n"
            "  discrete        enable discrete scrolling\n"
            "  toggle          switch between the two\n"
            "  status          print current mode\n");
    return help ? 0 : 2;
}

// ---- tray -----------------------------------------------------------------

static void tray_update(void)
{
    lstrcpynW(nid.szTip, get_smooth() ? L"Scrolling: smooth" : L"Scrolling: discrete",
              ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
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
    AppendMenuW(m, MF_STRING, ID_SMOOTH, L"Smooth scrolling");
    AppendMenuW(m, MF_STRING, ID_DISCRETE, L"Discrete scrolling");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    CheckMenuRadioItem(m, ID_SMOOTH, ID_DISCRETE,
                       get_smooth() ? ID_SMOOTH : ID_DISCRETE, MF_BYCOMMAND);

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
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_SMOOTH:   set_smooth(TRUE);  tray_update(); break;
        case ID_DISCRETE: set_smooth(FALSE); tray_update(); break;
        case ID_EXIT:     DestroyWindow(hwnd); break;
        }
        return 0;
    case WM_SETTINGCHANGE: // also fires when the CLI changes the setting
        tray_update();
        return 0;
    case WM_DESTROY:
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
    CreateMutexW(NULL, TRUE, L"Local\\smooth-scroll-tray");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    wm_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = APP_NAME;
    RegisterClassW(&wc);

    // Hidden top-level window (not message-only) so it receives the
    // WM_SETTINGCHANGE broadcast.
    HWND hwnd = CreateWindowExW(0, APP_NAME, APP_NAME, 0, 0, 0, 0, 0,
                                NULL, NULL, inst, NULL);
    if (!hwnd)
        return 1;
    tray_add(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
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
