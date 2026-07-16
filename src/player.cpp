// Engine lifecycle and the public C API. Track switching is implemented as
// a fast reopen at the current position, which keeps the threading model
// simple (v1 tradeoff documented in PLAN.md).
#include "player_int.h"
#include <cmath>
#include <mutex>
#include <timeapi.h>

static void engine_av_log(void*, int level, const char* fmt, va_list args) {
    if (level > AV_LOG_WARNING) return;
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    OutputDebugStringA(buf);
}

static void engine_global_init() {
    static std::once_flag once;
    std::call_once(once, [] {
        av_log_set_callback(engine_av_log);
        timeBeginPeriod(1);  // engine pacing relies on ~1ms Sleep granularity
    });
}

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
    engine_global_init();
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
    {
        std::lock_guard<std::mutex> lk(p->tracks_m);
        p->audio_names.clear();
        p->sub_names.clear();
        p->chapters.clear();
    }
    p->vst = p->ast = p->sst = -1;
    p->running = false;
    p->abort = false;
    p->eof = false;
    p->ended = false;
    p->ended_fired = false;
    {
        std::lock_guard<std::mutex> lk(p->lastf_m);
        av_frame_free(&p->last_frame);
    }
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

void player_frame_step(Player* p) {
    if (!p->running) return;
    if (!p->paused) player_toggle_pause(p);
    p->step_req = true;
}

void player_seek_to(Player* p, double seconds) {
    if (!p->running) return;
    double target = seconds;
    if (target < 0) target = 0;
    if (p->duration > 0 && target > p->duration - 0.5) target = p->duration - 0.5;
    std::lock_guard<std::mutex> lk(p->seek_m);
    p->seek_to = target;
    p->seek_req = true;
}

void player_seek_rel(Player* p, double seconds) {
    player_seek_to(p, player_position(p) + seconds);
}

void player_volume_step(Player* p, int steps) { p->ao.volume_step(steps); }
void player_volume_set(Player* p, float v) { p->ao.volume_set(v); }
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

int player_audio_track_count(Player* p) {
    std::lock_guard<std::mutex> lk(p->tracks_m);
    return (int)p->audio_names.size();
}

int player_audio_track_current(Player* p) { return p->want_audio_rel; }

void player_audio_track_name(Player* p, int i, wchar_t* buf, size_t buflen) {
    if (!buf || !buflen) return;
    buf[0] = 0;
    std::lock_guard<std::mutex> lk(p->tracks_m);
    if (i < 0 || i >= (int)p->audio_names.size()) return;
    wcsncpy(buf, p->audio_names[i].c_str(), buflen - 1);
    buf[buflen - 1] = 0;
}

void player_select_audio_track(Player* p, int i) {
    if (!p->running || i == p->want_audio_rel) return;
    if (i < 0 || i >= player_audio_track_count(p)) return;
    reopen(p, i, p->sub_choice);
}

int player_sub_track_count(Player* p) {
    std::lock_guard<std::mutex> lk(p->tracks_m);
    return (int)p->sub_names.size();
}

int player_sub_track_current(Player* p) {
    int n = player_sub_track_count(p);
    return p->sub_choice >= n ? -1 : p->sub_choice;
}

void player_sub_track_name(Player* p, int i, wchar_t* buf, size_t buflen) {
    if (!buf || !buflen) return;
    buf[0] = 0;
    std::lock_guard<std::mutex> lk(p->tracks_m);
    if (i < 0 || i >= (int)p->sub_names.size()) return;
    wcsncpy(buf, p->sub_names[i].c_str(), buflen - 1);
    buf[buflen - 1] = 0;
}

void player_select_sub_track(Player* p, int i) {
    if (!p->running) return;
    int n = player_sub_track_count(p);
    if (n == 0) return;
    int choice = (i < 0) ? n : i;  // n = the "off" slot
    if (choice > n || choice == p->sub_choice) return;
    reopen(p, p->want_audio_rel, choice);
}

int player_chapter_count(Player* p) {
    std::lock_guard<std::mutex> lk(p->tracks_m);
    return (int)p->chapters.size();
}

