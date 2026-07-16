// Win32 shell: main window with a video child window, a native control
// bar (play/pause, skip, seek slider, volume slider), a right-click
// context menu, drag-and-drop, and hotkeys. Standard OS controls only.
#include "player.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <cstdio>
#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
    IDM_OPEN = 201, IDM_PAUSE, IDM_AUDIO, IDM_SUBS, IDM_FULL, IDM_MUTE,
    IDM_NEXTFILE, IDM_PREVFILE, IDM_AUTONEXT, IDM_EXIT,
    IDM_STRACK_OFF = 299, IDM_ATRACK_BASE = 300, IDM_STRACK_BASE = 400,
    IDM_CHAP_BASE = 500,  // ..563
};
#define MSG_PLAYER_EVENT (WM_APP + 1)
#define MSG_PREVIEW_READY (WM_APP + 2)

// Engine events arrive on engine threads; bounce them to the UI thread.
static void on_player_event(void* user, PlayerEvent evt) {
    PostMessageW((HWND)user, MSG_PLAYER_EVENT, (WPARAM)evt, 0);
}

// ------------------------- persisted state (resume, window, options) ----
static std::wstring g_cur_path;
static std::vector<std::wstring> g_siblings;   // video files in current folder
static int g_sib_cur = -1;
static std::map<std::wstring, double> g_resume;
static std::map<std::wstring, std::pair<int, int>> g_track_mem;  // audio, sub
static double g_loop_a = -1, g_loop_b = -1;  // A-B loop (seconds; <0 unset)
static bool g_autonext = false;
static bool g_have_placement = false;
static WINDOWPLACEMENT g_loaded_placement = {sizeof(WINDOWPLACEMENT)};
static int g_loaded_vol = -1;   // 0..100, -1 = not in state file
static int g_loaded_mute = 0;

static bool is_video_ext(const wchar_t* path) {
    const wchar_t* dot = wcsrchr(path, L'.');
    if (!dot) return false;
    for (const wchar_t* e : {L".mp4", L".m4v", L".mov", L".mkv", L".webm", L".avi"})
        if (!_wcsicmp(dot, e)) return true;
    return false;
}

static std::wstring state_path() {
    wchar_t dir[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, dir))) return L"";
    std::wstring d = std::wstring(dir) + L"\\minimal-player";
    CreateDirectoryW(d.c_str(), nullptr);
    return d + L"\\state.txt";
}

