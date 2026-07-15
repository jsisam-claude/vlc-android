// Win32 shell: one window, drag-and-drop, hotkeys. All playback goes
// through the engine's C API in player.h.
#include "player_int.h"
#include <shellapi.h>
#include <timeapi.h>

static Player* g_player = nullptr;
static bool g_fullscreen = false;
static WINDOWPLACEMENT g_saved_placement = {sizeof(WINDOWPLACEMENT)};

static const wchar_t* APP_TITLE = L"minimal-player";

static void toggle_fullscreen(HWND hwnd) {
    DWORD style = GetWindowLongW(hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi = {sizeof(MONITORINFO)};
        if (GetWindowPlacement(hwnd, &g_saved_placement) &&
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongW(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_fullscreen = true;
        }
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &g_saved_placement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                     SWP_FRAMECHANGED);
        g_fullscreen = false;
    }
}

static void update_title(HWND hwnd) {
    wchar_t buf[512];
    if (g_player && player_has_media(g_player)) {
        double pos = player_position(g_player);
        double dur = player_duration(g_player);
        const wchar_t* paused = player_is_paused(g_player) ? L"  [paused]" : L"";
        swprintf(buf, 512, L"%02d:%02d:%02d / %02d:%02d:%02d%s — %s",
                 (int)pos / 3600, ((int)pos / 60) % 60, (int)pos % 60,
                 (int)dur / 3600, ((int)dur / 60) % 60, (int)dur % 60,
                 paused, APP_TITLE);
    } else {
        swprintf(buf, 512, L"%s — drop a video file here", APP_TITLE);
    }
    SetWindowTextW(hwnd, buf);
}

static void open_path(HWND hwnd, const wchar_t* path) {
    if (!g_player) return;
    player_open(g_player, path);
    update_title(hwnd);
}

static void on_key(HWND hwnd, WPARAM key) {
    if (!g_player) return;
    switch (key) {
        case VK_SPACE: player_toggle_pause(g_player); break;
        case VK_LEFT: player_seek_rel(g_player, -10); break;
        case VK_RIGHT: player_seek_rel(g_player, 10); break;
        case VK_PRIOR: player_seek_rel(g_player, -60); break;
        case VK_NEXT: player_seek_rel(g_player, 60); break;
        case VK_UP: player_volume_step(g_player, 1); break;
        case VK_DOWN: player_volume_step(g_player, -1); break;
        case 'A': player_cycle_audio(g_player); break;
        case 'S': player_cycle_subtitle(g_player); break;
        case 'F': toggle_fullscreen(hwnd); break;
        case VK_ESCAPE:
            if (g_fullscreen) toggle_fullscreen(hwnd);
            break;
        case 'Q': PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
    }
    update_title(hwnd);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_KEYDOWN:
            on_key(hwnd, wp);
            return 0;
        case WM_LBUTTONDBLCLK:
            toggle_fullscreen(hwnd);
            return 0;
        case WM_SIZE:
            if (g_player) player_notify_resize(g_player);
            return 0;
        case WM_DROPFILES: {
            wchar_t path[MAX_PATH];
            if (DragQueryFileW((HDROP)wp, 0, path, MAX_PATH)) open_path(hwnd, path);
            DragFinish((HDROP)wp);
            return 0;
        }
        case WM_TIMER:
            update_title(hwnd);
            return 0;
        case WM_APP_PLAYER_ERROR:
            if (g_player)
                MessageBoxW(hwnd, player_error(g_player), APP_TITLE,
                            MB_OK | MB_ICONERROR);
            update_title(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;  // D3D owns the client area
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void av_log_to_debugger(void*, int level, const char* fmt, va_list args) {
    if (level > AV_LOG_WARNING) return;
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    OutputDebugStringA(buf);
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int show) {
    // When launched from a console, show our log lines there.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    timeBeginPeriod(1);
    av_log_set_callback(av_log_to_debugger);

    WNDCLASSW wc = {};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"minimal_player_wnd";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, APP_TITLE,
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                1280, 720, nullptr, nullptr, hinst, nullptr);
    if (!hwnd) return 1;

    g_player = player_create(hwnd);
    if (!g_player) {
        wchar_t msg[512];
        swprintf(msg, 512, L"Failed to initialize D3D11 video output.\n\n%s",
                 vo_init_error());
        MessageBoxW(hwnd, msg, APP_TITLE, MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, show);
    SetTimer(hwnd, 1, 500, nullptr);
    update_title(hwnd);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) open_path(hwnd, argv[1]);
    if (argv) LocalFree(argv);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    player_destroy(g_player);
    g_player = nullptr;
    timeEndPeriod(1);
    CoUninitialize();
    return 0;
}
