// Text subtitle handling: decoding (srt/ass/mov_text via avcodec), ASS event
// field extraction, sidecar discovery. Encoding fallback (UTF-8 then ANSI)
// follows the spirit of VLC's modules/demux/subtitle.c heuristics.
#include "player_int.h"

void SubtitleList::add(double start, double end, std::wstring text) {
    if (text.empty()) return;
    std::lock_guard<std::mutex> lk(m_);
    entries_.push_back({start, end, std::move(text)});
}

std::wstring SubtitleList::active_at(double pts) {
    std::lock_guard<std::mutex> lk(m_);
    std::wstring out;
    for (auto& e : entries_) {
        if (pts >= e.start && pts <= e.end) {
            if (!out.empty()) out += L"\n";
            out += e.text;
        }
    }
    return out;
}

void SubtitleList::clear() {
    std::lock_guard<std::mutex> lk(m_);
    entries_.clear();
    bitmaps_.clear();
}

void SubtitleList::add_bitmap(std::shared_ptr<SubBitmap> b) {
    if (!b || b->pixels.empty()) return;
    std::lock_guard<std::mutex> lk(m_);
    bitmaps_.push_back(std::move(b));
    if (bitmaps_.size() > 64)  // bound memory; old cues are long gone
        bitmaps_.erase(bitmaps_.begin(), bitmaps_.begin() + 32);
}

void SubtitleList::clear_bitmaps_at(double pts) {
    std::lock_guard<std::mutex> lk(m_);
    for (auto& b : bitmaps_)
        if (b->end > pts && b->start <= pts) b->end = pts;
}

void SubtitleList::active_bitmaps_at(double pts,
                                     std::vector<std::shared_ptr<SubBitmap>>& out) {
    std::lock_guard<std::mutex> lk(m_);
    for (auto& b : bitmaps_)
        if (pts >= b->start && pts <= b->end) out.push_back(b);
}

// Strip ASS override tags ("{\...}") and unescape \N, \n, \h.
static std::wstring clean_markup(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == L'{') {
            size_t close = in.find(L'}', i);
            if (close != std::wstring::npos) { i = close; continue; }
        }
        if (in[i] == L'\\' && i + 1 < in.size()) {
            wchar_t c = in[i + 1];
            if (c == L'N' || c == L'n') { out += L'\n'; i++; continue; }
            if (c == L'h') { out += L' '; i++; continue; }
        }
        out += in[i];
    }
    // trim trailing whitespace/newlines
    while (!out.empty() && (out.back() == L'\n' || out.back() == L'\r' || out.back() == L' '))
        out.pop_back();
    return out;
}

// FFmpeg ASS rects carry the event line without the "Dialogue: " prefix:
// ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text
static std::wstring ass_event_text(const char* ass) {
    const char* s = ass;
    for (int commas = 0; *s && commas < 8; s++)
        if (*s == ',') commas++;
    return clean_markup(utf8_to_wide(s));
}