static void save_state(HWND hwnd) {
    std::wstring sp = state_path();
    if (sp.empty()) return;
    FILE* f = _wfopen(sp.c_str(), L"w, ccs=UTF-8");
    if (!f) return;
    fwprintf(f, L"A|%d\n", g_autonext ? 1 : 0);
    if (g_player)
        fwprintf(f, L"V|%d|%d\n", (int)(player_volume(g_player) * 100 + 0.5f),
                 player_is_muted(g_player) ? 1 : 0);
    int nk = 0;
    for (auto& kv : g_track_mem) {
        if (++nk > 500) break;
        fwprintf(f, L"K|%d,%d|%s\n", kv.second.first, kv.second.second,
                 kv.first.c_str());
    }
    WINDOWPLACEMENT wp = {sizeof(WINDOWPLACEMENT)};
    if (hwnd && GetWindowPlacement(hwnd, &wp))
        fwprintf(f, L"W|%ld,%ld,%ld,%ld,%u\n",
                 wp.rcNormalPosition.left, wp.rcNormalPosition.top,
                 wp.rcNormalPosition.right, wp.rcNormalPosition.bottom,
                 wp.showCmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    int n = 0;
    for (auto& kv : g_resume) {
        if (++n > 500) break;  // keep the file bounded
        fwprintf(f, L"R|%.1f|%s\n", kv.second, kv.first.c_str());
    }
    fclose(f);
}

static void load_state() {
    std::wstring sp = state_path();
    if (sp.empty()) return;
    FILE* f = _wfopen(sp.c_str(), L"r, ccs=UTF-8");
    if (!f) return;
    wchar_t line[1200];
    while (fgetws(line, 1200, f)) {
        size_t len = wcslen(line);
        while (len && (line[len - 1] == L'\n' || line[len - 1] == L'\r')) line[--len] = 0;
        if (line[0] == L'A' && line[1] == L'|') {
            g_autonext = line[2] == L'1';
        } else if (line[0] == L'W' && line[1] == L'|') {
            WINDOWPLACEMENT& wp = g_loaded_placement;
            unsigned cmd = SW_SHOWNORMAL;
            if (swscanf(line + 2, L"%ld,%ld,%ld,%ld,%u",
                        &wp.rcNormalPosition.left, &wp.rcNormalPosition.top,
                        &wp.rcNormalPosition.right, &wp.rcNormalPosition.bottom,
                        &cmd) == 5) {
                wp.showCmd = cmd;
                g_have_placement = true;
            }
        } else if (line[0] == L'V' && line[1] == L'|') {
            int v = -1, m = 0;
            if (swscanf(line + 2, L"%d|%d", &v, &m) >= 1 && v >= 0 && v <= 100) {
                g_loaded_vol = v;
                g_loaded_mute = m;
            }
        } else if (line[0] == L'R' && line[1] == L'|') {
            wchar_t* bar = wcschr(line + 2, L'|');
            if (bar) {
                *bar = 0;
                g_resume[bar + 1] = _wtof(line + 2);
            }
        } else if (line[0] == L'K' && line[1] == L'|') {
            wchar_t* bar = wcschr(line + 2, L'|');
            int a = 0, s = 0;
            if (bar && swscanf(line + 2, L"%d,%d", &a, &s) == 2) {
                *bar = 0;
                g_track_mem[bar + 1] = {a, s};
            }
        }
    }
    fclose(f);
}

static void remember_position() {
    if (g_cur_path.empty() || !g_player) return;
    double pos = player_position(g_player), dur = player_duration(g_player);
    if (pos > 15 && dur > 0 && pos < dur * 0.95 && !player_media_ended(g_player))
        g_resume[g_cur_path] = pos;
    else
        g_resume.erase(g_cur_path);
}

// Called after any explicit track change so the choice sticks per file.
static void remember_tracks() {
    if (g_cur_path.empty() || !g_player || !player_has_media(g_player)) return;
    g_track_mem[g_cur_path] = {player_audio_track_current(g_player),
                               player_sub_track_current(g_player)};
}

static void build_siblings(const wchar_t* path) {
    g_siblings.clear();
    g_sib_cur = -1;
    std::wstring dir(path);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    dir.resize(slash);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && is_video_ext(fd.cFileName))
            g_siblings.push_back(dir + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(g_siblings.begin(), g_siblings.end(),
              [](const std::wstring& a, const std::wstring& b) {
                  return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
              });
    for (size_t i = 0; i < g_siblings.size(); i++)
        if (!_wcsicmp(g_siblings[i].c_str(), path)) { g_sib_cur = (int)i; break; }
}

// ----------------------------------------------- seek-bar hover preview
// A worker thread decodes one frame per hovered position (48 buckets per
// file, cached as HBITMAPs); a no-activate popup above the bar shows it.
static HWND g_preview = nullptr;
static const int PV_W = 200, PV_H = 120, PV_TEXT = 18;
static std::map<int, HBITMAP> g_pv_cache;   // bucket -> bitmap, current file
static int g_pv_bucket = -1;                // bucket the popup is showing
static double g_pv_time = 0;
static std::thread g_thumb_thread;
static std::mutex g_thumb_m;
static std::condition_variable g_thumb_cv;
static bool g_thumb_quit = false;
static std::wstring g_thumb_path;
static double g_thumb_at = -1;              // <0 = no pending request
static int g_thumb_bucket = 0, g_thumb_gen = 0;

struct PvResult {
    HBITMAP bmp;
    int w, h, bucket, gen;
};

static void thumb_worker() {
    for (;;) {
        std::wstring path;
        double at;
        int gen, bucket;
        {
            std::unique_lock<std::mutex> lk(g_thumb_m);
            g_thumb_cv.wait(lk, [] { return g_thumb_quit || g_thumb_at >= 0; });
            if (g_thumb_quit) return;
            path = g_thumb_path;
            at = g_thumb_at;
            gen = g_thumb_gen;
            bucket = g_thumb_bucket;
            g_thumb_at = -1;
        }
        std::vector<uint8_t> buf((size_t)PV_W * PV_H * 4);
        int w = 0, h = 0;
        if (!player_extract_thumb_at(path.c_str(), at, PV_W, PV_H,
                                     buf.data(), &w, &h))
            continue;
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp) continue;
        memcpy(bits, buf.data(), (size_t)w * h * 4);
        PvResult* r = new PvResult{bmp, w, h, bucket, gen};
        if (!PostMessageW(g_main, MSG_PREVIEW_READY, 0, (LPARAM)r)) {
            DeleteObject(bmp);
            delete r;
        }
    }
}

