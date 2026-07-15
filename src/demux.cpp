// Demux thread: opens the file, selects streams, feeds packet queues,
// executes seeks, decodes subtitle packets inline (cheap, synchronous).
#include "player_int.h"

static AVCodecContext* open_decoder(AVFormatContext* fmt, int stream, int threads) {
    if (stream < 0) return nullptr;
    AVCodecParameters* par = fmt->streams[stream]->codecpar;
    const AVCodec* dec = avcodec_find_decoder(par->codec_id);
    if (!dec) {
        log_line("demux: no decoder for stream %d (%s)", stream,
                 avcodec_get_name(par->codec_id));
        return nullptr;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    if (!ctx) return nullptr;
    if (avcodec_parameters_to_context(ctx, par) < 0 ||
        (ctx->thread_count = threads, ctx->pkt_timebase = fmt->streams[stream]->time_base,
         avcodec_open2(ctx, dec, nullptr)) < 0) {
        avcodec_free_context(&ctx);
        return nullptr;
    }
    return ctx;
}

static bool open_input(Player* p) {
    std::string u8 = wide_to_utf8(p->path.c_str());
    int ret = avformat_open_input(&p->fmt, u8.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char err[256];
        av_strerror(ret, err, sizeof(err));
        std::lock_guard<std::mutex> lk(p->err_m);
        p->error = L"Cannot open file:\n" + p->path + L"\n\n" + utf8_to_wide(err);
        return false;
    }
    if (avformat_find_stream_info(p->fmt, nullptr) < 0) {
        std::lock_guard<std::mutex> lk(p->err_m);
        p->error = L"Cannot read stream info:\n" + p->path;
        return false;
    }

    p->vst = av_find_best_stream(p->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    for (unsigned i = 0; i < p->fmt->nb_streams; i++) {
        AVMediaType t = p->fmt->streams[i]->codecpar->codec_type;
        if (t == AVMEDIA_TYPE_AUDIO) p->audio_streams.push_back((int)i);
        if (t == AVMEDIA_TYPE_SUBTITLE) p->sub_streams.push_back((int)i);
        p->fmt->streams[i]->discard = AVDISCARD_ALL;
    }
    if (p->vst < 0 && p->audio_streams.empty()) {
        std::lock_guard<std::mutex> lk(p->err_m);
        p->error = L"No playable streams in:\n" + p->path;
        return false;
    }

    if (!p->audio_streams.empty()) {
        if (p->want_audio_rel >= (int)p->audio_streams.size()) p->want_audio_rel = 0;
        p->ast = p->audio_streams[p->want_audio_rel];
    }

    // Effective subtitle track list: [off?, external?, internal streams...]
    // sub_choice: 0 = default (external if present, else first internal, else off)
    p->has_external_subs = subs_load_sidecar(p->path.c_str(), &p->subs) > 0;
    int n_subs = (p->has_external_subs ? 1 : 0) + (int)p->sub_streams.size();
    if (n_subs == 0) {
        p->sst = -1;
    } else {
        int choice = p->sub_choice % (n_subs + 1);  // last slot = off
        p->sub_choice = choice;
        if (choice == n_subs) {
            p->sst = -1;                       // subtitles off
            if (p->has_external_subs) p->subs.clear();
        } else if (p->has_external_subs && choice == 0) {
            p->sst = -1;                       // external file drives p->subs
        } else {
            p->subs.clear();                   // internal stream selected
            p->sst = p->sub_streams[choice - (p->has_external_subs ? 1 : 0)];
        }
    }

    if (p->vst >= 0) p->fmt->streams[p->vst]->discard = AVDISCARD_DEFAULT;
    if (p->ast >= 0) p->fmt->streams[p->ast]->discard = AVDISCARD_DEFAULT;
    if (p->sst >= 0) p->fmt->streams[p->sst]->discard = AVDISCARD_DEFAULT;

    p->vctx = open_decoder(p->fmt, p->vst, 0 /*auto threads*/);
    p->actx = open_decoder(p->fmt, p->ast, 1);
    p->sctx = open_decoder(p->fmt, p->sst, 1);
    if (p->vst >= 0 && !p->vctx) p->vst = -1;
    if (p->ast >= 0 && !p->actx) p->ast = -1;
    if (p->sst >= 0 && !p->sctx) p->sst = -1;
    if (p->vst < 0 && p->ast < 0) {
        std::lock_guard<std::mutex> lk(p->err_m);
        p->error = L"No decodable streams in:\n" + p->path;
        return false;
    }

    p->duration = p->fmt->duration > 0 ? p->fmt->duration / (double)AV_TIME_BASE : 0;
    p->start_time = p->fmt->start_time != AV_NOPTS_VALUE
                        ? p->fmt->start_time / (double)AV_TIME_BASE : 0;
    log_line("demux: opened '%s' v=%d a=%d s=%d dur=%.1fs",
             u8.c_str(), p->vst, p->ast, p->sst, p->duration);
    return true;
}

static void do_seek(Player* p, double target) {
    int64_t ts = (int64_t)((target + p->start_time) * AV_TIME_BASE);
    int64_t min_ts = INT64_MIN, max_ts = INT64_MAX;
    double cur = p->master_clock();
    int flags = (!std::isnan(cur) && target < cur - p->start_time) ? AVSEEK_FLAG_BACKWARD : 0;
    (void)min_ts; (void)max_ts;
    if (av_seek_frame(p->fmt, -1, ts, flags | AVSEEK_FLAG_BACKWARD) < 0)
        log_line("demux: seek to %.1fs failed", target);

    p->vq.flush();
    p->aq.flush();
    p->vfq.flush();
    p->afq.flush();
    p->ao.flush();
    if (p->sst >= 0) p->subs.clear();  // internal subs re-fill; sidecar list stays
    p->extclk_set(NAN);
    {
        std::lock_guard<std::mutex> lk(p->extclk_m);
        p->extclk_pts = NAN;
    }
    p->eof = false;
    p->ended = false;
    p->ended_fired = false;
}

void demux_thread(Player* p) {
    if (!open_input(p)) {
        p->open_failed = true;
        p->fire(PLAYER_EVT_ERROR);
        return;
    }

    if (p->open_at > 0.1) do_seek(p, p->open_at);

    if (p->vst >= 0) p->th_vdec = std::thread(video_decode_thread, p);
    if (p->ast >= 0) {
        p->th_adec = std::thread(audio_decode_thread, p);
        p->ao.start(&p->afq, &p->aq_serial);
        p->ao.pause(p->paused);
    }
    if (p->vst >= 0) p->th_vrender = std::thread(video_render_thread, p);
    p->running = true;
    p->fire(PLAYER_EVT_OPENED);

    AVPacket* pkt = av_packet_alloc();
    const size_t MAX_QUEUE_BYTES = 16 * 1024 * 1024;

    while (!p->abort) {
        {
            std::lock_guard<std::mutex> lk(p->seek_m);
            if (p->seek_req) {
                do_seek(p, p->seek_to);
                p->seek_req = false;
            }
        }

        if (p->vq.bytes() + p->aq.bytes() > MAX_QUEUE_BYTES ||
            (p->vq.count() > 200 && p->aq.count() > 200)) {
            Sleep(10);
            continue;
        }

        int ret = av_read_frame(p->fmt, pkt);
        if (ret == AVERROR_EOF || ret == AVERROR(EIO)) {
            p->eof = true;
            if (!p->ended_fired && p->vq.count() == 0 && p->aq.count() == 0 &&
                p->vfq.count() == 0 && p->afq.count() == 0) {
                p->ended = true;
                p->ended_fired = true;
                p->fire(PLAYER_EVT_ENDED);
            }
            Sleep(50);
            continue;
        }
        if (ret < 0) {
            Sleep(10);
            continue;
        }

        if (pkt->stream_index == p->vst) {
            p->vq.push(pkt);
        } else if (pkt->stream_index == p->ast) {
            p->aq.push(pkt);
        } else if (pkt->stream_index == p->sst && p->sctx) {
            subs_decode_packet(p->sctx, pkt, p->fmt->streams[p->sst]->time_base, &p->subs);
            av_packet_unref(pkt);
        } else {
            av_packet_unref(pkt);
        }
    }
    av_packet_free(&pkt);
    log_line("demux thread exit");
}