double player_chapter_start(Player* p, int i) {
    std::lock_guard<std::mutex> lk(p->tracks_m);
    if (i < 0 || i >= (int)p->chapters.size()) return 0;
    return p->chapters[i].first;
}

void player_chapter_name(Player* p, int i, wchar_t* buf, size_t buflen) {
    if (!buf || !buflen) return;
    buf[0] = 0;
    std::lock_guard<std::mutex> lk(p->tracks_m);
    if (i < 0 || i >= (int)p->chapters.size()) return;
    wcsncpy(buf, p->chapters[i].second.c_str(), buflen - 1);
    buf[buflen - 1] = 0;
}

int player_chapter_current(Player* p) {
    double pos = player_position(p) + 0.5;  // bias: right after a jump we
    std::lock_guard<std::mutex> lk(p->tracks_m);  // are "in" that chapter
    int cur = -1;
    for (int i = 0; i < (int)p->chapters.size(); i++)
        if (p->chapters[i].first <= pos) cur = i;
    return cur;
}

void player_chapter_go(Player* p, int i) {
    double start;
    {
        std::lock_guard<std::mutex> lk(p->tracks_m);
        if (i < 0 || i >= (int)p->chapters.size()) return;
        start = p->chapters[i].first;
    }
    player_seek_to(p, start);
}

int player_chapter_seek(Player* p, int delta) {
    int n = player_chapter_count(p);
    if (n == 0) return -1;
    int cur = player_chapter_current(p);
    int next = cur + delta;
    if (delta < 0 && cur >= 0) {
        // Going "back" from mid-chapter returns to its start first, like
        // every disc player; only jump further if already near the start.
        double pos = player_position(p);
        if (pos - player_chapter_start(p, cur) > 3.0) next = cur;
    }
    if (next < 0) next = 0;
    if (next >= n) next = n - 1;
    player_chapter_go(p, next);
    return next;
}

void player_show_osd(Player* p, const wchar_t* text, double seconds) {
    {
        std::lock_guard<std::mutex> lk(p->osd_m);
        p->osd_text = text ? text : L"";
        p->osd_until = av_gettime_relative() + (int64_t)(seconds * 1e6);
    }
    p->redraw_req = true;  // repaint promptly while paused
}

