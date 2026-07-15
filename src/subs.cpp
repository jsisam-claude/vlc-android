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

void subs_decode_packet(AVCodecContext* ctx, AVPacket* pkt, AVRational tb, SubtitleList* out) {
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

    std::wstring text;
    for (unsigned i = 0; i < sub.num_rects; i++) {
        AVSubtitleRect* r = sub.rects[i];
        std::wstring piece;
        if (r->type == SUBTITLE_ASS && r->ass)
            piece = ass_event_text(r->ass);
        else if (r->type == SUBTITLE_TEXT && r->text)
            piece = clean_markup(utf8_to_wide(r->text));
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

int subs_load_sidecar(const wchar_t* media_path, SubtitleList* out) {
    std::wstring side = subs_find_sidecar(media_path);
    if (side.empty()) return 0;

    std::string u8 = wide_to_utf8(side.c_str());
    AVFormatContext* fc = nullptr;
    if (avformat_open_input(&fc, u8.c_str(), nullptr, nullptr) < 0) return 0;
    if (avformat_find_stream_info(fc, nullptr) < 0) { avformat_close_input(&fc); return 0; }

    int si = av_find_best_stream(fc, AVMEDIA_TYPE_SUBTITLE, -1, -1, nullptr, 0);
    if (si < 0) { avformat_close_input(&fc); return 0; }

    const AVCodec* dec = avcodec_find_decoder(fc->streams[si]->codecpar->codec_id);
    AVCodecContext* ctx = dec ? avcodec_alloc_context3(dec) : nullptr;
    if (!ctx || avcodec_parameters_to_context(ctx, fc->streams[si]->codecpar) < 0 ||
        avcodec_open2(ctx, dec, nullptr) < 0) {
        if (ctx) avcodec_free_context(&ctx);
        avformat_close_input(&fc);
        return 0;
    }

    int n = 0;
    AVPacket* pkt = av_packet_alloc();
    while (av_read_frame(fc, pkt) >= 0) {
        if (pkt->stream_index == si) {
            subs_decode_packet(ctx, pkt, fc->streams[si]->time_base, out);
            n++;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fc);
    log_line("subs: loaded %d cues from sidecar", n);
    return n;
}
