// Internal definitions shared by the engine translation units.
// Player structure (queues, serials, audio-master clock) follows
// FFmpeg's fftools/ffplay.c (LGPL-2.1+), adapted for Win32/WASAPI/D3D11.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "player.h"

void log_line(const char* fmt, ...);
std::string wide_to_utf8(const wchar_t* w);
std::wstring utf8_to_wide(const char* s, int len = -1);

// ---------------------------------------------------------------- queues

struct Pkt {
    AVPacket* p = nullptr;
    int serial = 0;
    bool flush = false;   // marker: decoder must flush, adopt this serial
};

class PacketQueue {
public:
    void push(AVPacket* src);            // takes ownership of the ref
    bool pop(Pkt& out, int timeout_ms);  // false on timeout/abort
    void flush();                        // clears, bumps serial, queues marker
    void set_abort(bool a);
    int serial() const { return serial_.load(); }
    size_t bytes() const { return bytes_.load(); }
    size_t count();

private:
    std::deque<Pkt> q_;
    std::mutex m_;
    std::condition_variable cv_;
    std::atomic<int> serial_{0};
    std::atomic<size_t> bytes_{0};
    bool abort_ = false;
};

struct FQFrame {
    AVFrame* f = nullptr;
    int serial = 0;
    double pts = 0;       // seconds, stream timebase applied
    double dur = 0;
};

class FrameQueue {
public:
    explicit FrameQueue(size_t max) : max_(max) {}
    bool push(AVFrame* f, int serial, double pts, double dur); // blocks; false on abort
    bool pop(FQFrame& out, int timeout_ms);
    bool peek(FQFrame& out);             // no removal, no wait
    void drop_front();
    void flush();
    void set_abort(bool a);
    size_t count();

private:
    std::deque<FQFrame> q_;
    std::mutex m_;
    std::condition_variable cv_push_, cv_pop_;
    size_t max_;
    bool abort_ = false;
};

// ---------------------------------------------------------------- subtitles

struct SubEntry {
    double start = 0, end = 0;  // seconds
    std::wstring text;
};

// One decoded bitmap subtitle rect (PGS/VobSub), BGRA premultiplied.
struct SubBitmap {
    double start = 0, end = 0;
    int x = 0, y = 0, w = 0, h = 0;   // in (src_w, src_h) coordinate space
    int src_w = 0, src_h = 0;
    std::vector<uint32_t> pixels;      // w*h premultiplied BGRA
};

// What the renderer composites over a frame.
struct SubRender {
    std::wstring text;
    std::wstring osd;
    std::vector<std::shared_ptr<SubBitmap>> bitmaps;
};

class SubtitleList {
public:
    void add(double start, double end, std::wstring text);
    void add_bitmap(std::shared_ptr<SubBitmap> b);
    void clear_bitmaps_at(double pts);  // PGS empty event ends open bitmaps
    std::wstring active_at(double pts);
    void active_bitmaps_at(double pts, std::vector<std::shared_ptr<SubBitmap>>& out);
    void clear();

private:
    std::mutex m_;
    std::vector<SubEntry> entries_;
    std::vector<std::shared_ptr<SubBitmap>> bitmaps_;
};

// Decode one subtitle packet with ctx and append results (handles srt/ass/mov_text).
void subs_decode_packet(AVCodecContext* ctx, AVPacket* pkt, AVRational tb, SubtitleList* out);
// Load an external .srt/.ass sidecar entirely. Returns entry count, 0 on failure.
int subs_load_sidecar(const wchar_t* media_path, SubtitleList* out);
// Returns sidecar path if one exists next to the media file, else empty.
std::wstring subs_find_sidecar(const wchar_t* media_path);

// ---------------------------------------------------------------- audio out

struct MMDevice;  // COM bits hidden in audio_out.cpp

class AudioOut {
public:
    // Pulls decoded frames from fq; compares frame serials against pq_serial.
    // drop_until (optional): frames wholly before this pts are discarded and
    // the value reset to NAN once reached (exact-seek support).
    bool start(FrameQueue* fq, const std::atomic<int>* pq_serial,
               std::atomic<double>* drop_until = nullptr);
    void stop();
    void pause(bool paused);
    void flush();                 // seek: clear FIFO, clock invalid until next frame
    double clock();               // playback pts in seconds; NAN when unknown
    void volume_step(int steps);  // +-5% per step
    void volume_set(float v);     // 0..2; >1 = session max + software gain
    float volume();
    void set_mute(bool m);
    bool muted();
    // Endpoint id ("" = follow system default). Takes effect immediately
    // when playing (device reopen) and on the next start otherwise.
    void set_device(const wchar_t* id);
    std::wstring device();
    void set_speed(double s);     // resample ratio (pitch shifts with rate)
    ~AudioOut() { stop(); }

private:
    void thread_main();
    bool run_device();            // one device lifetime; true = lost, retry
    MMDevice* dev_ = nullptr;
    std::mutex dev_m_;            // guards dev_/want_dev_ vs. reopen
    std::wstring want_dev_;       // endpoint id; empty = system default
    std::atomic<bool> dev_change_{false};
    std::atomic<double> speed_{1.0};
    std::atomic<float> gain_{1.0f};       // software gain (volume above 100%)
    std::atomic<float> want_vol_{-1.0f};  // last requested; reapplied on reopen
    std::atomic<int> want_mute_{-1};
    FrameQueue* fq_ = nullptr;
    const std::atomic<int>* pq_serial_ = nullptr;
    std::atomic<double>* drop_until_ = nullptr;
    std::thread th_;
    std::atomic<bool> abort_{false}, paused_{false}, flush_req_{false};
    std::mutex clock_m_;
    double fifo_end_pts_ = NAN;   // pts at the end of what we queued so far
    SwrContext* swr_ = nullptr;
    AVAudioFifo* fifo_ = nullptr;
    int in_rate_ = 0, in_fmt_ = -1;
    AVChannelLayout in_layout_{};
};