static void preview_clear_cache() {
    for (auto& kv : g_pv_cache) DeleteObject(kv.second);
    g_pv_cache.clear();
    std::lock_guard<std::mutex> lk(g_thumb_m);
    g_thumb_gen++;
    g_thumb_at = -1;
}

static void preview_hide() {
    if (g_preview) ShowWindow(g_preview, SW_HIDE);
    g_pv_bucket = -1;
}

static LRESULT CALLBACK preview_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        auto it = g_pv_cache.find(g_pv_bucket);
        if (it != g_pv_cache.end()) {
            BITMAP bm;
            GetObjectW(it->second, sizeof(bm), &bm);
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mem, it->second);
            int x = (PV_W - bm.bmWidth) / 2 + 1;
            int y = (PV_H - bm.bmHeight) / 2 + 1;
            BitBlt(dc, x, y, bm.bmWidth, bm.bmHeight, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
        }
        wchar_t t[32];
        int s = (int)g_pv_time;
        swprintf(t, 32, L"%02d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT tr = {0, PV_H + 2, PV_W + 2, PV_H + PV_TEXT};
        DrawTextW(dc, t, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void preview_hover(HWND bar, int x) {
    if (!g_player || !player_has_media(g_player) || g_cur_path.empty()) return;
    double dur = player_duration(g_player);
    if (dur <= 0) return;

    RECT ch = {};
    SendMessageW(bar, TBM_GETCHANNELRECT, 0, (LPARAM)&ch);
    int cw = ch.right - ch.left;
    if (cw <= 0) return;
    double frac = (double)(x - ch.left) / cw;
    frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
    int bucket = (int)(frac * 47.999);
    g_pv_time = dur * (bucket + 0.5) / 48.0;

    if (g_pv_cache.find(bucket) == g_pv_cache.end()) {
        std::lock_guard<std::mutex> lk(g_thumb_m);
        g_thumb_path = g_cur_path;
        g_thumb_at = g_pv_time;
        g_thumb_bucket = bucket;
        g_thumb_cv.notify_one();
    }
    g_pv_bucket = bucket;

    POINT pt = {x, 0};
    ClientToScreen(bar, &pt);
    RECT brc;
    GetWindowRect(bar, &brc);
    int w = PV_W + 2, h = PV_H + PV_TEXT + 2;
    int px = pt.x - w / 2;
    int py = brc.top - h - 8;
    SetWindowPos(g_preview, HWND_TOPMOST, px, py, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_preview, nullptr, FALSE);
}

static LRESULT CALLBACK seek_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                      UINT_PTR, DWORD_PTR) {
    if (msg == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        preview_hover(hwnd, (short)LOWORD(lp));
    } else if (msg == WM_MOUSELEAVE) {
        preview_hide();
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void osd(const wchar_t* s) {
    if (g_player) player_show_osd(g_player, s, 1.5);
}

static void osd_volume() {
    if (!g_player) return;
    wchar_t b[64];
    if (player_is_muted(g_player))
        wcscpy(b, L"Muted");
    else
        swprintf(b, 64, L"Volume %d%%", (int)(player_volume(g_player) * 100 + 0.5f));
    osd(b);
}

static void play_pause() {
    if (!g_player) return;
    if (player_media_ended(g_player)) {
        player_seek_to(g_player, 0);  // restart from the top after the end
        if (player_is_paused(g_player)) player_toggle_pause(g_player);
        osd(L"Playing");
    } else {
        player_toggle_pause(g_player);
        osd(player_is_paused(g_player) ? L"Paused" : L"Playing");
    }
}
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
    bool ended = g_player && player_media_ended(g_player);
    if (media) {
        double pos = player_position(g_player);
        double dur = player_duration(g_player);
        swprintf(buf, 512, L"%02d:%02d:%02d / %02d:%02d:%02d%s — %s",
                 (int)pos / 3600, ((int)pos / 60) % 60, (int)pos % 60,
                 (int)dur / 3600, ((int)dur / 60) % 60, (int)dur % 60,
                 ended ? L"  [ended]" : paused ? L"  [paused]" : L"", APP_TITLE);
        if (!g_seek_dragging && dur > 0)
            SendMessageW(g_seek, TBM_SETPOS, TRUE,
                         ended ? SEEK_RANGE : (LPARAM)(pos / dur * SEEK_RANGE));
    } else {
        swprintf(buf, 512, L"%s — drop a video file here", APP_TITLE);
    }
    SetWindowTextW(hwnd, buf);
    SetWindowTextW(g_play, paused || ended || !media ? L"Play" : L"Pause");
}

static void open_path(HWND hwnd, const wchar_t* path) {
    if (!g_player) return;
    remember_position();
    g_cur_path = path;
    build_siblings(path);
    preview_hide();
    preview_clear_cache();
    g_loop_a = g_loop_b = -1;
    player_open(g_player, path);
    update_ui(hwnd);
}

static void nav_folder(HWND hwnd, int step) {
    if (g_siblings.empty()) return;
    int n = (int)g_siblings.size();
    int idx = g_sib_cur < 0 ? 0 : (g_sib_cur + step % n + n) % n;
    std::wstring next = g_siblings[idx];  // copy: open_path rebuilds the list
    open_path(hwnd, next.c_str());
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
    AppendMenuW(m, MF_STRING | (g_siblings.size() > 1 ? 0 : MF_GRAYED),
                IDM_NEXTFILE, L"Next in Folder\tN");
    AppendMenuW(m, MF_STRING | (g_siblings.size() > 1 ? 0 : MF_GRAYED),
                IDM_PREVFILE, L"Previous in Folder\tP");
    AppendMenuW(m, MF_STRING | (g_autonext ? MF_CHECKED : 0), IDM_AUTONEXT,
                L"Autoplay Next");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    int ac = media ? player_audio_track_count(g_player) : 0;
    if (ac > 0) {
        HMENU am = CreatePopupMenu();
        int cur = player_audio_track_current(g_player);
        for (int i = 0; i < ac && i < 32; i++) {
            wchar_t nm[128];
            player_audio_track_name(g_player, i, nm, 128);
            AppendMenuW(am, MF_STRING | (i == cur ? MF_CHECKED : 0),
                        IDM_ATRACK_BASE + i, nm);
        }
        AppendMenuW(m, MF_POPUP, (UINT_PTR)am, L"Audio Track\tA");
    }
    int sc = media ? player_sub_track_count(g_player) : 0;
    if (sc > 0) {
        HMENU sm = CreatePopupMenu();
        int cur = player_sub_track_current(g_player);
        AppendMenuW(sm, MF_STRING | (cur < 0 ? MF_CHECKED : 0), IDM_STRACK_OFF, L"Off");
        for (int i = 0; i < sc && i < 32; i++) {
            wchar_t nm[128];
            player_sub_track_name(g_player, i, nm, 128);
            AppendMenuW(sm, MF_STRING | (i == cur ? MF_CHECKED : 0),
                        IDM_STRACK_BASE + i, nm);
        }
        AppendMenuW(m, MF_POPUP, (UINT_PTR)sm, L"Subtitles\tS");
    }
    int cc = media ? player_chapter_count(g_player) : 0;
    if (cc > 0) {
        HMENU cm = CreatePopupMenu();
        int cur = player_chapter_current(g_player);
        for (int i = 0; i < cc && i < 64; i++) {
            wchar_t nm[128], item[176];
            player_chapter_name(g_player, i, nm, 128);
            double st = player_chapter_start(g_player, i);
            swprintf(item, 176, L"%s\t%02d:%02d:%02d", nm,
                     (int)st / 3600, ((int)st / 60) % 60, (int)st % 60);
            AppendMenuW(cm, MF_STRING | (i == cur ? MF_CHECKED : 0),
                        IDM_CHAP_BASE + i, item);
        }
        AppendMenuW(m, MF_POPUP, (UINT_PTR)cm, L"Chapters\tCtrl+PgUp/PgDn");
    }
    if (ac > 0 || sc > 0 || cc > 0) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_fullscreen ? MF_CHECKED : 0), IDM_FULL, L"Fullscreen\tF");
    AppendMenuW(m, MF_STRING | (g_player && player_is_muted(g_player) ? MF_CHECKED : 0),
                IDM_MUTE, L"Mute\tM");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit\tQ");
    TrackPopupMenu(m, TPM_RIGHTBUTTON, x, y, 0, hwnd, nullptr);
    DestroyMenu(m);
}

