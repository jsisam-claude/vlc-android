#include "player_int.h"
#include <cstdarg>

void log_line(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    buf[n] = '\n';
    buf[n + 1] = 0;
    OutputDebugStringA(buf);
    fputs(buf, stderr);
}

std::string wide_to_utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, 0);
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring utf8_to_wide(const char* s, int len) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, len, nullptr, 0);
    UINT cp = CP_UTF8;
    if (n <= 0) { // not valid UTF-8: fall back to the system ANSI codepage
        cp = CP_ACP;
        n = MultiByteToWideChar(cp, 0, s, len, nullptr, 0);
        if (n <= 0) return L"";
    }
    std::wstring w(n, 0);
    MultiByteToWideChar(cp, 0, s, len, w.data(), n);
    if (len == -1 && !w.empty() && w.back() == 0) w.pop_back();
    return w;
}

// ---------------------------------------------------------------- PacketQueue

void PacketQueue::push(AVPacket* src) {
    AVPacket* p = av_packet_alloc();
    av_packet_move_ref(p, src);
    std::lock_guard<std::mutex> lk(m_);
    bytes_ += p->size;
    q_.push_back({p, serial_.load(), false});
    cv_.notify_all();
}

bool PacketQueue::pop(Pkt& out, int timeout_ms) {
    std::unique_lock<std::mutex> lk(m_);
    if (!cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                      [&] { return abort_ || !q_.empty(); }))
        return false;
    if (abort_) return false;
    out = q_.front();
    q_.pop_front();
    if (out.p) bytes_ -= out.p->size;
    return true;
}

void PacketQueue::flush() {
    std::lock_guard<std::mutex> lk(m_);
    for (auto& e : q_)
        if (e.p) av_packet_free(&e.p);
    q_.clear();
    bytes_ = 0;
    serial_++;
    q_.push_back({nullptr, serial_.load(), true});
    cv_.notify_all();
}

void PacketQueue::set_abort(bool a) {
    std::lock_guard<std::mutex> lk(m_);
    abort_ = a;
    cv_.notify_all();
}

size_t PacketQueue::count() {
    std::lock_guard<std::mutex> lk(m_);
    return q_.size();
}

// ---------------------------------------------------------------- FrameQueue

bool FrameQueue::push(AVFrame* f, int serial, double pts, double dur) {
    std::unique_lock<std::mutex> lk(m_);
    cv_push_.wait(lk, [&] { return abort_ || q_.size() < max_; });
    if (abort_) return false;
    AVFrame* c = av_frame_alloc();
    av_frame_move_ref(c, f);
    q_.push_back({c, serial, pts, dur});
    cv_pop_.notify_all();
    return true;
}

bool FrameQueue::pop(FQFrame& out, int timeout_ms) {
    std::unique_lock<std::mutex> lk(m_);
    if (!cv_pop_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                          [&] { return abort_ || !q_.empty(); }))
        return false;
    if (abort_ || q_.empty()) return false;
    out = q_.front();
    q_.pop_front();
    cv_push_.notify_all();
    return true;
}

bool FrameQueue::peek(FQFrame& out) {
    std::lock_guard<std::mutex> lk(m_);
    if (q_.empty()) return false;
    out = q_.front();
    return true;
}

void FrameQueue::drop_front() {
    std::lock_guard<std::mutex> lk(m_);
    if (q_.empty()) return;
    av_frame_free(&q_.front().f);
    q_.pop_front();
    cv_push_.notify_all();
}

void FrameQueue::flush() {
    std::lock_guard<std::mutex> lk(m_);
    for (auto& e : q_) av_frame_free(&e.f);
    q_.clear();
    cv_push_.notify_all();
}

void FrameQueue::set_abort(bool a) {
    std::lock_guard<std::mutex> lk(m_);
    abort_ = a;
    cv_push_.notify_all();
    cv_pop_.notify_all();
}

size_t FrameQueue::count() {
    std::lock_guard<std::mutex> lk(m_);
    return q_.size();
}
