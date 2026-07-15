// D3D11 video output. The GPU driver does the heavy lifting via
// ID3D11VideoProcessor (NV12->RGB, scaling, colorspace); setup follows
// Microsoft's MIT-licensed DX11VideoRenderer sample and VLC's d3d11 output.
// CPU frames are converted to NV12 with swscale and uploaded; the same
// blit path will later accept D3D11VA decoder textures directly.
// Subtitles are drawn with Direct2D/DirectWrite onto the backbuffer.
#include "player_int.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1.h>
#include <dwrite.h>

template <class T> static void safe_release(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

struct D3DState {
    HWND hwnd = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain1* swap = nullptr;
    ID3D11VideoDevice* vdev = nullptr;
    ID3D11VideoContext* vctx = nullptr;
    ID3D11VideoProcessorEnumerator* vpe = nullptr;
    ID3D11VideoProcessor* vp = nullptr;
    ID3D11Texture2D* in_tex = nullptr;
    ID3D11VideoProcessorInputView* in_view = nullptr;
    int in_w = 0, in_h = 0;
    int out_w = 0, out_h = 0;

    ID2D1Factory* d2df = nullptr;
    ID2D1RenderTarget* d2drt = nullptr;
    IDWriteFactory* dwf = nullptr;
    IDWriteTextFormat* text_fmt = nullptr;
    int text_fmt_px = 0;

    SwsContext* sws = nullptr;
    uint8_t* nv12 = nullptr;
    int nv12_pitch = 0;
    size_t nv12_size = 0;
};

static bool create_device_swapchain(D3DState* d) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   nullptr, 0, D3D11_SDK_VERSION, &d->dev, &fl, &d->ctx);
    if (FAILED(hr)) {
        log_line("video: hardware D3D11 device failed (0x%08lx), trying WARP", hr);
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                               nullptr, 0, D3D11_SDK_VERSION, &d->dev, &fl, &d->ctx);
    }
    if (FAILED(hr)) return false;

    IDXGIDevice* dxgi_dev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    hr = d->dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_dev);
    if (SUCCEEDED(hr)) hr = dxgi_dev->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
    if (SUCCEEDED(hr)) {
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hr = factory->CreateSwapChainForHwnd(d->dev, d->hwnd, &sd, nullptr, nullptr, &d->swap);
        if (SUCCEEDED(hr)) factory->MakeWindowAssociation(d->hwnd, DXGI_MWA_NO_ALT_ENTER);
    }
    safe_release(factory);
    safe_release(adapter);
    safe_release(dxgi_dev);
    if (FAILED(hr)) return false;

    hr = d->dev->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&d->vdev);
    if (SUCCEEDED(hr)) hr = d->ctx->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&d->vctx);
    if (FAILED(hr)) { log_line("video: no D3D11 video device support"); return false; }

    RECT rc;
    GetClientRect(d->hwnd, &rc);
    d->out_w = rc.right > 0 ? rc.right : 1;
    d->out_h = rc.bottom > 0 ? rc.bottom : 1;

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED,
                                 __uuidof(ID2D1Factory), nullptr, (void**)&d->d2df)))
        d->d2df = nullptr;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   (IUnknown**)&d->dwf)))
        d->dwf = nullptr;
    return true;
}

static bool ensure_video_processor(D3DState* d, int w, int h) {
    if (d->vp && w == d->in_w && h == d->in_h) return true;
    safe_release(d->in_view);
    safe_release(d->in_tex);
    safe_release(d->vp);
    safe_release(d->vpe);

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = w;
    cd.InputHeight = h;
    cd.OutputWidth = d->out_w;
    cd.OutputHeight = d->out_h;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (FAILED(d->vdev->CreateVideoProcessorEnumerator(&cd, &d->vpe))) return false;
    if (FAILED(d->vdev->CreateVideoProcessor(d->vpe, 0, &d->vp))) return false;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(d->dev->CreateTexture2D(&td, nullptr, &d->in_tex))) return false;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
    ivd.FourCC = 0;
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.MipSlice = 0;
    if (FAILED(d->vdev->CreateVideoProcessorInputView(d->in_tex, d->vpe, &ivd, &d->in_view)))
        return false;

    d->in_w = w;
    d->in_h = h;
    return true;
}

