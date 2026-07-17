// WASAPI shared-mode, event-driven audio output with an audio-master clock.
// Structure adapted from mpv's audio/out/ao_wasapi.c and VLC's
// modules/audio_output/mmdevice.c (both LGPL-2.1+), heavily simplified:
// shared mode only, mix-format only, default endpoint only.
#include "player_int.h"

#include <initguid.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <cmath>

struct MMDevice {
    IMMDeviceEnumerator* enu = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    ISimpleAudioVolume* volume = nullptr;
    WAVEFORMATEX* wf = nullptr;
    HANDLE event = nullptr;
    UINT32 buffer_frames = 0;
    bool started = false;
};

static void mmdevice_free(MMDevice* d) {
    if (!d) return;
    if (d->client && d->started) {
        d->client->Stop();
        d->client->Reset();  // discard the ~200ms still buffered so the tail
                             // of this file can't play over the next one
    }
    if (d->volume) d->volume->Release();
    if (d->render) d->render->Release();
    if (d->client) d->client->Release();
    if (d->dev) d->dev->Release();
    if (d->enu) d->enu->Release();
    if (d->wf) CoTaskMemFree(d->wf);
    if (d->event) CloseHandle(d->event);
    delete d;
}

// Waveform-similarity overlap-add (WSOLA) time stretcher: changes tempo
// while preserving pitch. Fixed synthesis hop of half a window; each step
// picks the analysis window (within a small search range around the
// nominal speed-scaled position) that best correlates with the previous
// output tail, then crossfades. Interleaved float, any channel count.
class TimeStretch {
public:
    void init(int channels, int rate) {
        ch_ = channels > 0 ? channels : 2;
        win_ = rate / 25;  // 40ms window
        if (win_ < 256) win_ = 256;
        win_ &= ~1;
        half_ = win_ / 2;  // synthesis hop == overlap
        seek_ = rate / 100;  // +-10ms similarity search
        reset();
    }
    void reset() {
        in_.clear();
        tail_.assign((size_t)half_ * ch_, 0.0f);
        primed_ = false;
        src_ = 0;
    }
    // Consumes nsamples interleaved floats, appends stretched audio to out.
    void process(const float* in, int nsamples, double speed,
                 std::vector<float>& out) {
        if (speed > 0.99 && speed < 1.01) {  // unity: bypass
            if (primed_) reset();
            out.insert(out.end(), in, in + (size_t)nsamples * ch_);
            return;
        }
        in_.insert(in_.end(), in, in + (size_t)nsamples * ch_);
        if (!primed_) {
            if ((int)(in_.size() / ch_) < half_) return;
            memcpy(tail_.data(), in_.data(), (size_t)half_ * ch_ * sizeof(float));
            src_ = half_;
            primed_ = true;
        }
        for (;;) {
            int base = (int)(src_ + 0.5) - seek_;
            if (base < 0) base = 0;
            int have = (int)(in_.size() / ch_);
            if (base + 2 * seek_ + win_ > have) break;

            int best = best_offset(base, have);
            const float* seg = in_.data() + (size_t)best * ch_;
            for (int i = 0; i < half_; i++) {
                float wf = (float)i / half_;
                for (int c = 0; c < ch_; c++)
                    out.push_back(tail_[(size_t)i * ch_ + c] * (1.0f - wf) +
                                  seg[(size_t)i * ch_ + c] * wf);
            }
            memcpy(tail_.data(), seg + (size_t)half_ * ch_,
                   (size_t)half_ * ch_ * sizeof(float));
            src_ += half_ * speed;

            int drop = (int)src_ - seek_ - 1;  // keep the search slack behind
            if (drop > 0) {
                if (drop > have) drop = have;
                in_.erase(in_.begin(), in_.begin() + (size_t)drop * ch_);
                src_ -= drop;
            }
        }
    }

private:
    int best_offset(int base, int have) {
        int bestoff = base;
        float bestscore = -1e30f;
        for (int off = base; off <= base + 2 * seek_; off += 2) {
            if (off + win_ > have) break;
            float score = 0;
            for (int i = 0; i < half_; i += 4) {  // mono sum, coarse stride
                float a = 0, b = 0;
                for (int c = 0; c < ch_; c++) {
                    a += tail_[(size_t)i * ch_ + c];
                    b += in_[(size_t)(off + i) * ch_ + c];
                }
                score += a * b;
            }
            if (score > bestscore) {
                bestscore = score;
                bestoff = off;
            }
        }
        return bestoff;
    }
    int ch_ = 2, win_ = 0, half_ = 0, seek_ = 0;
    bool primed_ = false;
    double src_ = 0;
    std::vector<float> in_, tail_;
};

