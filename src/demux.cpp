// Demux thread: opens the file, selects streams, feeds packet queues,
// executes seeks, decodes subtitle packets inline (cheap, synchronous).
#include "player_int.h"

#include <d3d11.h>
extern "C" {
#include <libavutil/display.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

// Prefer D3D11 decoder surfaces when the codec's hwaccel offers them; any
// failure later in avcodec's hwaccel setup re-invokes this without
// AV_PIX_FMT_D3D11 in the list, falling back to software transparently.
static AVPixelFormat get_d3d11_format(AVCodecContext* ctx, const AVPixelFormat* fmts) {
    Player* p = (Player*)ctx->opaque;
    for (const AVPixelFormat* f = fmts; *f != AV_PIX_FMT_NONE; f++)
        if (*f == AV_PIX_FMT_D3D11) {
            log_line("decode: D3D11VA hardware decoding active (%s)",
                     avcodec_get_name(ctx->codec_id));
            if (p) p->hw_active = true;
            return AV_PIX_FMT_D3D11;
        }
    log_line("decode: no D3D11VA for %s, decoding in software",
             avcodec_get_name(ctx->codec_id));
    if (p) p->hw_active = false;
    return avcodec_default_get_format(ctx, fmts);
}

// Wraps the renderer's D3D11 device for libavcodec. Sharing one device
// keeps decoder output textures directly usable by the video processor.
static AVBufferRef* create_hw_device(Player* p) {
    if (!p->want_hw) return nullptr;  // user disabled hardware decode
    ID3D11Device* dev = p->vo ? p->vo->decode_device() : nullptr;
    if (!dev) return nullptr;
    AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!ref) return nullptr;
    AVHWDeviceContext* hc = (AVHWDeviceContext*)ref->data;
    AVD3D11VADeviceContext* dc = (AVD3D11VADeviceContext*)hc->hwctx;
    dev->AddRef();  // the hwdevice ctx releases it on free, always
    dc->device = dev;
    if (av_hwdevice_ctx_init(ref) < 0) {
        log_line("decode: av_hwdevice_ctx_init(d3d11va) failed");
        av_buffer_unref(&ref);
        return nullptr;
    }
    return ref;
}

