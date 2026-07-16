// Decoder threads: pull packets, push frames. Serial numbers (bumped on
// every queue flush) let stale data from before a seek be discarded, as in
// ffplay.
#include "player_int.h"

static void run_decoder(Player* p, AVCodecContext* ctx, PacketQueue* pq,
                        std::atomic<int>* serial_mirror, FrameQueue* fq,
                        AVRational tb, const char* tag) {
    int cur_serial = pq->serial();
    AVFrame* frame = av_frame_alloc();

    while (!p->abort) {
        Pkt pk;
        if (!pq->pop(pk, 100)) continue;

        if (pk.flush) {
            avcodec_flush_buffers(ctx);
            cur_serial = pk.serial;
            serial_mirror->store(pk.serial);
            continue;
        }
        if (pk.serial != pq->serial()) {  // stale packet from before a flush
            av_packet_free(&pk.p);
            continue;
        }

        int ret = avcodec_send_packet(ctx, pk.p);
        av_packet_free(&pk.p);
        if (ret < 0 && ret != AVERROR(EAGAIN)) continue;

        while (!p->abort) {
            ret = avcodec_receive_frame(ctx, frame);
            if (ret < 0) break;
            double pts = NAN;
            if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                pts = frame->best_effort_timestamp * av_q2d(tb);
            double dur = frame->duration > 0 ? frame->duration * av_q2d(tb) : 0;
            if (!fq->push(frame, cur_serial, pts, dur)) break;
        }
    }
    av_frame_free(&frame);
    log_line("%s decoder thread exit", tag);
}

void video_decode_thread(Player* p) {
    if (p->vst < 0 || !p->vctx) return;
    run_decoder(p, p->vctx, &p->vq, &p->vq_serial, &p->vfq,
                p->fmt->streams[p->vst]->time_base, "video");
}

void audio_decode_thread(Player* p) {
    if (p->ast < 0 || !p->actx) return;
    run_decoder(p, p->actx, &p->aq, &p->aq_serial, &p->afq,
                p->fmt->streams[p->ast]->time_base, "audio");
}

// ------------------------------------------------------------ video render

void video_render_thread(Player* p) {
    bool first_frame = true;
    int fps_count = 0;
    double fps = 0;
    int64_t fps_t0 = av_gettime_relative();

    while (!p->abort) {
        if (p->paused) {
            if (p->step_req.exchange(false)) {
                FQFrame fr;
                if (p->vfq.pop(fr, 200)) {
                    if (fr.serial == p->vq.serial()) {
                        double pts = fr.pts;
                        SubRender ov;
                        ov.osd = p->osd_now();
                        if (!std::isnan(pts)) {
                            ov.text = p->subs.active_at(pts - p->sub_delay);
                            p->subs.active_bitmaps_at(pts - p->sub_delay, ov.bitmaps);
                        }
                        p->vo->render(fr.f, ov, p->rotation);
                        if (!std::isnan(pts)) {
                            p->vclock.store(pts);
                            p->extclk_set(pts);
                        }
                        std::lock_guard<std::mutex> lk(p->lastf_m);
                        av_frame_free(&p->last_frame);
                        p->last_frame = av_frame_clone(fr.f);
                    }
                    av_frame_free(&fr.f);
                }
                continue;
            }
            if (p->redraw_req.exchange(false)) {
                std::lock_guard<std::mutex> lk(p->lastf_m);
                if (p->last_frame) {
                    double lp = p->vclock.load();
                    SubRender ov;
                    ov.osd = p->osd_now();
                    if (!std::isnan(lp)) {
                        ov.text = p->subs.active_at(lp - p->sub_delay);
                        p->subs.active_bitmaps_at(lp - p->sub_delay, ov.bitmaps);
                    }
                    p->vo->render(p->last_frame, ov, p->rotation);
                }
            }
            Sleep(20);
            continue;
        }

        FQFrame fr;
        if (!p->vfq.pop(fr, 100)) continue;
        if (fr.serial != p->vq.serial()) {
            av_frame_free(&fr.f);
            first_frame = true;
            continue;
        }

        // Exact seek: drop decoded frames between the keyframe we landed on
        // and the requested position.
        double pv = p->precise_v.load();
        if (!std::isnan(pv)) {
            if (!std::isnan(fr.pts) &&
                fr.pts + (fr.dur > 0 ? fr.dur : 0.040) <= pv) {
                av_frame_free(&fr.f);
                continue;
            }
            p->precise_v = NAN;
        }

        double pts = fr.pts;
        if (std::isnan(pts)) pts = p->vclock.load();

        if (p->ast < 0) p->extclk_set(first_frame ? pts : NAN);  // anchor once
        double clock = p->master_clock();

        if (!std::isnan(clock) && !std::isnan(pts)) {
            double delay = pts - clock;
            if (delay > 0.002) {
                // wait, in slices, staying responsive to abort/seek
                double remaining = std::fmin(delay, 1.0);
                while (remaining > 0.002 && !p->abort && !p->paused &&
                       fr.serial == p->vq.serial()) {
                    DWORD ms = (DWORD)(std::fmin(remaining, 0.015) * 1000.0 + 0.5);
                    Sleep(ms ? ms : 1);
                    clock = p->master_clock();
                    remaining = std::isnan(clock) ? 0 : pts - clock;
                }
            } else if (delay < -0.060 && p->vfq.count() > 0 && !first_frame) {
                p->stat_drops++;
                av_frame_free(&fr.f);  // hopelessly late and a newer frame waits
                continue;
            }
        }
        if (p->abort || fr.serial != p->vq.serial()) {
            av_frame_free(&fr.f);
            continue;
        }

        fps_count++;
        int64_t now = av_gettime_relative();
        if (now - fps_t0 >= 1000000) {
            fps = fps_count * 1e6 / (now - fps_t0);
            fps_count = 0;
            fps_t0 = now;
        }

        SubRender ov;
        ov.osd = p->osd_now();
        if (p->hud && ov.osd.empty()) {
            wchar_t h[192];
            swprintf(h, 192,
                     L"%.0f fps · dropped %d · %ls decode · %dx%d %ls · "
                     L"vq %zu aq %zu · ×%.2f",
                     fps, p->stat_drops.load(),
                     p->hw_active.load() ? L"D3D11VA" : L"software",
                     fr.f->width, fr.f->height,
                     fr.f->format == AV_PIX_FMT_D3D11 ? L"gpu" : L"cpu",
                     p->vfq.count(), p->afq.count(), p->speed.load());
            ov.osd = h;
        }
        if (!std::isnan(pts)) {
            ov.text = p->subs.active_at(pts - p->sub_delay);
            p->subs.active_bitmaps_at(pts - p->sub_delay, ov.bitmaps);
        }
        p->vo->render(fr.f, ov, p->rotation);
        if (!std::isnan(pts)) p->vclock.store(pts);
        {
            std::lock_guard<std::mutex> lk(p->lastf_m);
            av_frame_free(&p->last_frame);
            p->last_frame = av_frame_clone(fr.f);
        }
        av_frame_free(&fr.f);
        first_frame = false;
    }
    log_line("video render thread exit");
}