static bool wf_is_float(const WAVEFORMATEX* wf) {
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        return IsEqualGUID(ex->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
    }
    return false;
}

static MMDevice* mmdevice_open(const std::wstring& want_id) {
    MMDevice* d = new MMDevice();
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&d->enu);
    if (SUCCEEDED(hr)) {
        if (!want_id.empty())
            hr = d->enu->GetDevice(want_id.c_str(), &d->dev);
        if (want_id.empty() || FAILED(hr))  // picked device gone: use default
            hr = d->enu->GetDefaultAudioEndpoint(eRender, eConsole, &d->dev);
    }
    if (SUCCEEDED(hr)) hr = d->dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                             (void**)&d->client);
    if (SUCCEEDED(hr)) hr = d->client->GetMixFormat(&d->wf);
    if (SUCCEEDED(hr) && !wf_is_float(d->wf)) hr = E_FAIL;  // mix format is float in practice
    if (SUCCEEDED(hr))
        hr = d->client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                   AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                   2000000 /*200ms*/, 0, d->wf, nullptr);
    if (SUCCEEDED(hr)) {
        d->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = d->client->SetEventHandle(d->event);
    }
    if (SUCCEEDED(hr)) hr = d->client->GetBufferSize(&d->buffer_frames);
    if (SUCCEEDED(hr)) hr = d->client->GetService(__uuidof(IAudioRenderClient),
                                                  (void**)&d->render);
    if (SUCCEEDED(hr)) d->client->GetService(__uuidof(ISimpleAudioVolume),
                                             (void**)&d->volume);
    if (FAILED(hr)) {
        log_line("audio: WASAPI init failed hr=0x%08lx", hr);
        mmdevice_free(d);
        return nullptr;
    }
    return d;
}

bool AudioOut::start(FrameQueue* fq, const std::atomic<int>* pq_serial,
                     std::atomic<double>* drop_until) {
    fq_ = fq;
    pq_serial_ = pq_serial;
    drop_until_ = drop_until;
    abort_ = false;
    flush_req_ = false;
    {
        // Drop any clock left over from the previous file. Until this file's
        // audio thread produces a real timestamp, master_clock() must report
        // NAN so the video render thread paces against its own pts instead of
        // the old file's clock (which would free-run or mis-drop the opening
        // frames of the new file — a visible desync right after a switch).
        std::lock_guard<std::mutex> lk(clock_m_);
        fifo_end_pts_ = NAN;
    }
    th_ = std::thread(&AudioOut::thread_main, this);
    return true;
}

void AudioOut::stop() {
    abort_ = true;
    if (th_.joinable()) th_.join();
    std::lock_guard<std::mutex> lk(clock_m_);
    fifo_end_pts_ = NAN;  // no lingering clock between files
}

void AudioOut::pause(bool paused) { paused_ = paused; }

void AudioOut::flush() { flush_req_ = true; }

double AudioOut::clock() {
    std::lock_guard<std::mutex> lk(clock_m_);
    return fifo_end_pts_;  // adjusted for buffered amount by the audio thread
}

void AudioOut::volume_step(int steps) {
    volume_set(volume() + 0.05f * steps);
}

// 0..1 maps to the WASAPI session volume; 1..2 pins the session at 1 and
// applies software gain (with a soft clipper) - a VLC-style loudness boost.
void AudioOut::volume_set(float v) {
    v = std::fmax(0.0f, std::fmin(2.0f, v));
    float session = v <= 1.0f ? v : 1.0f;
    gain_ = v <= 1.0f ? 1.0f : v;
    want_vol_ = session;
    std::lock_guard<std::mutex> lk(dev_m_);
    if (dev_ && dev_->volume) dev_->volume->SetMasterVolume(session, nullptr);
}