void subs_decode_packet(AVCodecContext* ctx, AVPacket* pkt, AVRational tb,
                        SubtitleList* out, AssRenderer* ass) {
    AVSubtitle sub;
    int got = 0;
    if (avcodec_decode_subtitle2(ctx, &sub, &got, pkt) < 0 || !got) return;

    double base = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts * av_q2d(tb) : 0;
    double start = base + sub.start_display_time / 1000.0;
    double end = base + (sub.end_display_time ? sub.end_display_time / 1000.0 : 0);
    if (end <= start) {
        double dur = (pkt->duration > 0) ? pkt->duration * av_q2d(tb) : 4.0;
        end = start + dur;
    }

    if (sub.num_rects == 0)  // PGS-style clear event ends open bitmaps
        out->clear_bitmaps_at(start);

    std::wstring text;
    for (unsigned i = 0; i < sub.num_rects; i++) {
        AVSubtitleRect* r = sub.rects[i];
        std::wstring piece;
        if (r->type == SUBTITLE_ASS && r->ass && ass) {
            // Styled path: hand the raw event line to libass; it renders
            // positioning/karaoke/fonts the flat-text path can't.
            ass_feed(ass, r->ass, (int)strlen(r->ass), start, end - start);
            continue;
        } else if (r->type == SUBTITLE_ASS && r->ass)
            piece = ass_event_text(r->ass);
        else if (r->type == SUBTITLE_TEXT && r->text)
            piece = clean_markup(utf8_to_wide(r->text));
        else if (r->type == SUBTITLE_BITMAP && r->data[0] && r->data[1] &&
                 r->w > 0 && r->h > 0) {
            auto b = std::make_shared<SubBitmap>();
            b->start = start;
            // PGS often signals the end with a later empty event
            b->end = (sub.end_display_time > 0) ? end : start + 10.0;
            b->x = r->x;
            b->y = r->y;
            b->w = r->w;
            b->h = r->h;
            b->src_w = ctx->width > 0 ? ctx->width : r->x + r->w;
            b->src_h = ctx->height > 0 ? ctx->height : r->y + r->h;
            b->pixels.resize((size_t)r->w * r->h);
            const uint32_t* pal = (const uint32_t*)r->data[1];
            for (int row = 0; row < r->h; row++) {
                const uint8_t* src = r->data[0] + (size_t)row * r->linesize[0];
                uint32_t* dst = b->pixels.data() + (size_t)row * r->w;
                for (int col = 0; col < r->w; col++) {
                    uint32_t c = pal[src[col]];  // AARRGGBB
                    uint32_t a = c >> 24;
                    uint32_t rr = ((c >> 16) & 0xFF) * a / 255;
                    uint32_t gg = ((c >> 8) & 0xFF) * a / 255;
                    uint32_t bb = (c & 0xFF) * a / 255;
                    dst[col] = (a << 24) | (rr << 16) | (gg << 8) | bb;
                }
            }
            out->add_bitmap(std::move(b));
        }
        if (!piece.empty()) {
            if (!text.empty()) text += L"\n";
            text += piece;
        }
    }
    if (!text.empty()) out->add(start, end, std::move(text));
    avsubtitle_free(&sub);
}

std::wstring subs_find_sidecar(const wchar_t* media_path) {
    std::wstring base(media_path);
    size_t dot = base.find_last_of(L'.');
    if (dot != std::wstring::npos) base.resize(dot);
    for (const wchar_t* ext : {L".srt", L".ass", L".ssa"}) {
        std::wstring cand = base + ext;
        DWORD attr = GetFileAttributesW(cand.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
            return cand;
    }
    return L"";
}

// ---- sidecar encoding detection -----------------------------------------
// FFmpeg's text-subtitle demuxers require UTF-8. Real-world .srt files come
// as UTF-8 (with or without BOM), UTF-16, or legacy ANSI codepages; detect
// and convert, then feed avformat from memory.

static bool valid_utf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        int follow = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1
                     : (c & 0xF0) == 0xE0              ? 2
                     : (c & 0xF8) == 0xF0              ? 3
                                                       : -1;
        if (follow < 0 || i + follow >= s.size()) return false;
        for (int k = 1; k <= follow; k++)
            if ((s[i + k] & 0xC0) != 0x80) return false;
        i += follow + 1;
    }
    return true;
}

static std::string wide_span_to_utf8(const wchar_t* w, size_t n) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w, (int)n, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? len : 0, 0);
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w, (int)n, &out[0], len, nullptr, nullptr);
    return out;
}

static std::string read_sidecar_utf8(const wchar_t* path) {
    FILE* f = _wfopen(path, L"rb");
    if (!f) return "";
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 8 * 1024 * 1024) {
        fclose(f);
        return "";
    }
    std::string raw((size_t)len, 0);
    size_t rd = fread(&raw[0], 1, (size_t)len, f);
    fclose(f);
    raw.resize(rd);
    if (raw.size() < 2) return raw;

    const unsigned char* b = (const unsigned char*)raw.data();
    if (raw.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF)
        return raw.substr(3);  // UTF-8 BOM
    if (b[0] == 0xFF && b[1] == 0xFE) {  // UTF-16 LE
        return wide_span_to_utf8((const wchar_t*)(raw.data() + 2),
                                 (raw.size() - 2) / 2);
    }
    if (b[0] == 0xFE && b[1] == 0xFF) {  // UTF-16 BE
        std::wstring w((raw.size() - 2) / 2, 0);
        for (size_t i = 0; i < w.size(); i++)
            w[i] = (wchar_t)((b[2 + 2 * i] << 8) | b[3 + 2 * i]);
        return wide_span_to_utf8(w.c_str(), w.size());
    }
    if (valid_utf8(raw)) return raw;
    // legacy single-byte file: interpret in the user's ANSI codepage
    int wn = MultiByteToWideChar(CP_ACP, 0, raw.c_str(), (int)raw.size(), nullptr, 0);
    std::wstring w(wn > 0 ? wn : 0, 0);
    if (wn > 0)
        MultiByteToWideChar(CP_ACP, 0, raw.c_str(), (int)raw.size(), &w[0], wn);
    log_line("subs: sidecar converted from ANSI codepage");
    return wide_span_to_utf8(w.c_str(), w.size());
}

