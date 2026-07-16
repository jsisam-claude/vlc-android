// Public C-style engine API. Hosts (the bundled shell in main.cpp, or an
// embedding application like a gallery) talk to the engine only through
// these calls and never see FFmpeg or renderer types.
#pragma once
#include <stdint.h>
#include <windows.h>

struct Player;

// Events fire on engine threads - marshal to your UI thread (e.g.
// PostMessage) before touching UI state.
typedef enum PlayerEvent {
    PLAYER_EVT_OPENED = 1,  // media opened, pipeline running
    PLAYER_EVT_ERROR = 2,   // open failed; see player_last_error
    PLAYER_EVT_ENDED = 3,   // playback reached end of media
} PlayerEvent;
typedef void (*PlayerEventFn)(void* user, enum PlayerEvent evt);

Player* player_create(HWND video_window);
void player_destroy(Player* p);
void player_set_event_callback(Player* p, PlayerEventFn fn, void* user);

bool player_open(Player* p, const wchar_t* path);  // async; events follow
void player_close(Player* p);
bool player_has_media(Player* p);
bool player_media_ended(Player* p);

void player_toggle_pause(Player* p);
bool player_is_paused(Player* p);
void player_frame_step(Player* p);  // pauses if playing, then advances one frame
void player_seek_rel(Player* p, double seconds);
void player_seek_to(Player* p, double seconds);
void player_volume_step(Player* p, int steps);
void player_volume_set(Player* p, float v);  // 0..1
float player_volume(Player* p);
void player_set_mute(Player* p, bool mute);
bool player_is_muted(Player* p);

// Playback speed 0.25..4 (audio pitch shifts with rate). Persists across
// files within the session.
void player_set_speed(Player* p, double s);
double player_speed(Player* p);
// Sync corrections in seconds, reset on every open. Positive audio delay
// = audio heard later; positive subtitle delay = subtitles shown later.
void player_set_audio_delay(Player* p, double s);
double player_audio_delay(Player* p);
void player_set_sub_delay(Player* p, double s);
double player_sub_delay(Player* p);

// Audio output endpoints. Enumeration is standalone (COM-initialized
// thread); selection by endpoint id, NULL = follow the system default.
int player_audio_device_count(void);
void player_audio_device_name(int i, wchar_t* buf, size_t buflen);
void player_audio_device_id(int i, wchar_t* buf, size_t buflen);
void player_set_audio_device(Player* p, const wchar_t* id);
void player_audio_device_current(Player* p, wchar_t* buf, size_t buflen);

// Both return the number of the now-active track (1-based) or 0 if none.
int player_cycle_audio(Player* p);
int player_cycle_subtitle(Player* p);

// Track enumeration/selection. Subtitle index -1 = off; the external
// sidecar file, when present, is subtitle track 0. Selection reopens the
// media at the current position (cheap for local files).
int player_audio_track_count(Player* p);
int player_audio_track_current(Player* p);
void player_audio_track_name(Player* p, int i, wchar_t* buf, size_t buflen);
void player_select_audio_track(Player* p, int i);
int player_sub_track_count(Player* p);
int player_sub_track_current(Player* p);  // -1 = off
void player_sub_track_name(Player* p, int i, wchar_t* buf, size_t buflen);
void player_select_sub_track(Player* p, int i);  // -1 = off
// Select both at once (one reopen); pass current values to leave unchanged.
void player_select_tracks(Player* p, int audio, int sub);

// Chapters (mkv/mp4). Count is 0 when the media has none.
int player_chapter_count(Player* p);
double player_chapter_start(Player* p, int i);  // seconds from media start
void player_chapter_name(Player* p, int i, wchar_t* buf, size_t buflen);
int player_chapter_current(Player* p);          // index, -1 when none
void player_chapter_go(Player* p, int i);       // seek to chapter i
int player_chapter_seek(Player* p, int delta);  // jump +-N; returns target or -1

// Saves the currently displayed frame as a PNG (synchronous, WIC encoder;
// call from a COM-initialized thread). Applies aspect ratio and rotation.
bool player_snapshot(Player* p, const wchar_t* png_path);

// Transient on-screen text (volume, seek feedback...), auto-expires.
void player_show_osd(Player* p, const wchar_t* text, double seconds);

void player_notify_resize(Player* p);
double player_position(Player* p);
double player_duration(Player* p);

// Copies the last open error into buf (always NUL-terminated).
void player_last_error(Player* p, wchar_t* buf, size_t buflen);
// Which video-init step failed when player_create returned null.
const wchar_t* player_video_init_error(void);

// Lightweight metadata probe without starting playback (synchronous,
// hits the disk - call off the UI thread for slow media).
typedef struct PlayerMediaInfo {
    double duration_sec;
    int width, height;
    int audio_tracks, sub_tracks;
    wchar_t video_codec[32];
    wchar_t audio_codec[32];
} PlayerMediaInfo;
bool player_probe(const wchar_t* path, PlayerMediaInfo* info);

// Decodes one representative video frame (seeks a little way in, so
// thumbnails aren't all black lead-ins), scaled to fit (max_w, max_h)
// preserving aspect ratio, written to buf as tightly packed 32-bit BGRA
// rows (*out_w * 4 bytes per row). buf must hold max_w*max_h*4 bytes.
// Standalone and synchronous with an internal ~5s time budget; no player
// instance needed - safe to call from a host's thumbnail worker thread.
bool player_extract_thumb(const wchar_t* path, int max_w, int max_h,
                          uint8_t* buf, int* out_w, int* out_h);
// Same, at an explicit position (seconds, clamped to the duration).
// at_seconds < 0 picks the representative default.
bool player_extract_thumb_at(const wchar_t* path, double at_seconds,
                             int max_w, int max_h,
                             uint8_t* buf, int* out_w, int* out_h);