void AudioOut::set_mute(bool m) {
    want_mute_ = m ? 1 : 0;
    std::lock_guard<std::mutex> lk(dev_m_);
    if (dev_ && dev_->volume) dev_->volume->SetMute(m, nullptr);
}

bool AudioOut::muted() {
    std::lock_guard<std::mutex> lk(dev_m_);
    if (dev_ && dev_->volume) {
        BOOL m = FALSE;
        dev_->volume->GetMute(&m);
        return m != FALSE;
    }
    return want_mute_ > 0;  // no device yet: report the pending choice
}

float AudioOut::volume() {
    float g = gain_;
    if (g > 1.0f) return g;  // boosted: session is pinned at 1
    std::lock_guard<std::mutex> lk(dev_m_);
    if (dev_ && dev_->volume) {
        float v = 1.0f;
        dev_->volume->GetMasterVolume(&v);
        return v;
    }
    float w = want_vol_;
    return w >= 0 ? w : 1.0f;
}

// ------------------------------------------- endpoint enumeration (API)
// Standalone helpers behind the public player.h device functions; the
// caller's thread must have COM initialized (any apartment).

static IMMDeviceCollection* enum_endpoints(IMMDeviceEnumerator** enu_out) {
    IMMDeviceEnumerator* enu = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&enu)))
        return nullptr;
    IMMDeviceCollection* col = nullptr;
    if (FAILED(enu->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
        enu->Release();
        return nullptr;
    }
    *enu_out = enu;
    return col;
}

int player_audio_device_count(void) {
    IMMDeviceEnumerator* enu = nullptr;
    IMMDeviceCollection* col = enum_endpoints(&enu);
    if (!col) return 0;
    UINT n = 0;
    col->GetCount(&n);
    col->Release();
    enu->Release();
    return (int)n;
}

void player_audio_device_name(int i, wchar_t* buf, size_t buflen) {
    if (!buf || !buflen) return;
    buf[0] = 0;
    IMMDeviceEnumerator* enu = nullptr;
    IMMDeviceCollection* col = enum_endpoints(&enu);
    if (!col) return;
    IMMDevice* dev = nullptr;
    if (SUCCEEDED(col->Item((UINT)i, &dev))) {
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) &&
                v.vt == VT_LPWSTR) {
                wcsncpy(buf, v.pwszVal, buflen - 1);
                buf[buflen - 1] = 0;
            }
            PropVariantClear(&v);
            ps->Release();
        }
        dev->Release();
    }
    col->Release();
    enu->Release();
}

void player_audio_device_id(int i, wchar_t* buf, size_t buflen) {
    if (!buf || !buflen) return;
    buf[0] = 0;
    IMMDeviceEnumerator* enu = nullptr;
    IMMDeviceCollection* col = enum_endpoints(&enu);
    if (!col) return;
    IMMDevice* dev = nullptr;
    if (SUCCEEDED(col->Item((UINT)i, &dev))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id)) && id) {
            wcsncpy(buf, id, buflen - 1);
            buf[buflen - 1] = 0;
            CoTaskMemFree(id);
        }
        dev->Release();
    }
    col->Release();
    enu->Release();
}

void player_set_audio_device(Player* p, const wchar_t* id) {
    p->ao.set_device(id);
}

void player_audio_device_current(Player* p, wchar_t* buf, size_t buflen) {
    if (!buf || !buflen) return;
    std::wstring cur = p->ao.device();
    wcsncpy(buf, cur.c_str(), buflen - 1);
    buf[buflen - 1] = 0;
}

void AudioOut::thread_main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task_idx = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", &task_idx);

    // Each run_device() spans one device lifetime; when the endpoint goes
    // away (unplugged, default output switched) we reopen the new default
    // and continue. The clock freezes across the gap, so video waits.
    while (!abort_) {
        if (!run_device()) break;
        Sleep(500);
    }

    if (task) AvRevertMmThreadCharacteristics(task);
    CoUninitialize();
}

void AudioOut::set_device(const wchar_t* id) {
    {
        std::lock_guard<std::mutex> lk(dev_m_);
        want_dev_ = id ? id : L"";
    }
    dev_change_ = true;  // playing: the thread reopens on the new endpoint
}