struct MemReader {
    const uint8_t* data;
    size_t size, pos;
};

static int mem_read(void* op, uint8_t* buf, int n) {
    MemReader* m = (MemReader*)op;
    size_t left = m->size - m->pos;
    if (left == 0) return AVERROR_EOF;
    if ((size_t)n > left) n = (int)left;
    memcpy(buf, m->data + m->pos, n);
    m->pos += n;
    return n;
}

static int64_t mem_seek(void* op, int64_t off, int whence) {
    MemReader* m = (MemReader*)op;
    if (whence == AVSEEK_SIZE) return (int64_t)m->size;
    int64_t base = whence == SEEK_CUR ? (int64_t)m->pos
                   : whence == SEEK_END ? (int64_t)m->size
                                        : 0;
    int64_t p = base + off;
    if (p < 0 || p > (int64_t)m->size) return -1;
    m->pos = (size_t)p;
    return p;
}

int subs_load_sidecar(const wchar_t* media_path, SubtitleList* out) {
    std::wstring side = subs_find_sidecar(media_path);
    if (side.empty()) return 0;

    std::string text = read_sidecar_utf8(side.c_str());
    if (text.empty()) return 0;

    MemReader mr = {(const uint8_t*)text.data(), text.size(), 0};
    unsigned char* iobuf = (unsigned char*)av_malloc(4096);
    AVIOContext* avio =
        iobuf ? avio_alloc_context(iobuf, 4096, 0, &mr, mem_read, nullptr, mem_seek)
              : nullptr;
    if (!avio) {
        av_free(iobuf);
        return 0;
    }

    std::string u8 = wide_to_utf8(side.c_str());  // name only (format hints)
    AVFormatContext* fc = avformat_alloc_context();
    if (!fc) {
        av_freep(&avio->buffer);
        avio_context_free(&avio);
        return 0;
    }
    fc->pb = avio;
    fc->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_open_input(&fc, u8.c_str(), nullptr, nullptr) < 0) {
        av_freep(&avio->buffer);
        avio_context_free(&avio);
        return 0;
    }
    int n = -1;
    AVCodecContext* ctx = nullptr;
    AVPacket* pkt = nullptr;
    do {
        if (avformat_find_stream_info(fc, nullptr) < 0) break;
        int si = av_find_best_stream(fc, AVMEDIA_TYPE_SUBTITLE, -1, -1, nullptr, 0);
        if (si < 0) break;

        const AVCodec* dec = avcodec_find_decoder(fc->streams[si]->codecpar->codec_id);
        ctx = dec ? avcodec_alloc_context3(dec) : nullptr;
        if (!ctx || avcodec_parameters_to_context(ctx, fc->streams[si]->codecpar) < 0 ||
            avcodec_open2(ctx, dec, nullptr) < 0)
            break;

        n = 0;
        pkt = av_packet_alloc();
        if (!pkt) break;
        while (av_read_frame(fc, pkt) >= 0) {
            if (pkt->stream_index == si) {
                subs_decode_packet(ctx, pkt, fc->streams[si]->time_base, out);
                n++;
            }
            av_packet_unref(pkt);
        }
    } while (false);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fc);
    av_freep(&avio->buffer);
    avio_context_free(&avio);
    if (n > 0) log_line("subs: loaded %d cues from sidecar", n);
    return n > 0 ? n : 0;
}