void player_notify_resize(Player* p) {
    if (p->vo) p->vo->resize();
    p->redraw_req = true;
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

void player_set_event_callback(Player* p, PlayerEventFn fn, void* user) {
    p->evt_fn = fn;
    p->evt_user = user;
}

bool player_media_ended(Player* p) { return p->ended; }

void player_set_mute(Player* p, bool m) { p->ao.set_mute(m); }
bool player_is_muted(Player* p) { return p->ao.muted(); }

void player_last_error(Player* p, wchar_t* buf, size_t buflen) {
    if (!buf || !buflen) return;
    std::lock_guard<std::mutex> lk(p->err_m);
    wcsncpy(buf, p->error.c_str(), buflen - 1);
    buf[buflen - 1] = 0;
}

const wchar_t* player_video_init_error(void) { return vo_init_error(); }

// Interrupt callback giving standalone avformat calls a wall-clock budget,
// so probing a file on a dead share fails instead of hanging the caller.
static int deadline_interrupt(void* op) {
    return av_gettime_relative() > *(int64_t*)op ? 1 : 0;
}

bool player_probe(const wchar_t* path, PlayerMediaInfo* info) {
    if (!info) return false;
    memset(info, 0, sizeof(*info));
    std::string u8 = wide_to_utf8(path);
    int64_t deadline = av_gettime_relative() + 5 * INT64_C(1000000);
    AVFormatContext* fc = avformat_alloc_context();
    if (!fc) return false;
    fc->interrupt_callback.callback = deadline_interrupt;
    fc->interrupt_callback.opaque = &deadline;
    if (avformat_open_input(&fc, u8.c_str(), nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(fc, nullptr) < 0) {
        avformat_close_input(&fc);
        return false;
    }
    info->duration_sec = fc->duration > 0 ? fc->duration / (double)AV_TIME_BASE : 0;
    for (unsigned i = 0; i < fc->nb_streams; i++) {
        AVCodecParameters* par = fc->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && !info->width) {
            info->width = par->width;
            info->height = par->height;
            std::wstring n = utf8_to_wide(avcodec_get_name(par->codec_id));
            wcsncpy(info->video_codec, n.c_str(), 31);
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (!info->audio_tracks) {
                std::wstring n = utf8_to_wide(avcodec_get_name(par->codec_id));
                wcsncpy(info->audio_codec, n.c_str(), 31);
            }
            info->audio_tracks++;
        } else if (par->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            info->sub_tracks++;
        }
    }
    avformat_close_input(&fc);
    return true;
}

bool player_extract_thumb(const wchar_t* path, int max_w, int max_h,
                          uint8_t* buf, int* out_w, int* out_h) {
    if (!buf || !out_w || !out_h || max_w < 8 || max_h < 8) return false;
    engine_global_init();
    std::string u8 = wide_to_utf8(path);
    int64_t deadline = av_gettime_relative() + 5 * INT64_C(1000000);

    AVFormatContext* fc = avformat_alloc_context();
    if (!fc) return false;
    fc->interrupt_callback.callback = deadline_interrupt;
    fc->interrupt_callback.opaque = &deadline;
    if (avformat_open_input(&fc, u8.c_str(), nullptr, nullptr) < 0) return false;

    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    SwsContext* sws = nullptr;
    bool ok = false;
    do {
        if (avformat_find_stream_info(fc, nullptr) < 0) break;
        int vst = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vst < 0) break;

        const AVCodec* dec = avcodec_find_decoder(fc->streams[vst]->codecpar->codec_id);
        if (!dec) break;
        ctx = avcodec_alloc_context3(dec);
        if (!ctx || avcodec_parameters_to_context(ctx, fc->streams[vst]->codecpar) < 0)
            break;
        ctx->thread_count = 1;
        if (avcodec_open2(ctx, dec, nullptr) < 0) break;

        // A few seconds in beats the (often black/logo) very first frame.
        double dur = fc->duration > 0 ? fc->duration / (double)AV_TIME_BASE : 0;
        double at = dur > 3 ? std::fmin(dur * 0.10, 60.0) : 0;
        if (at > 0) {
            int64_t ts = fc->start_time != AV_NOPTS_VALUE
                             ? fc->start_time + (int64_t)(at * AV_TIME_BASE)
                             : (int64_t)(at * AV_TIME_BASE);
            av_seek_frame(fc, -1, ts, AVSEEK_FLAG_BACKWARD);
        }

        frame = av_frame_alloc();
        pkt = av_packet_alloc();
        if (!frame || !pkt) break;
        bool got = false;
        for (int packets = 0; !got && packets < 512; ) {
            if (av_read_frame(fc, pkt) < 0) break;
            if (pkt->stream_index == vst) {
                packets++;
                if (avcodec_send_packet(ctx, pkt) >= 0)
                    while (avcodec_receive_frame(ctx, frame) >= 0) got = true;
            }
            av_packet_unref(pkt);
        }
        if (!got || frame->width <= 0 || frame->height <= 0) break;

        int sw = frame->width, sh = frame->height;
        if (frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0)
            sw = (int)av_rescale(sw, frame->sample_aspect_ratio.num,
                                 frame->sample_aspect_ratio.den);
        double scale = std::fmin((double)max_w / sw, (double)max_h / sh);
        if (scale > 1.0) scale = 1.0;
        int tw = (int)(sw * scale + 0.5), th = (int)(sh * scale + 0.5);
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;

        sws = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format,
                             tw, th, AV_PIX_FMT_BGRA, SWS_BILINEAR,
                             nullptr, nullptr, nullptr);
        if (!sws) break;
        uint8_t* dst[1] = {buf};
        int dst_stride[1] = {tw * 4};
        sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, dst_stride);
        *out_w = tw;
        *out_h = th;
        ok = true;
    } while (false);

    sws_freeContext(sws);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    avformat_close_input(&fc);
    return ok;
}
