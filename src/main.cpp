// Win32 shell: main window with a video child window, a native control
// bar (play/pause, skip, seek slider, volume slider), a right-click
// context menu, drag-and-drop, and hotkeys. Standard OS controls only.
#include "player_int.h"
#include <shellapi.h>
#include <timeapi.h>
#include <commctrl.h>
#include <commdlg.h>

static Player* g_player = nullptr;
static HWND g_main = nullptr, g_video = nullptr;
static HWND g_play = nullptr, g_back = nullptr, g_fwd = nullptr;
static HWND g_seek = nullptr, g_vol = nullptr, g_full = nullptr;
static bool g_fullscreen = false;
static bool g_seek_dragging = false;
static bool g_fs_bar = true;           // bar visibility while fullscreen
static ULONGLONG g_hide_at = 0;        // tick when the fs bar auto-hides
static POINT g_last_pt = {-1, -1};
static bool g_cursor_hidden = false;
static WINDOWPLACEMENT g_saved_placement = {sizeof(WINDOWPLACEMENT)};

static const wchar_t* APP_TITLE = L"minimal-player";
enum {
    IDC_PLAY = 101, IDC_BACK, IDC_FWD, IDC_SEEK, IDC_VOL, IDC_FULL,
    IDM_OPEN = 201, IDM_PAUSE, IDM_AUDIO, IDM_SUBS, IDM_FULL, IDM_EXIT,
};
static const int BAR_H = 34;
static const int SEEK_RANGE = 1000;
static const ULONGLONG FS_HIDE_MS = 2500;

static void set_cursor_hidden(bool hide) {
    if (hide == g_cursor_hidden) return;
    ShowCursor(hide ? FALSE : TRUE);
    g_cursor_hidden = hide;
}

static void layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    // Fullscreen: video fills the client; the bar overlays it when shown.
    int barh = g_fullscreen ? 0 : BAR_H;
    int vh = rc.bottom - barh;
    MoveWindow(g_video, 0, 0, rc.right, vh > 0 ? vh : 1, TRUE);
    bool show = !g_fullscreen || g_fs_bar;
    HWND bar[] = {g_play, g_back, g_fwd, g_seek, g_vol, g_full};
    if (show) {
        int y = (g_fullscreen ? rc.bottom - BAR_H : vh) + 4, h = BAR_H - 8;
        MoveWindow(g_play, 4, y, 56, h, TRUE);
        MoveWindow(g_back, 64, y, 44, h, TRUE);
        MoveWindow(g_fwd, 112, y, 44, h, TRUE);
        int fullw = 44, volw = 110;
        int seekx = 162, seekw = rc.right - seekx - volw - fullw - 16;
        MoveWindow(g_seek, seekx, y, seekw > 40 ? seekw : 40, h, TRUE);
        MoveWindow(g_vol, rc.right - volw - fullw - 8, y, volw, h, TRUE);
        MoveWindow(g_full, rc.right - fullw - 4, y, fullw, h, TRUE);
    }
    for (HWND c : bar) {
        ShowWindow(c, show ? SW_SHOW : SW_HIDE);
        if (show) SetWindowPos(c, HWND_TOP, 0, 0, 0, 0,
                               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (g_player) player_notify_resize(g_player);
}

// Central auto-hide: runs from the UI timer; movement anywhere in the
// window shows the bar and cursor, idleness hides both (fullscreen only).
static void fs_autohide_tick(HWND hwnd) {
    if (!g_fullscreen) {
        set_cursor_hidden(false);
        return;
    }
    POINT pt;
    GetCursorPos(&pt);
    ULONGLONG now = GetTickCount64();
    bool moved = (pt.x != g_last_pt.x || pt.y != g_last_pt.y);
    g_last_pt = pt;
    if (moved) {
        g_hide_at = now + FS_HIDE_MS;
        set_cursor_hidden(false);
        if (!g_fs_bar) {
            g_fs_bar = true;
            layout(hwnd);
        }
    } else if (g_fs_bar && !g_seek_dragging && now >= g_hide_at) {
        g_fs_bar = false;
        layout(hwnd);
        if (GetForegroundWindow() == hwnd) set_cursor_hidden(true);
    }
}

static void toggle_fullscreen(HWND hwnd) {
    DWORD style = GetWindowLongW(hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi = {sizeof(MONITORINFO)};
        if (GetWindowPlacement(hwnd, &g_saved_placement) &&
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            g_fullscreen = true;
            SetWindowLongW(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    } else {
        g_fullscreen = false;
        SetWindowLongW(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &g_saved_placement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                     SWP_FRAMECHANGED);
    }
    g_fs_bar = true;
    g_hide_at = GetTickCount64() + FS_HIDE_MS;
    set_cursor_hidden(false);
    SetWindowTextW(g_full, g_fullscreen ? L"Exit FS" : L"Full");
    layout(hwnd);
}

static void update_ui(HWND hwnd) {
    wchar_t buf[512];
    bool media = g_player && player_has_media(g_player);
    bool paused = g_player && player_is_paused(g_player);
    if (media) {
        double pos = player_position(g_player);
        double dur = player_duration(g_player);
        swprintf(buf, 512, L"%02d:%02d:%02d / %02d:%02d:%02d%s — %s",
                 (int)pos / 3600, ((int)pos / 60) % 60, (int)pos % 60,
                 (int)dur / 3600, ((int)dur / 60) % 60, (int)dur % 60,
                 paused ? L"  [paused]" : L"", APP_TITLE);
        if (!g_seek_dragging && dur > 0)
            SendMessageW(g_seek, TBM_SETPOS, TRUE, (LPARAM)(pos / dur * SEEK_RANGE));
    } else {
        swprintf(buf, 512, L"%s — drop a video file here", APP_TITLE);
    }
    SetWindowTextW(hwnd, buf);
    SetWindowTextW(g_play, paused || !media ? L"Play" : L"Pause");
}

static void open_path(HWND hwnd, const wchar_t* path) {
    if (!g_player) return;
    player_open(g_player, path);
    update_ui(hwnd);
}

static void open_dialog(HWND hwnd) {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {sizeof(OPENFILENAMEW)};
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Video files\0*.mp4;*.m4v;*.mov;*.mkv;*.webm;*.avi\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) open_path(hwnd, path);
}

static void show_context_menu(HWND hwnd, int x, int y) {
    bool media = g_player && player_has_media(g_player);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_OPEN, L"Open File...");
    AppendMenuW(m, MF_STRING | (media ? 0 : MF_GRAYED), IDM_PAUSE,
                g_player && player_is_paused(g_player) ? L"Play\tSpace" : L"Pause\tSpace");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (media ? 0 : MF_GRAYED), IDM_AUDIO, L"Next Audio Track\tA");
    AppendMenuW(m, MF_STRING | (media ? 0 : MF_GRAYED), IDM_SUBS, L"Next Subtitle Track\tS");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_fullscreen ? MF_CHECKED : 0), IDM_FULL, L"Fullscreen\tF");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit\tQ");
    TrackPopupMenu(m, TPM_RIGHTBUTTON, x, y, 0, hwnd, nullptr);
    DestroyMenu(m);
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
        case 'O': open_dialog(hwnd); break;
        case VK_ESCAPE:
            if (g_fullscreen) toggle_fullscreen(hwnd);
            break;
        case 'Q': PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
    }
    if (g_player) {
        SendMessageW(g_vol, TBM_SETPOS, TRUE, (LPARAM)(player_volume(g_player) * 100));
    }
    update_ui(hwnd);
}