// ---------------------------------------------------------------- video out

struct D3DState;  // D3D11/D2D bits hidden in video_out.cpp
struct ID3D11Device;

// Which init step failed and its HRESULT (empty if none).
const wchar_t* vo_init_error();

class VideoOut {
public:
    bool init(HWND hwnd);
    // Renders a CPU frame (any sw pix fmt) or an AV_PIX_FMT_D3D11 decoder
    // texture, plus subtitle/OSD overlays. rotation_deg (0/90/180/270,
    // clockwise) comes from the stream's display matrix.
    bool render(AVFrame* f, const SubRender& overlays, int rotation_deg = 0);
    void resize();
    void shutdown();
    // Device for D3D11VA decoding (shared with rendering so decoder output
    // feeds the video processor with zero copies). Null when the device has
    // no video API (shader fallback) - callers then decode in software.
    ID3D11Device* decode_device();
    // ProcAmp picture controls (-100..100; VideoProcessor path only) and
    // aspect override (0 auto, 1 16:9, 2 4:3, 3 stretch, 4 crop-fill).
    void set_picture(int b, int c, int s, int h);
    void get_picture(int* b, int* c, int* s, int* h);
    void set_aspect(int mode);
    int aspect();
    void set_sub_scale(double s);  // 0.5..2 text size multiplier
    double sub_scale();
    ~VideoOut() { shutdown(); }

private:
    D3DState* d = nullptr;
    std::mutex m_;
};

// ---------------------------------------------------------------- player

struct Player {
    HWND hwnd = nullptr;
    std::wstring path;
    std::wstring error;               // set by demux thread on open failure
    std::atomic<bool> open_failed{false};

    AVFormatContext* fmt = nullptr;
    int vst = -1, ast = -1, sst = -1; // active stream indices (-1 = none)
    std::vector<int> audio_streams;
    std::vector<int> sub_streams;     // internal subtitle stream indices
    std::mutex tracks_m;              // guards the name vectors + chapters
    std::vector<std::wstring> audio_names;
    std::vector<std::wstring> sub_names;  // [external?, internals...]
    std::vector<std::pair<double, std::wstring>> chapters;  // start sec, title
    bool has_external_subs = false;
    int sub_choice = 0;               // index into effective sub track list; 0 = default
    AVCodecContext* vctx = nullptr;
    AVCodecContext* actx = nullptr;
    AVCodecContext* sctx = nullptr;
    std::atomic<int> rotation{0};     // display-matrix rotation, CW degrees

    PacketQueue vq, aq;
    std::atomic<int> vq_serial{0}, aq_serial{0}; // mirrors queue serials for consumers
    FrameQueue vfq{3}, afq{9};
    SubtitleList subs;

    AudioOut ao;
    VideoOut* vo = nullptr;           // owned by shell, shared across opens

    std::thread th_demux, th_vdec, th_adec, th_vrender;
    std::atomic<bool> abort{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> eof{false};
    std::atomic<bool> running{false};

    // seek request (guarded by seek_m)
    std::mutex seek_m;
    bool seek_req = false;
    double seek_to = 0;
    // Exact seek: av_seek_frame lands on the prior keyframe; frames and
    // audio before this pts (stream time) are decoded but dropped.
    std::atomic<double> precise_v{NAN}, precise_a{NAN};

    // host event callback (fires on engine threads)
    PlayerEventFn evt_fn = nullptr;
    void* evt_user = nullptr;
    std::atomic<bool> ended{false};
    bool ended_fired = false;         // demux thread only
    std::mutex err_m;                 // guards `error`

    // last presented frame, kept for repaint-while-paused
    std::mutex lastf_m;
    AVFrame* last_frame = nullptr;
    std::atomic<bool> redraw_req{false};
    std::atomic<bool> step_req{false};  // advance one frame while paused

    // transient OSD text (guarded by osd_m)
    std::mutex osd_m;
    std::wstring osd_text;
    int64_t osd_until = 0;            // av_gettime_relative() deadline
    std::wstring osd_now() {
        std::lock_guard<std::mutex> lk(osd_m);
        if (osd_text.empty() || av_gettime_relative() > osd_until) return L"";
        return osd_text;
    }

    void fire(PlayerEvent evt) {
        if (evt_fn) evt_fn(evt_user, evt);
    }

    // playback shaping
    std::atomic<double> speed{1.0};       // 0.25..4
    std::atomic<double> audio_delay{0};   // s; + = audio heard later
    std::atomic<double> sub_delay{0};     // s; + = subtitles shown later

    // debug HUD + counters (render thread computes fps itself)
    std::atomic<bool> hud{false};
    std::atomic<int> stat_drops{0};
    std::atomic<bool> hw_active{false};   // D3D11VA negotiated by get_format

    // external clock fallback when there is no audio stream
    std::mutex extclk_m;
    double extclk_pts = NAN;          // pts anchor
    int64_t extclk_time = 0;          // av_gettime_relative() at anchor

    std::atomic<double> vclock{NAN};  // pts of last presented video frame
    double duration = 0;
    double start_time = 0;            // fmt->start_time in seconds (or 0)

    // desired tracks for (re)open
    int want_audio_rel = 0;           // index into audio_streams
    double open_at = 0;               // start position for reopen

    double master_clock();            // audio if present, else external
    void extclk_set(double pts);
};

void demux_thread(Player* p);
void video_decode_thread(Player* p);
void audio_decode_thread(Player* p);
void video_render_thread(Player* p);
