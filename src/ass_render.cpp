// Styled subtitle rendering via vendored libass (ASS/SSA). Produces a
// full-frame premultiplied-BGRA overlay from an ASS_Image chain; the D3D
// path scales it into the letterbox exactly like a PGS bitmap. libass has
// no internal locking, so the engine serializes every call here behind
// Player::ass_m (see demux.cpp / decode.cpp).
#include "player_int.h"

extern "C" {
#include <ass.h>
}

static void ass_log_cb(int level, const char* fmt, va_list va, void*) {
    if (level > 4) return;  // 0..4 are fatal..warning; skip info/debug spam
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, va);
    log_line("ass: %s", buf);
}

struct AssRenderer {
    ASS_Library* lib = nullptr;
    ASS_Renderer* ren = nullptr;
    ASS_Track* track = nullptr;
    bool fonts_set = false;
    int store_w = 0, store_h = 0, frame_w = 0, frame_h = 0;
};

AssRenderer* ass_create() {
    AssRenderer* a = new AssRenderer();
    a->lib = ass_library_init();
    if (!a->lib) { delete a; return nullptr; }
    ass_set_message_cb(a->lib, ass_log_cb, nullptr);
    ass_set_extract_fonts(a->lib, 1);  // honor [Fonts] embedded in the script
    a->ren = ass_renderer_init(a->lib);
    if (!a->ren) { ass_library_done(a->lib); delete a; return nullptr; }
    a->track = ass_new_track(a->lib);
    if (!a->track) {
        ass_renderer_done(a->ren);
        ass_library_done(a->lib);
        delete a;
        return nullptr;
    }
    return a;
}

void ass_destroy(AssRenderer* a) {
    if (!a) return;
    if (a->track) ass_free_track(a->track);
    if (a->ren) ass_renderer_done(a->ren);
    if (a->lib) ass_library_done(a->lib);
    delete a;
}

void ass_set_header(AssRenderer* a, const char* hdr, int len) {
    if (a && a->track && hdr && len > 0)
        ass_process_codec_private(a->track, (char*)hdr, len);
}

void ass_add_attachment(AssRenderer* a, const char* name,
                        const char* data, int len) {
    // Must precede ass_set_fonts; fonts_set stays false until first render.
    if (a && a->lib && name && data && len > 0)
        ass_add_font(a->lib, (char*)name, (char*)data, len);
}

void ass_feed(AssRenderer* a, const char* event, int len,
              double start_s, double dur_s) {
    if (!a || !a->track || !event || len <= 0) return;
    long long start_ms = (long long)(start_s * 1000.0 + 0.5);
    long long dur_ms = (long long)(dur_s * 1000.0 + 0.5);
    if (dur_ms < 0) dur_ms = 0;
    // ReadOrder dedup (default) makes this safe to re-feed across seeks.
    ass_process_chunk(a->track, (char*)event, len, start_ms, dur_ms);
}

void ass_reset(AssRenderer* a) {
    if (a && a->track) ass_flush_events(a->track);
}

bool ass_render(AssRenderer* a, double now_s, int w, int h,
                std::vector<uint32_t>& out) {
    if (!a || !a->ren || !a->track || w <= 0 || h <= 0) return false;

    if (!a->fonts_set) {  // after attachments were added
        ass_set_fonts(a->ren, nullptr, "sans-serif",
                      ASS_FONTPROVIDER_DIRECTWRITE, nullptr, 1);
        a->fonts_set = true;
    }
    if (w != a->frame_w || h != a->frame_h) {
        ass_set_frame_size(a->ren, w, h);
        a->frame_w = w;
        a->frame_h = h;
    }
    if (a->store_w != w || a->store_h != h) {
        ass_set_storage_size(a->ren, w, h);
        a->store_w = w;
        a->store_h = h;
    }

    int change = 0;
    ASS_Image* img = ass_render_frame(a->ren, a->track,
                                      (long long)(now_s * 1000.0 + 0.5), &change);
    if (!img) return false;

    out.assign((size_t)w * h, 0u);  // transparent premultiplied BGRA
    for (; img; img = img->next) {
        if (img->w <= 0 || img->h <= 0) continue;
        // libass color is RGBA with A = 0 opaque .. 255 transparent.
        unsigned rr = (img->color >> 24) & 0xFF;
        unsigned gg = (img->color >> 16) & 0xFF;
        unsigned bb = (img->color >> 8) & 0xFF;
        unsigned ta = img->color & 0xFF;  // transparency
        unsigned base_a = 255 - ta;       // opacity of the fill
        for (int y = 0; y < img->h; y++) {
            int dy = img->dst_y + y;
            if (dy < 0 || dy >= h) continue;
            const unsigned char* srow = img->bitmap + (size_t)y * img->stride;
            uint32_t* drow = out.data() + (size_t)dy * w;
            for (int x = 0; x < img->w; x++) {
                int dx = img->dst_x + x;
                if (dx < 0 || dx >= w) continue;
                unsigned cov = srow[x];
                if (!cov) continue;
                unsigned a = base_a * cov / 255;  // final coverage 0..255
                if (!a) continue;
                // premultiplied BGRA; composite over what's already there
                // (source-over, straight src alpha)
                uint32_t dstpx = drow[dx];
                unsigned da = (dstpx >> 24) & 0xFF;
                unsigned dr = (dstpx >> 16) & 0xFF;
                unsigned dg = (dstpx >> 8) & 0xFF;
                unsigned db = dstpx & 0xFF;
                unsigned sr = rr * a / 255;  // premultiplied source
                unsigned sg = gg * a / 255;
                unsigned sb = bb * a / 255;
                unsigned inv = 255 - a;
                unsigned or_ = sr + dr * inv / 255;
                unsigned og = sg + dg * inv / 255;
                unsigned ob = sb + db * inv / 255;
                unsigned oa = a + da * inv / 255;
                drow[dx] = (oa << 24) | (or_ << 16) | (og << 8) | ob;
            }
        }
    }
    return true;
}