static LRESULT CALLBACK video_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_LBUTTONDBLCLK:
        case WM_DROPFILES:
        case WM_KEYDOWN:
            return SendMessageW(GetParent(hwnd), msg, wp, lp);
        case WM_RBUTTONUP: {
            POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
            ClientToScreen(hwnd, &pt);
            show_context_menu(GetParent(hwnd), pt.x, pt.y);
            return 0;
        }
        case WM_LBUTTONDOWN:
            SetFocus(GetParent(hwnd));
            return 0;
        case WM_ERASEBKGND:
            return 1;  // D3D owns this surface
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_KEYDOWN:
            on_key(hwnd, wp);
            return 0;
        case WM_LBUTTONDBLCLK:
            toggle_fullscreen(hwnd);
            return 0;
        case WM_CONTEXTMENU:
            if ((HWND)wp == hwnd)
                show_context_menu(hwnd, (short)LOWORD(lp), (short)HIWORD(lp));
            return 0;
        case WM_SIZE:
            layout(hwnd);
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_PLAY:
                case IDM_PAUSE: if (g_player) player_toggle_pause(g_player); break;
                case IDC_BACK: if (g_player) player_seek_rel(g_player, -10); break;
                case IDC_FWD: if (g_player) player_seek_rel(g_player, 10); break;
                case IDC_FULL: toggle_fullscreen(hwnd); break;
                case IDM_OPEN: open_dialog(hwnd); break;
                case IDM_AUDIO: if (g_player) player_cycle_audio(g_player); break;
                case IDM_SUBS: if (g_player) player_cycle_subtitle(g_player); break;
                case IDM_FULL: toggle_fullscreen(hwnd); break;
                case IDM_EXIT: PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
            }
            SetFocus(hwnd);  // keep hotkeys working after button clicks
            update_ui(hwnd);
            return 0;
        case WM_HSCROLL: {
            HWND src = (HWND)lp;
            int code = LOWORD(wp);
            if (src == g_seek && g_player) {
                if (code == TB_THUMBTRACK) g_seek_dragging = true;
                if (code == TB_ENDTRACK || code == TB_THUMBPOSITION) {
                    double dur = player_duration(g_player);
                    if (dur > 0) {
                        LRESULT pos = SendMessageW(g_seek, TBM_GETPOS, 0, 0);
                        player_seek_to(g_player, (double)pos / SEEK_RANGE * dur);
                    }
                    g_seek_dragging = false;
                    SetFocus(hwnd);
                }
            } else if (src == g_vol && g_player) {
                LRESULT pos = SendMessageW(g_vol, TBM_GETPOS, 0, 0);
                player_volume_set(g_player, (float)pos / 100.0f);
                if (code == TB_ENDTRACK) SetFocus(hwnd);
            }
            return 0;
        }
        case WM_DROPFILES: {
            wchar_t path[MAX_PATH];
            if (DragQueryFileW((HDROP)wp, 0, path, MAX_PATH)) open_path(hwnd, path);
            DragFinish((HDROP)wp);
            return 0;
        }
        case WM_TIMER:
            fs_autohide_tick(hwnd);
            update_ui(hwnd);
            return 0;
        case WM_APP_PLAYER_ERROR:
            if (g_player)
                MessageBoxW(hwnd, player_error(g_player), APP_TITLE,
                            MB_OK | MB_ICONERROR);
            update_ui(hwnd);
            return 0;
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
    INITCOMMONCONTROLSEX icc = {sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSW wc = {};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"minimal_player_wnd";
    RegisterClassW(&wc);

    WNDCLASSW vc = {};
    vc.style = CS_DBLCLKS;
    vc.lpfnWndProc = video_proc;
    vc.hInstance = hinst;
    vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    vc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    vc.lpszClassName = L"minimal_player_video";
    RegisterClassW(&vc);

    g_main = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, APP_TITLE,
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             1280, 720 + BAR_H, nullptr, nullptr, hinst, nullptr);
    if (!g_main) return 1;
    g_video = CreateWindowExW(WS_EX_ACCEPTFILES, vc.lpszClassName, nullptr,
                              WS_CHILD | WS_VISIBLE, 0, 0, 100, 100,
                              g_main, nullptr, hinst, nullptr);

    g_play = CreateWindowExW(0, L"BUTTON", L"Play", WS_CHILD | WS_VISIBLE,
                             0, 0, 0, 0, g_main, (HMENU)IDC_PLAY, hinst, nullptr);
    g_back = CreateWindowExW(0, L"BUTTON", L"-10s", WS_CHILD | WS_VISIBLE,
                             0, 0, 0, 0, g_main, (HMENU)IDC_BACK, hinst, nullptr);
    g_fwd = CreateWindowExW(0, L"BUTTON", L"+10s", WS_CHILD | WS_VISIBLE,
                            0, 0, 0, 0, g_main, (HMENU)IDC_FWD, hinst, nullptr);
    g_seek = CreateWindowExW(0, TRACKBAR_CLASSW, nullptr,
                             WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                             0, 0, 0, 0, g_main, (HMENU)IDC_SEEK, hinst, nullptr);
    g_vol = CreateWindowExW(0, TRACKBAR_CLASSW, nullptr,
                            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                            0, 0, 0, 0, g_main, (HMENU)IDC_VOL, hinst, nullptr);
    g_full = CreateWindowExW(0, L"BUTTON", L"Full", WS_CHILD | WS_VISIBLE,
                             0, 0, 0, 0, g_main, (HMENU)IDC_FULL, hinst, nullptr);
    SendMessageW(g_seek, TBM_SETRANGE, TRUE, MAKELPARAM(0, SEEK_RANGE));
    SendMessageW(g_vol, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(g_vol, TBM_SETPOS, TRUE, 100);

    g_player = player_create(g_video);
    if (!g_player) {
        wchar_t msg[512];
        swprintf(msg, 512, L"Failed to initialize D3D11 video output.\n\n%s",
                 vo_init_error());
        MessageBoxW(g_main, msg, APP_TITLE, MB_OK | MB_ICONERROR);
        return 1;
    }
    SendMessageW(g_vol, TBM_SETPOS, TRUE, (LPARAM)(player_volume(g_player) * 100));

    layout(g_main);
    ShowWindow(g_main, show);
    SetTimer(g_main, 1, 250, nullptr);
    update_ui(g_main);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) open_path(g_main, argv[1]);
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