static void set_colorspace(D3DState* d, const AVFrame* f) {
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs = {};
    bool bt709 = f->height >= 720;
    if (f->colorspace == AVCOL_SPC_BT709) bt709 = true;
    else if (f->colorspace == AVCOL_SPC_BT470BG || f->colorspace == AVCOL_SPC_SMPTE170M) bt709 = false;
    cs.YCbCr_Matrix = bt709 ? 1 : 0;
    cs.Nominal_Range = (f->color_range == AVCOL_RANGE_JPEG)
                           ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
                           : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    d->vctx->VideoProcessorSetStreamColorSpace(d->vp, 0, &cs);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs = {};
    out_cs.RGB_Range = 0;  // full-range RGB out
    d->vctx->VideoProcessorSetOutputColorSpace(d->vp, &out_cs);
}

static RECT letterbox(int vw, int vh, int ww, int wh) {
    RECT r;
    if (vw <= 0 || vh <= 0 || ww <= 0 || wh <= 0) { SetRect(&r, 0, 0, ww, wh); return r; }
    double scale = std::fmin((double)ww / vw, (double)wh / vh);
    int dw = (int)(vw * scale + 0.5), dh = (int)(vh * scale + 0.5);
    int x = (ww - dw) / 2, y = (wh - dh) / 2;
    SetRect(&r, x, y, x + dw, y + dh);
    return r;
}

static void draw_subtitle(D3DState* d, ID3D11Texture2D* backbuffer, const std::wstring& text) {
    if (!d->d2df || !d->dwf || text.empty()) return;

    if (!d->d2drt) {
        IDXGISurface* surf = nullptr;
        if (FAILED(backbuffer->QueryInterface(__uuidof(IDXGISurface), (void**)&surf))) return;
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        HRESULT hr = d->d2df->CreateDxgiSurfaceRenderTarget(surf, &props, &d->d2drt);
        surf->Release();
        if (FAILED(hr)) { d->d2drt = nullptr; return; }
    }

    int px = d->out_h / 18;
    if (px < 14) px = 14;
    if (!d->text_fmt || d->text_fmt_px != px) {
        safe_release(d->text_fmt);
        d->dwf->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                 (float)px, L"", &d->text_fmt);
        if (!d->text_fmt) return;
        d->text_fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        d->text_fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_FAR);
        d->text_fmt_px = px;
    }

    float margin = d->out_w * 0.05f;
    D2D1_RECT_F box = D2D1::RectF(margin, 0.0f, d->out_w - margin, d->out_h - px * 0.75f);

    ID2D1SolidColorBrush *black = nullptr, *white = nullptr;
    d->d2drt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.9f), &black);
    d->d2drt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1.0f), &white);
    if (!black || !white) { safe_release(black); safe_release(white); return; }

    d->d2drt->BeginDraw();
    const float o = px / 14.0f;  // shadow/outline offset
    const D2D1_POINT_2F offs[] = {{-o, 0}, {o, 0}, {0, -o}, {0, o}, {o, o}};
    for (auto& off : offs) {
        D2D1_RECT_F b = box;
        b.left += off.x; b.right += off.x; b.top += off.y; b.bottom += off.y;
        d->d2drt->DrawTextW(text.c_str(), (UINT32)text.size(), d->text_fmt, b, black);
    }
    d->d2drt->DrawTextW(text.c_str(), (UINT32)text.size(), d->text_fmt, box, white);
    d->d2drt->EndDraw();
    black->Release();
    white->Release();
}

bool VideoOut::init(HWND hwnd) {
    std::lock_guard<std::mutex> lk(m_);
    if (d) return true;
    d = new D3DState();
    d->hwnd = hwnd;
    if (!create_device_swapchain(d)) {
        shutdown();
        return false;
    }
    return true;
}

void VideoOut::resize() {
    std::lock_guard<std::mutex> lk(m_);
    if (!d || !d->swap) return;
    RECT rc;
    GetClientRect(d->hwnd, &rc);
    int w = rc.right > 0 ? rc.right : 1;
    int h = rc.bottom > 0 ? rc.bottom : 1;
    if (w == d->out_w && h == d->out_h) return;
    safe_release(d->d2drt);
    d->swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    d->out_w = w;
    d->out_h = h;
    // force VP re-creation with new output size
    d->in_w = d->in_h = 0;
}

