// Public C-style engine API. The shell (main.cpp today, possibly a WinForms
// host later) talks to the engine only through these calls.
#pragma once
#include <windows.h>

struct Player;

Player* player_create(HWND video_window);
void player_destroy(Player* p);

// Opens asynchronously; failures are reported via WM_APP_PLAYER_ERROR.
bool player_open(Player* p, const wchar_t* path);
void player_close(Player* p);
bool player_has_media(Player* p);

void player_toggle_pause(Player* p);
bool player_is_paused(Player* p);
void player_seek_rel(Player* p, double seconds);
void player_seek_to(Player* p, double seconds);
void player_volume_step(Player* p, int steps);
void player_volume_set(Player* p, float v);  // 0..1
float player_volume(Player* p);

// Both return the number of the now-active track (1-based) or 0 if none.
int player_cycle_audio(Player* p);
int player_cycle_subtitle(Player* p);

void player_notify_resize(Player* p);
double player_position(Player* p);
double player_duration(Player* p);
const wchar_t* player_error(Player* p);

#define WM_APP_PLAYER_ERROR (WM_APP + 1)