static void on_key(HWND hwnd, WPARAM key) {
    if (!g_player) return;
    switch (key) {
        case VK_SPACE: play_pause(); break;
        case 'M':
            player_set_mute(g_player, !player_is_muted(g_player));
            osd_volume();
            break;
        case VK_OEM_PERIOD: player_frame_step(g_player); osd(L"Frame step"); break;
        case VK_LEFT: player_seek_rel(g_player, -10); osd(L"-10s"); break;
        case VK_RIGHT: player_seek_rel(g_player, 10); osd(L"+10s"); break;
        case VK_PRIOR:
        case VK_NEXT: {
            int dir = (key == VK_NEXT) ? 1 : -1;
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                int i = player_chapter_seek(g_player, dir);
                if (i >= 0) {
                    wchar_t nm[128];
                    player_chapter_name(g_player, i, nm, 128);
                    osd(nm);
                } else {
                    osd(L"No chapters");
                }
            } else {
                player_seek_rel(g_player, 60.0 * dir);
                osd(dir > 0 ? L"+60s" : L"-60s");
            }
            break;
        }
        case VK_UP: player_volume_step(g_player, 1); osd_volume(); break;
        case VK_DOWN: player_volume_step(g_player, -1); osd_volume(); break;
        case 'A': player_cycle_audio(g_player); remember_tracks(); break;
        case 'S': player_cycle_subtitle(g_player); remember_tracks(); break;
        case 'L':
            if (g_loop_a < 0) {
                g_loop_a = player_position(g_player);
                osd(L"Loop: A set");
            } else if (g_loop_b < 0) {
                g_loop_b = player_position(g_player);
                if (g_loop_b <= g_loop_a + 0.2) {
                    g_loop_a = g_loop_b = -1;
                    osd(L"Loop: off");
                } else {
                    osd(L"Loop: A–B on");
                }
            } else {
                g_loop_a = g_loop_b = -1;
                osd(L"Loop: off");
            }
            break;
        case 'N': nav_folder(hwnd, 1); break;
        case 'P': nav_folder(hwnd, -1); break;
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
                case IDM_PAUSE: play_pause(); break;
                case IDM_MUTE:
                    if (g_player) {
                        player_set_mute(g_player, !player_is_muted(g_player));
                        osd_volume();
                    }
                    break;
                case IDC_BACK: if (g_player) player_seek_rel(g_player, -10); break;
                case IDC_FWD: if (g_player) player_seek_rel(g_player, 10); break;
                case IDC_FULL: toggle_fullscreen(hwnd); break;
                case IDM_OPEN: open_dialog(hwnd); break;
                case IDM_NEXTFILE: nav_folder(hwnd, 1); break;
                case IDM_PREVFILE: nav_folder(hwnd, -1); break;
                case IDM_AUTONEXT: g_autonext = !g_autonext; break;
                case IDM_STRACK_OFF:
                    if (g_player) {
                        player_select_sub_track(g_player, -1);
                        remember_tracks();
                    }
                    break;
                default:
                    if (g_player && LOWORD(wp) >= IDM_ATRACK_BASE &&
                        LOWORD(wp) < IDM_ATRACK_BASE + 32) {
                        player_select_audio_track(g_player, LOWORD(wp) - IDM_ATRACK_BASE);
                        remember_tracks();
                    } else if (g_player && LOWORD(wp) >= IDM_STRACK_BASE &&
                               LOWORD(wp) < IDM_STRACK_BASE + 32) {
                        player_select_sub_track(g_player, LOWORD(wp) - IDM_STRACK_BASE);
                        remember_tracks();
                    }
                    else if (g_player && LOWORD(wp) >= IDM_CHAP_BASE &&
                             LOWORD(wp) < IDM_CHAP_BASE + 64)
                        player_chapter_go(g_player, LOWORD(wp) - IDM_CHAP_BASE);
                    break;
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
                if (code == TB_THUMBTRACK) {
                    g_seek_dragging = true;
                    static ULONGLONG last_live = 0;  // throttled live seek
                    ULONGLONG now = GetTickCount64();
                    if (now - last_live > 250) {
                        last_live = now;
                        double dur = player_duration(g_player);
                        if (dur > 0) {
                            LRESULT pos = SendMessageW(g_seek, TBM_GETPOS, 0, 0);
                            player_seek_to(g_player, (double)pos / SEEK_RANGE * dur);
                        }
                    }
                }
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
                osd_volume();
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
            if (g_player && g_loop_b > 0 && player_position(g_player) > g_loop_b)
                player_seek_to(g_player, g_loop_a > 0 ? g_loop_a : 0);
            update_ui(hwnd);
            return 0;
        case MSG_PREVIEW_READY: {
            PvResult* r = (PvResult*)lp;
            bool current;
            {
                std::lock_guard<std::mutex> lk(g_thumb_m);
                current = r->gen == g_thumb_gen;
            }
            if (current) {
                auto it = g_pv_cache.find(r->bucket);
                if (it != g_pv_cache.end()) DeleteObject(it->second);
                g_pv_cache[r->bucket] = r->bmp;
                if (g_pv_bucket == r->bucket && g_preview &&
                    IsWindowVisible(g_preview))
                    InvalidateRect(g_preview, nullptr, FALSE);
            } else {
                DeleteObject(r->bmp);
            }
            delete r;
            return 0;
        }
        case MSG_PLAYER_EVENT:
            if ((PlayerEvent)wp == PLAYER_EVT_ERROR && g_player) {
                wchar_t err[512];
                player_last_error(g_player, err, 512);
                MessageBoxW(hwnd, err, APP_TITLE, MB_OK | MB_ICONERROR);
            } else if ((PlayerEvent)wp == PLAYER_EVT_OPENED && g_player) {
                auto it = g_resume.find(g_cur_path);
                if (it != g_resume.end() && it->second > 15)
                    player_seek_to(g_player, it->second);
                auto tm = g_track_mem.find(g_cur_path);
                if (tm != g_track_mem.end())  // no-op when already matching
                    player_select_tracks(g_player, tm->second.first,
                                         tm->second.second);
            } else if ((PlayerEvent)wp == PLAYER_EVT_ENDED) {
                g_resume.erase(g_cur_path);
                if (g_autonext && g_siblings.size() > 1) nav_folder(hwnd, 1);
            }
            update_ui(hwnd);
            return 0;
        case WM_CLOSE:
            remember_position();
            save_state(hwnd);
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int show) {
    // When launched from a console, show our log lines there.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    load_state();
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

    WNDCLASSW pc = {};
    pc.lpfnWndProc = preview_proc;
    pc.hInstance = hinst;
    pc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    pc.lpszClassName = L"minimal_player_preview";
    RegisterClassW(&pc);

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

    g_preview = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        pc.lpszClassName, nullptr, WS_POPUP, 0, 0,
        PV_W + 2, PV_H + PV_TEXT + 2, g_main, nullptr, hinst, nullptr);
    SetWindowSubclass(g_seek, seek_subclass, 1, 0);
    g_thumb_thread = std::thread(thumb_worker);

    g_player = player_create(g_video);
    if (!g_player) {
        wchar_t msg[512];
        swprintf(msg, 512, L"Failed to initialize D3D11 video output.\n\n%s",
                 player_video_init_error());
        MessageBoxW(g_main, msg, APP_TITLE, MB_OK | MB_ICONERROR);
        return 1;
    }
    player_set_event_callback(g_player, on_player_event, g_main);
    if (g_loaded_vol >= 0) {
        player_volume_set(g_player, g_loaded_vol / 100.0f);
        player_set_mute(g_player, g_loaded_mute > 0);
    }
    SendMessageW(g_vol, TBM_SETPOS, TRUE, (LPARAM)(player_volume(g_player) * 100));

    if (g_have_placement) {
        g_loaded_placement.length = sizeof(WINDOWPLACEMENT);
        SetWindowPlacement(g_main, &g_loaded_placement);
    }
    layout(g_main);
    ShowWindow(g_main, g_have_placement ? (int)g_loaded_placement.showCmd : show);
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

    {
        std::lock_guard<std::mutex> lk(g_thumb_m);
        g_thumb_quit = true;
    }
    g_thumb_cv.notify_one();
    if (g_thumb_thread.joinable()) g_thumb_thread.join();
    preview_clear_cache();

    player_destroy(g_player);
    g_player = nullptr;
    CoUninitialize();
    return 0;
}