static AVCodecContext* open_decoder(AVFormatContext* fmt, int stream, int threads,
                                    AVBufferRef* hwdev = nullptr,
                                    void* opaque = nullptr) {
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
    if (hwdev) {
        ctx->hw_device_ctx = av_buffer_ref(hwdev);
        ctx->opaque = opaque;
        ctx->get_format = get_d3d11_format;
        // The frame queue, the paused-repaint clone and the in-flight render
        // all hold decoder surfaces beyond the codec's own working set.
        ctx->extra_hw_frames = 8;
    }
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
    // Abort-aware I/O: without this, a dead network share or unplugged
    // drive blocks avformat_open_input/av_read_frame indefinitely and
    // player_close() hangs joining this thread.
    p->fmt = avformat_alloc_context();
    if (!p->fmt) return false;
    p->fmt->interrupt_callback.callback = [](void* op) -> int {
        return ((Player*)op)->abort ? 1 : 0;
    };
    p->fmt->interrupt_callback.opaque = p;
    // Least-privilege protocol set per open: local files must never reach
    // the network, URLs must never read local files (a hostile playlist
    // could otherwise reference either). tls_verify is forced on - lavf 62
    // still defaults to NOT verifying certificates (FF_API_NO_DEFAULT_TLS_VERIFY).
    AVDictionary* opts = nullptr;
    if (u8.find("://") != std::string::npos) {
        av_dict_set(&opts, "protocol_whitelist", "http,https,tcp,tls,udp,crypto,data", 0);
        av_dict_set(&opts, "tls_verify", "1", 0);
    } else {
        av_dict_set(&opts, "protocol_whitelist", "file,crypto,data", 0);
    }
    int ret = avformat_open_input(&p->fmt, u8.c_str(), nullptr, &opts);
    av_dict_free(&opts);
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

    // Human-readable track labels for host menus.
    auto stream_label = [&](int idx, int n) {
        AVDictionaryEntry* lang = av_dict_get(p->fmt->streams[idx]->metadata,
                                              "language", nullptr, 0);
        AVDictionaryEntry* title = av_dict_get(p->fmt->streams[idx]->metadata,
                                               "title", nullptr, 0);
        std::wstring name = L"Track " + std::to_wstring(n);
        if (lang) name += L" [" + utf8_to_wide(lang->value) + L"]";
        if (title) name += L" \u2014 " + utf8_to_wide(title->value);
        return name;
    };
    {
        std::lock_guard<std::mutex> lk(p->tracks_m);
        p->audio_names.clear();
        p->sub_names.clear();
        int n = 1;
        for (int idx : p->audio_streams) p->audio_names.push_back(stream_label(idx, n++));
        if (p->has_external_subs) p->sub_names.push_back(L"External file");
        n = 1;
        for (int idx : p->sub_streams) p->sub_names.push_back(stream_label(idx, n++));
    }

    // Phone recordings store their orientation as a display matrix; the
    // renderer applies it so portrait videos play upright.
    if (p->vst >= 0) {
        AVCodecParameters* vp = p->fmt->streams[p->vst]->codecpar;
        const AVPacketSideData* sd = av_packet_side_data_get(
            vp->coded_side_data, vp->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
        int rot = 0;
        if (sd && sd->size >= 9 * sizeof(int32_t)) {
            double a = av_display_rotation_get((const int32_t*)sd->data);
            if (!std::isnan(a)) {
                rot = (int)lround(-a);          // matrix angle is CCW; we rotate CW
                rot = ((rot % 360) + 360) % 360;
                rot = ((rot + 45) / 90 * 90) % 360;  // snap to 90-degree steps
            }
        }
        p->rotation = rot;
        if (rot) log_line("demux: video is rotated %d degrees", rot);
    }

    if (p->vst >= 0) p->fmt->streams[p->vst]->discard = AVDISCARD_DEFAULT;
    if (p->ast >= 0) p->fmt->streams[p->ast]->discard = AVDISCARD_DEFAULT;
    if (p->sst >= 0) p->fmt->streams[p->sst]->discard = AVDISCARD_DEFAULT;

    AVBufferRef* hwdev = (p->vst >= 0) ? create_hw_device(p) : nullptr;
    p->vctx = open_decoder(p->fmt, p->vst, 0 /*auto threads*/, hwdev, p);
    av_buffer_unref(&hwdev);
    p->actx = open_decoder(p->fmt, p->ast, 1);
    p->sctx = open_decoder(p->fmt, p->sst, 1);
    if (p->vst >= 0 && !p->vctx) p->vst = -1;
    if (p->ast >= 0 && !p->actx) p->ast = -1;
    if (p->sst >= 0 && !p->sctx) p->sst = -1;

    // Styled subtitles: for an internal ASS/SSA stream, drive libass with
    // the script header + any embedded font attachments. srt/mov_text/PGS
    // keep the existing text/bitmap paths.
    if (p->sctx && (p->sctx->codec_id == AV_CODEC_ID_ASS ||
                    p->sctx->codec_id == AV_CODEC_ID_SSA)) {
        std::lock_guard<std::mutex> lk(p->ass_m);
        p->ass = ass_create();
        if (p->ass) {
            if (p->sctx->subtitle_header && p->sctx->subtitle_header_size > 0)
                ass_set_header(p->ass, (const char*)p->sctx->subtitle_header,
                               p->sctx->subtitle_header_size);
            for (unsigned i = 0; i < p->fmt->nb_streams; i++) {
                AVStream* st = p->fmt->streams[i];
                if (st->codecpar->codec_type != AVMEDIA_TYPE_ATTACHMENT) continue;
                AVDictionaryEntry* fn = av_dict_get(st->metadata, "filename",
                                                    nullptr, 0);
                if (fn && st->codecpar->extradata &&
                    st->codecpar->extradata_size > 0)
                    ass_add_attachment(p->ass, fn->value,
                                       (const char*)st->codecpar->extradata,
                                       st->codecpar->extradata_size);
            }
        }
    }
    if (p->vst < 0 && p->ast < 0) {
        std::lock_guard<std::mutex> lk(p->err_m);
        p->error = L"No decodable streams in:\n" + p->path;
        return false;
    }

    p->duration = p->fmt->duration > 0 ? p->fmt->duration / (double)AV_TIME_BASE : 0;
    p->start_time = p->fmt->start_time != AV_NOPTS_VALUE
                        ? p->fmt->start_time / (double)AV_TIME_BASE : 0;

    // Music metadata + cover-art detection (an attached_pic stream is a
    // still image, not a real video track; it plays as a static frame).
    {
        auto metaval = [&](const char* key) -> std::wstring {
            AVDictionaryEntry* e = av_dict_get(p->fmt->metadata, key, nullptr, 0);
            return e && e->value[0] ? utf8_to_wide(e->value) : L"";
        };
        std::lock_guard<std::mutex> lk(p->tracks_m);
        p->meta_title = metaval("title");
        p->meta_artist = metaval("artist");
        p->meta_album = metaval("album");
        p->cover_only =
            p->vst >= 0 && (p->fmt->streams[p->vst]->disposition &
                            AV_DISPOSITION_ATTACHED_PIC) != 0;
    }

    // Chapter list for host menus; start times shifted to position space.
    {
        std::lock_guard<std::mutex> lk(p->tracks_m);
        p->chapters.clear();
        for (unsigned i = 0; i < p->fmt->nb_chapters; i++) {
            AVChapter* ch = p->fmt->chapters[i];
            double sec = ch->start * av_q2d(ch->time_base) - p->start_time;
            if (sec < 0) sec = 0;
            AVDictionaryEntry* t = av_dict_get(ch->metadata, "title", nullptr, 0);
            std::wstring name = t && t->value[0]
                                    ? utf8_to_wide(t->value)
                                    : L"Chapter " + std::to_wstring(i + 1);
            p->chapters.emplace_back(sec, std::move(name));
        }
    }
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
    if (p->ass) {
        std::lock_guard<std::mutex> lk(p->ass_m);
        ass_reset(p->ass);
    }
    p->extclk_set(NAN);
    {
        std::lock_guard<std::mutex> lk(p->extclk_m);
        p->extclk_pts = NAN;
    }
    p->eof = false;
    p->ended = false;
    p->ended_fired = false;
    p->precise_v = ts / (double)AV_TIME_BASE;  // consumers drop up to here
    p->precise_a = ts / (double)AV_TIME_BASE;
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
        p->ao.start(&p->afq, &p->aq_serial, &p->precise_a);
        p->ao.pause(p->paused);
    }
    // Audio-only media still gets the render thread: it draws the spectrum
    // visualization (no cover art) via the same VideoOut.
    if (p->vst >= 0 || p->ast >= 0) p->th_vrender = std::thread(video_render_thread, p);
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
            std::unique_lock<std::mutex> lk(p->ass_m, std::defer_lock);
            if (p->ass) lk.lock();  // serialize libass feed vs. render
            subs_decode_packet(p->sctx, pkt, p->fmt->streams[p->sst]->time_base,
                               &p->subs, p->ass);
            av_packet_unref(pkt);
        } else {
            av_packet_unref(pkt);
        }
    }
    av_packet_free(&pkt);
    log_line("demux thread exit");
}