bool VideoOut::render(AVFrame* f, const std::wstring& subtitle) {
    std::lock_guard<std::mutex> lk(m_);
    if (!d || !d->swap || !f) return false;

    int w = f->width, h = f->height;
    if (w <= 0 || h <= 0) return false;

    // Convert to NV12 (contiguous Y then interleaved UV, single pitch).
    int pitch = (w + 127) & ~127;
    size_t need = (size_t)pitch * h * 3 / 2;
    if (need > d->nv12_size) {
        av_free(d->nv12);
        d->nv12 = (uint8_t*)av_malloc(need);
        d->nv12_size = d->nv12 ? need : 0;
        d->nv12_pitch = pitch;
    }
    if (!d->nv12) return false;
    d->sws = sws_getCachedContext(d->sws, w, h, (AVPixelFormat)f->format,
                                  w, h, AV_PIX_FMT_NV12, SWS_BILINEAR,
                                  nullptr, nullptr, nullptr);
    if (!d->sws) return false;
    uint8_t* dst[2] = {d->nv12, d->nv12 + (size_t)pitch * h};
    int dst_stride[2] = {pitch, pitch};
    sws_scale(d->sws, f->data, f->linesize, 0, h, dst, dst_stride);

    if (!ensure_video_processor(d, w, h)) return false;
    d->ctx->UpdateSubresource(d->in_tex, 0, nullptr, d->nv12, pitch, 0);
    set_colorspace(d, f);

    ID3D11Texture2D* back = nullptr;
    if (FAILED(d->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)))
        return false;

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ID3D11VideoProcessorOutputView* out_view = nullptr;
    HRESULT hr = d->vdev->CreateVideoProcessorOutputView(back, d->vpe, &ovd, &out_view);
    if (SUCCEEDED(hr)) {
        RECT src = {0, 0, w, h};
        int disp_w = w, disp_h = h;
        if (f->sample_aspect_ratio.num > 0 && f->sample_aspect_ratio.den > 0)
            disp_w = (int)av_rescale(w, f->sample_aspect_ratio.num, f->sample_aspect_ratio.den);
        RECT dst_rc = letterbox(disp_w, disp_h, d->out_w, d->out_h);
        RECT out_rc = {0, 0, d->out_w, d->out_h};
        d->vctx->VideoProcessorSetStreamSourceRect(d->vp, 0, TRUE, &src);
        d->vctx->VideoProcessorSetStreamDestRect(d->vp, 0, TRUE, &dst_rc);
        d->vctx->VideoProcessorSetOutputTargetRect(d->vp, TRUE, &out_rc);
        D3D11_VIDEO_COLOR black = {};
        black.YCbCr.Y = 0.0625f;
        black.YCbCr.Cb = 0.5f;
        black.YCbCr.Cr = 0.5f;
        black.YCbCr.A = 1.0f;
        d->vctx->VideoProcessorSetOutputBackgroundColor(d->vp, TRUE, &black);

        D3D11_VIDEO_PROCESSOR_STREAM stream = {};
        stream.Enable = TRUE;
        stream.pInputSurface = d->in_view;
        hr = d->vctx->VideoProcessorBlt(d->vp, out_view, 0, 1, &stream);
        out_view->Release();
    }
    if (SUCCEEDED(hr)) draw_subtitle(d, back, subtitle);
    back->Release();
    if (FAILED(hr)) {
        log_line("video: VideoProcessorBlt failed 0x%08lx", hr);
        return false;
    }
    d->swap->Present(1, 0);
    return true;
}

void VideoOut::shutdown() {
    if (!d) return;
    safe_release(d->text_fmt);
    safe_release(d->d2drt);
    safe_release(d->dwf);
    safe_release(d->d2df);
    safe_release(d->in_view);
    safe_release(d->in_tex);
    safe_release(d->vp);
    safe_release(d->vpe);
    safe_release(d->vctx);
    safe_release(d->vdev);
    safe_release(d->swap);
    safe_release(d->ctx);
    safe_release(d->dev);
    sws_freeContext(d->sws);
    av_free(d->nv12);
    delete d;
    d = nullptr;
}