std::wstring AudioOut::device() {
    std::lock_guard<std::mutex> lk(dev_m_);
    return want_dev_;
}

void AudioOut::set_speed(double s) { speed_ = s; }

bool AudioOut::tap(float* out, int n) {
    std::lock_guard<std::mutex> lk(tap_m_);
    if (!tap_filled_ || n <= 0 || (size_t)n > tap_ring_.size()) return false;
    size_t sz = tap_ring_.size();
    size_t start = (tap_pos_ + sz - (size_t)n) % sz;
    for (int i = 0; i < n; i++) out[i] = tap_ring_[(start + i) % sz];
    return true;
}

bool AudioOut::run_device() {
    std::wstring want;
    {
        std::lock_guard<std::mutex> lk(dev_m_);
        want = want_dev_;
    }
    dev_change_ = false;
    MMDevice* d = mmdevice_open(want);
    {
        std::lock_guard<std::mutex> lk(dev_m_);
        dev_ = d;
    }
    if (!d) return !abort_;  // no endpoint right now: retry until abort

    // Session volume/mute are per device; carry the user's choice over.
    if (want_vol_ >= 0 && d->volume) d->volume->SetMasterVolume(want_vol_, nullptr);
    if (want_mute_ >= 0 && d->volume) d->volume->SetMute(want_mute_ > 0, nullptr);

    const int out_rate = dev_->wf->nSamplesPerSec;
    const int out_ch = dev_->wf->nChannels;
    fifo_ = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLT, out_ch, out_rate);
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, out_ch);

    double queued_pts = NAN;  // pts at the *end* of everything pushed into fifo
    bool client_started = false;
    bool device_paused = false;
    bool lost = false;
    HRESULT hr;

    TimeStretch stretch;
    stretch.init(out_ch, out_rate);
    std::vector<float> stretched;
    double spd_used = 1.0;
    while (!abort_ && !lost) {
        if (dev_change_) { lost = true; continue; }  // reopen on new endpoint
        if (flush_req_) {
            flush_req_ = false;
            av_audio_fifo_reset(fifo_);
            stretch.reset();
            queued_pts = NAN;
            std::lock_guard<std::mutex> lk(clock_m_);
            fifo_end_pts_ = NAN;
        }

        if (paused_) {
            if (client_started && !device_paused) { dev_->client->Stop(); device_paused = true; }
            Sleep(20);
            continue;
        }
        if (device_paused) { dev_->client->Start(); device_paused = false; }

        // Keep at least ~200ms in the FIFO.
        while (av_audio_fifo_size(fifo_) < out_rate / 5 && !abort_ && !flush_req_) {
            FQFrame fr;
            if (!fq_->pop(fr, 10)) break;
            if (fr.serial != pq_serial_->load()) { av_frame_free(&fr.f); continue; }

            if (drop_until_) {  // exact seek: skip audio before the target
                double pa = drop_until_->load();
                if (!std::isnan(pa)) {
                    double end = !std::isnan(fr.pts) && fr.f->sample_rate > 0
                                     ? fr.pts + (double)fr.f->nb_samples / fr.f->sample_rate
                                     : NAN;
                    if (!std::isnan(end) && end <= pa) { av_frame_free(&fr.f); continue; }
                    drop_until_->store(NAN);
                }
            }

            AVFrame* f = fr.f;
            double spd = speed_.load();
            if (spd < 0.25) spd = 0.25;
            if (spd > 4.0) spd = 4.0;
            if (!swr_ || in_rate_ != f->sample_rate || in_fmt_ != f->format ||
                av_channel_layout_compare(&in_layout_, &f->ch_layout)) {
                swr_free(&swr_);
                av_channel_layout_uninit(&in_layout_);
                av_channel_layout_copy(&in_layout_, &f->ch_layout);
                in_rate_ = f->sample_rate;
                in_fmt_ = f->format;
                swr_alloc_set_opts2(&swr_, &out_layout, AV_SAMPLE_FMT_FLT, out_rate,
                                    &f->ch_layout, (AVSampleFormat)f->format,
                                    f->sample_rate, 0, nullptr);
                if (!swr_ || swr_init(swr_) < 0) {
                    log_line("audio: swr_init failed");
                    swr_free(&swr_);
                    av_frame_free(&fr.f);
                    continue;
                }
            }

            uint8_t* outbuf = nullptr;
            int max_out = (int)av_rescale_rnd(f->nb_samples + 256, out_rate,
                                              f->sample_rate, AV_ROUND_UP);
            av_samples_alloc(&outbuf, nullptr, out_ch, max_out, AV_SAMPLE_FMT_FLT, 0);
            int got = swr_convert(swr_, &outbuf, max_out,
                                  (const uint8_t**)f->extended_data, f->nb_samples);
            if (got > 0) {
                spd_used = spd;
                stretched.clear();
                stretch.process((const float*)outbuf, got, spd, stretched);
                float g = gain_.load();
                if (g != 1.0f) {
                    for (auto& x : stretched) {
                        x *= g;
                        if (g > 1.0f) {  // cubic soft clip: +-1.5 maps to +-1
                            if (x > 1.5f) x = 1.0f;
                            else if (x < -1.5f) x = -1.0f;
                            else x = x - 0.14815f * x * x * x;
                        }
                    }
                }
                int wrote = (int)(stretched.size() / out_ch);
                if (wrote > 0) {
                    void* p = stretched.data();
                    av_audio_fifo_write(fifo_, (void**)&p, wrote);
                }
                if (!std::isnan(fr.pts))
                    queued_pts = fr.pts + (double)f->nb_samples / f->sample_rate;
                else if (!std::isnan(queued_pts))
                    queued_pts += (double)got / out_rate;  // 1:1 media seconds
            }
            av_freep(&outbuf);
            av_frame_free(&fr.f);
        }

        DWORD wr = WaitForSingleObject(dev_->event, 100);
        (void)wr;
        UINT32 padding = 0;
        hr = dev_->client->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                log_line("audio: device invalidated, reopening default endpoint");
                lost = true;
            }
            continue;
        }
        UINT32 want = dev_->buffer_frames - padding;
        if (want == 0) continue;

        BYTE* buf = nullptr;
        hr = dev_->render->GetBuffer(want, &buf);
        if (FAILED(hr)) {
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) lost = true;
            continue;
        }
        int have = av_audio_fifo_size(fifo_);
        int take = (int)((UINT32)have < want ? (UINT32)have : want);
        if (take > 0) av_audio_fifo_read(fifo_, (void**)&buf, take);
        if (take > 0) {  // feed the visualization tap (mono mixdown)
            std::lock_guard<std::mutex> lk(tap_m_);
            const float* s = (const float*)buf;
            for (int i = 0; i < take; i++) {
                float m = 0;
                for (int c = 0; c < out_ch; c++) m += s[(size_t)i * out_ch + c];
                tap_ring_[tap_pos_] = m / out_ch;
                tap_pos_ = (tap_pos_ + 1) % tap_ring_.size();
            }
            tap_filled_ = true;
        }
        if ((UINT32)take < want)
            memset(buf + (size_t)take * out_ch * 4, 0, ((size_t)want - take) * out_ch * 4);
        dev_->render->ReleaseBuffer(want, 0);

        if (!client_started && take > 0) {
            dev_->client->Start();
            client_started = true;
            dev_->started = true;
        }

        // clock = pts at fifo end, minus what is still buffered (fifo +
        // device), converted from real seconds to media seconds by speed
        if (!std::isnan(queued_pts)) {
            double buffered = (double)av_audio_fifo_size(fifo_) / out_rate +
                              (double)(padding + (want - take)) / out_rate;
            std::lock_guard<std::mutex> lk(clock_m_);
            fifo_end_pts_ = queued_pts - buffered * spd_used;
        }
    }

    swr_free(&swr_);
    av_channel_layout_uninit(&in_layout_);
    if (fifo_) av_audio_fifo_free(fifo_);
    fifo_ = nullptr;
    {
        std::lock_guard<std::mutex> lk(dev_m_);
        mmdevice_free(dev_);
        dev_ = nullptr;
    }
    return lost && !abort_;
}
