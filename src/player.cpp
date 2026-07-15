// Engine lifecycle and the public C API. Track switching is implemented as
// a fast reopen at the current position, which keeps the threading model
// simple (v1 tradeoff documented in PLAN.md).
#include "player_int.h"
#include <cmath>

double Player::master_clock() {
    if (ast >= 0) {
        double c = ao.clock();
        if (!std::isnan(c)) return c;
        return NAN;
    }
    std::lock_guard<std::mutex> lk(extclk_m);
    if (std::isnan(extclk_pts)) return NAN;
    return extclk_pts + (av_gettime_relative() - extclk_time) / 1e6;
}

void Player::extclk_set(double pts) {
    if (std::isnan(pts)) return;
    std::lock_guard<std::mutex> lk(extclk_m);
    extclk_pts = pts;
    extclk_time = av_gettime_relative();
}

Player* player_create(HWND video_window) {
    Player* p = new Player();
    p->hwnd = video_window;
    p->vo = new VideoOut();
    if (!p->vo->init(video_window)) {
        log_line("player: video output init failed");
        delete p->vo;
        delete p;
        return nullptr;
    }
    return p;
}

static void stop_pipeline(Player* p) {
    if (!p->running && !p->th_demux.joinable()) return;
    p->abort = true;
    p->vq.set_abort(true);
    p->aq.set_abort(true);
    p->vfq.set_abort(true);
    p->afq.set_abort(true);
    p->ao.stop();
    if (p->th_demux.joinable()) p->th_demux.join();
    if (p->th_vdec.joinable()) p->th_vdec.join();
    if (p->th_adec.joinable()) p->th_adec.join();
    if (p->th_vrender.joinable()) p->th_vrender.join();

    avcodec_free_context(&p->vctx);
    avcodec_free_context(&p->actx);
    avcodec_free_context(&p->sctx);
    if (p->fmt) avformat_close_input(&p->fmt);

    p->vq.set_abort(false);
    p->aq.set_abort(false);
    p->vfq.set_abort(false);
    p->afq.set_abort(false);
    p->vfq.flush();
    p->afq.flush();
    p->subs.clear();
    p->audio_streams.clear();
    p->sub_streams.clear();
    p->vst = p->ast = p->sst = -1;
    p->running = false;
    p->abort = false;
    p->eof = false;
    p->vclock = NAN;
    p->duration = 0;
    {
        std::lock_guard<std::mutex> lk(p->extclk_m);
        p->extclk_pts = NAN;
    }
    std::lock_guard<std::mutex> lk(p->seek_m);
    p->seek_req = false;
}

static bool start_pipeline(Player* p) {
    p->open_failed = false;
    p->error.clear();
    p->th_demux = std::thread(demux_thread, p);
    return true;
}

bool player_open(Player* p, const wchar_t* path) {
    stop_pipeline(p);
    p->path = path;
    p->want_audio_rel = 0;
    p->sub_choice = 0;
    p->open_at = 0;
    p->paused = false;
    return start_pipeline(p);
}

static void reopen(Player* p, int want_audio_rel, int sub_choice) {
    double pos = player_position(p);
    bool was_paused = p->paused;
    stop_pipeline(p);
    p->want_audio_rel = want_audio_rel;
    p->sub_choice = sub_choice;
    p->open_at = pos > 0 ? pos : 0;
    p->paused = was_paused;
    start_pipeline(p);
}

void player_close(Player* p) {
    stop_pipeline(p);
    p->path.clear();
}

void player_destroy(Player* p) {
    if (!p) return;
    stop_pipeline(p);
    delete p->vo;
    delete p;
}

bool player_has_media(Player* p) { return p->running || p->th_demux.joinable(); }

void player_toggle_pause(Player* p) {
    bool now = !p->paused;
    p->paused = now;
    p->ao.pause(now);
    if (!now) p->extclk_set(p->vclock.load());  // re-anchor no-audio clock
}

bool player_is_paused(Player* p) { return p->paused; }

void player_seek_rel(Player* p, double seconds) {
    if (!p->running) return;
    double cur = player_position(p);
    double target = cur + seconds;
    if (target < 0) target = 0;
    if (p->duration > 0 && target > p->duration - 0.5) target = p->duration - 0.5;
    std::lock_guard<std::mutex> lk(p->seek_m);
    p->seek_to = target;
    p->seek_req = true;
}

void player_volume_step(Player* p, int steps) { p->ao.volume_step(steps); }
float player_volume(Player* p) { return p->ao.volume(); }

int player_cycle_audio(Player* p) {
    if (!p->running || p->audio_streams.size() < 2) return 0;
    int next = (p->want_audio_rel + 1) % (int)p->audio_streams.size();
    reopen(p, next, p->sub_choice);
    return next + 1;
}

int player_cycle_subtitle(Player* p) {
    if (!p->running) return 0;
    int n_subs = (p->has_external_subs ? 1 : 0) + (int)p->sub_streams.size();
    if (n_subs == 0) return 0;
    int next = (p->sub_choice + 1) % (n_subs + 1);  // extra slot = off
    reopen(p, p->want_audio_rel, next);
    return next == n_subs ? 0 : next + 1;
}

void player_notify_resize(Player* p) {
    if (p->vo) p->vo->resize();
}

double player_position(Player* p) {
    if (!p->running) return 0;
    double c = p->master_clock();
    if (std::isnan(c)) c = p->vclock.load();
    if (std::isnan(c)) return 0;
    double pos = c - p->start_time;
    return pos > 0 ? pos : 0;
}

double player_duration(Player* p) { return p->running ? p->duration : 0; }

const wchar_t* player_error(Player* p) { return p->error.c_str(); }
