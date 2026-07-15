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
    if (d->client && d->started) d->client->Stop();
    if (d->volume) d->volume->Release();
    if (d->render) d->render->Release();
    if (d->client) d->client->Release();
    if (d->dev) d->dev->Release();
    if (d->enu) d->enu->Release();
    if (d->wf) CoTaskMemFree(d->wf);
    if (d->event) CloseHandle(d->event);
    delete d;
}

static bool wf_is_float(const WAVEFORMATEX* wf) {
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        return IsEqualGUID(ex->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
    }
    return false;
}

static MMDevice* mmdevice_open() {
    MMDevice* d = new MMDevice();
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&d->enu);
    if (SUCCEEDED(hr)) hr = d->enu->GetDefaultAudioEndpoint(eRender, eConsole, &d->dev);
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

bool AudioOut::start(FrameQueue* fq, const std::atomic<int>* pq_serial) {
    fq_ = fq;
    pq_serial_ = pq_serial;
    abort_ = false;
    flush_req_ = false;
    th_ = std::thread(&AudioOut::thread_main, this);
    return true;
}

void AudioOut::stop() {
    abort_ = true;
    if (th_.joinable()) th_.join();
}

void AudioOut::pause(bool paused) { paused_ = paused; }

void AudioOut::flush() { flush_req_ = true; }

double AudioOut::clock() {
    std::lock_guard<std::mutex> lk(clock_m_);
    return fifo_end_pts_;  // adjusted for buffered amount by the audio thread
}

void AudioOut::volume_step(int steps) {
    if (!dev_ || !dev_->volume) return;
    float v = 1.0f;
    dev_->volume->GetMasterVolume(&v);
    v = std::fmax(0.0f, std::fmin(1.0f, v + 0.05f * steps));
    dev_->volume->SetMasterVolume(v, nullptr);
}

void AudioOut::volume_set(float v) {
    if (!dev_ || !dev_->volume) return;
    dev_->volume->SetMasterVolume(std::fmax(0.0f, std::fmin(1.0f, v)), nullptr);
}

void AudioOut::set_mute(bool m) {
    if (dev_ && dev_->volume) dev_->volume->SetMute(m, nullptr);
}

bool AudioOut::muted() {
    BOOL m = FALSE;
    if (dev_ && dev_->volume) dev_->volume->GetMute(&m);
    return m != FALSE;
}

float AudioOut::volume() {
    float v = 1.0f;
    if (dev_ && dev_->volume) dev_->volume->GetMasterVolume(&v);
    return v;
}

void AudioOut::thread_main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    dev_ = mmdevice_open();
    if (!dev_) { CoUninitialize(); return; }

    DWORD task_idx = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", &task_idx);

    const int out_rate = dev_->wf->nSamplesPerSec;
    const int out_ch = dev_->wf->nChannels;
    fifo_ = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLT, out_ch, out_rate);
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, out_ch);

    double queued_pts = NAN;  // pts at the *end* of everything pushed into fifo
    bool client_started = false;
    bool device_paused = false;

    while (!abort_) {
        if (flush_req_) {
            flush_req_ = false;
            av_audio_fifo_reset(fifo_);
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

            AVFrame* f = fr.f;
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
                av_audio_fifo_write(fifo_, (void**)&outbuf, got);
                if (!std::isnan(fr.pts))
                    queued_pts = fr.pts + (double)f->nb_samples / f->sample_rate;
                else if (!std::isnan(queued_pts))
                    queued_pts += (double)got / out_rate;
            }
            av_freep(&outbuf);
            av_frame_free(&fr.f);
        }

        DWORD wr = WaitForSingleObject(dev_->event, 100);
        (void)wr;
        UINT32 padding = 0;
        if (FAILED(dev_->client->GetCurrentPadding(&padding))) continue;
        UINT32 want = dev_->buffer_frames - padding;
        if (want == 0) continue;

        BYTE* buf = nullptr;
        if (FAILED(dev_->render->GetBuffer(want, &buf))) continue;
        int have = av_audio_fifo_size(fifo_);
        int take = (int)((UINT32)have < want ? (UINT32)have : want);
        if (take > 0) av_audio_fifo_read(fifo_, (void**)&buf, take);
        if ((UINT32)take < want)
            memset(buf + (size_t)take * out_ch * 4, 0, ((size_t)want - take) * out_ch * 4);
        dev_->render->ReleaseBuffer(want, 0);

        if (!client_started && take > 0) {
            dev_->client->Start();
            client_started = true;
            dev_->started = true;
        }

        // clock = pts at fifo end, minus what is still buffered (fifo + device)
        if (!std::isnan(queued_pts)) {
            double buffered = (double)av_audio_fifo_size(fifo_) / out_rate +
                              (double)(padding + (want - take)) / out_rate;
            std::lock_guard<std::mutex> lk(clock_m_);
            fifo_end_pts_ = queued_pts - buffered;
        }
    }

    swr_free(&swr_);
    av_channel_layout_uninit(&in_layout_);
    if (fifo_) av_audio_fifo_free(fifo_);
    fifo_ = nullptr;
    if (task) AvRevertMmThreadCharacteristics(task);
    mmdevice_free(dev_);
    dev_ = nullptr;
    CoUninitialize();
}
