// D3D11 video output. The GPU driver does the heavy lifting via
// ID3D11VideoProcessor (NV12->RGB, scaling, colorspace); setup follows
// Microsoft's MIT-licensed DX11VideoRenderer sample and VLC's d3d11 output.
// CPU frames are converted to NV12 with swscale and uploaded; the same
// blit path will later accept D3D11VA decoder textures directly.
// Subtitles are drawn with Direct2D/DirectWrite onto the backbuffer.
#include "player_int.h"

#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <d2d1.h>
#include <dwrite.h>

extern "C" {
#include <libavutil/pixdesc.h>
}

template <class T> static void safe_release(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

static wchar_t g_vo_err[256];
const wchar_t* vo_init_error() { return g_vo_err; }
static bool fail_step(const wchar_t* step, HRESULT hr) {
    swprintf(g_vo_err, 256, L"%s failed (hr=0x%08X)", step, (unsigned)hr);
    log_line("video: %ls", g_vo_err);
    return false;
}

struct D3DState {
    HWND hwnd = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain1* swap = nullptr;
    ID3D11VideoDevice* vdev = nullptr;
    ID3D11VideoContext* vctx = nullptr;
    ID3D11VideoContext1* vctx1 = nullptr;  // colorspace1 (BT.2020/PQ); optional
    ID3D11VideoProcessorEnumerator* vpe = nullptr;
    ID3D11VideoProcessor* vp = nullptr;
    bool vp_can_rotate = false;
    // ProcAmp: user values -100..100 and the driver's ranges (b, c, h, s)
    int pic[4] = {0, 0, 0, 0};
    bool pic_ok[4] = {};
    D3D11_VIDEO_PROCESSOR_FILTER_RANGE pic_range[4] = {};
    int aspect_mode = 0;  // 0 auto, 1 16:9, 2 4:3, 3 stretch, 4 crop-fill
    double sub_scale = 1.0;  // subtitle text size multiplier, 0.5..2
    ID3D11Texture2D* in_tex = nullptr;
    ID3D11VideoProcessorInputView* in_view = nullptr;
    bool staging_p010 = false;   // staging texture format (P010 vs NV12)
    bool no_p010 = false;        // driver rejected P010; stay on NV12
    int in_w = 0, in_h = 0;
    int out_w = 0, out_h = 0;

    // shader fallback path (devices without the D3D11 video API)
    bool use_vp = false;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11SamplerState* samp = nullptr;
    ID3D11Buffer* cbuf = nullptr;  // uv transform (rotation)
    int cbuf_rot = -1;
    ID3D11Texture2D* rgb_tex = nullptr;
    ID3D11ShaderResourceView* rgb_srv = nullptr;
    int rgb_w = 0, rgb_h = 0;

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

// Fullscreen-triangle blit of a BGRA texture; scaling via the sampler,
// letterboxing via the viewport. Compiled at runtime with the OS's
// d3dcompiler (an inbox Windows component since 8.1).
static const char SHADER_SRC[] =
    "cbuffer C : register(b0) { float4 UX; float4 UY; };\n"
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut VSMain(uint id : SV_VertexID) {\n"
    "    VSOut o;\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    o.uv = float2(UX.x + UX.y * uv.x + UX.z * uv.y,\n"
    "                  UY.x + UY.y * uv.x + UY.z * uv.y);\n"
    "    return o;\n"
    "}\n"
    "Texture2D tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "float4 PSMain(VSOut i) : SV_Target { return tex.Sample(smp, i.uv); }\n";

// Affine uv maps realizing 0/90/180/270-degree clockwise display rotation.
static void rot_constants(int rot, float ux[4], float uy[4]) {
    static const float T[4][8] = {
        //  UX            UY
        {0, 1, 0, 0,   0, 0, 1, 0},   // 0:   uv' = (u, v)
        {0, 0, 1, 0,   1, -1, 0, 0},  // 90:  uv' = (v, 1-u)
        {1, -1, 0, 0,  1, 0, -1, 0},  // 180: uv' = (1-u, 1-v)
        {1, 0, -1, 0,  0, 1, 0, 0},   // 270: uv' = (1-v, u)
    };
    const float* t = T[(rot / 90) & 3];
    memcpy(ux, t, 4 * sizeof(float));
    memcpy(uy, t + 4, 4 * sizeof(float));
}

static bool init_shader_path(D3DState* d) {
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    HRESULT hr = D3DCompile(SHADER_SRC, sizeof(SHADER_SRC) - 1, nullptr, nullptr,
                            nullptr, "VSMain", "vs_4_0", 0, 0, &vsb, &err);
    if (SUCCEEDED(hr))
        hr = D3DCompile(SHADER_SRC, sizeof(SHADER_SRC) - 1, nullptr, nullptr,
                        nullptr, "PSMain", "ps_4_0", 0, 0, &psb, &err);
    if (SUCCEEDED(hr))
        hr = d->dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(),
                                        nullptr, &d->vs);
    if (SUCCEEDED(hr))
        hr = d->dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(),
                                       nullptr, &d->ps);
    if (SUCCEEDED(hr)) {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        hr = d->dev->CreateSamplerState(&sd, &d->samp);
    }
    if (SUCCEEDED(hr)) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 8 * sizeof(float);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = d->dev->CreateBuffer(&bd, nullptr, &d->cbuf);
    }
    if (err) err->Release();
    if (vsb) vsb->Release();
    if (psb) psb->Release();
    if (FAILED(hr)) return fail_step(L"shader fallback initialization", hr);
    log_line("video: using shader render path (no D3D11 video API)");
    return true;
}

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
    if (FAILED(hr)) return fail_step(L"D3D11CreateDevice (hardware and WARP)", hr);

    // The avcodec D3D11VA decoder worker threads share this device with the
    // render thread; without this the driver may crash under contention.
    ID3D10Multithread* mt = nullptr;
    if (SUCCEEDED(d->dev->QueryInterface(__uuidof(ID3D10Multithread), (void**)&mt))) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }

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
    if (FAILED(hr)) return fail_step(L"DXGI swapchain creation", hr);

    hr = d->dev->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&d->vdev);
    if (SUCCEEDED(hr)) hr = d->ctx->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&d->vctx);
    d->use_vp = SUCCEEDED(hr);
    if (d->use_vp &&
        FAILED(d->vctx->QueryInterface(__uuidof(ID3D11VideoContext1), (void**)&d->vctx1)))
        d->vctx1 = nullptr;  // pre-1607 OS: legacy colorspace struct only
    if (!d->use_vp && !init_shader_path(d))
        return false;

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

// (Re)creates the video processor for a given input size. The NV12 staging
// texture used by the sw-frame path is created separately on demand.
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

    D3D11_VIDEO_PROCESSOR_CAPS caps = {};
    d->vp_can_rotate = SUCCEEDED(d->vpe->GetVideoProcessorCaps(&caps)) &&
                       (caps.FeatureCaps & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ROTATION);

    // ProcAmp filter ranges (b, c, h, s - matching pic[] order below)
    static const D3D11_VIDEO_PROCESSOR_FILTER fids[4] = {
        D3D11_VIDEO_PROCESSOR_FILTER_BRIGHTNESS,
        D3D11_VIDEO_PROCESSOR_FILTER_CONTRAST,
        D3D11_VIDEO_PROCESSOR_FILTER_HUE,
        D3D11_VIDEO_PROCESSOR_FILTER_SATURATION,
    };
    static const UINT fcaps[4] = {
        D3D11_VIDEO_PROCESSOR_FILTER_CAPS_BRIGHTNESS,
        D3D11_VIDEO_PROCESSOR_FILTER_CAPS_CONTRAST,
        D3D11_VIDEO_PROCESSOR_FILTER_CAPS_HUE,
        D3D11_VIDEO_PROCESSOR_FILTER_CAPS_SATURATION,
    };
    for (int i = 0; i < 4; i++)
        d->pic_ok[i] =
            (caps.FilterCaps & fcaps[i]) &&
            SUCCEEDED(d->vpe->GetVideoProcessorFilterRange(fids[i], &d->pic_range[i]));

    d->in_w = w;
    d->in_h = h;
    return true;
}

static bool ensure_staging_input(D3DState* d, bool p010) {
    if (d->in_tex && d->staging_p010 == p010) return true;
    safe_release(d->in_view);
    safe_release(d->in_tex);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = d->in_w;
    td.Height = d->in_h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = p010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(d->dev->CreateTexture2D(&td, nullptr, &d->in_tex))) return false;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
    ivd.FourCC = 0;
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.MipSlice = 0;
    if (FAILED(d->vdev->CreateVideoProcessorInputView(d->in_tex, d->vpe, &ivd, &d->in_view))) {
        safe_release(d->in_tex);
        return false;
    }
    d->staging_p010 = p010;
    return true;
}

static void set_colorspace(D3DState* d, const AVFrame* f) {
    bool full = f->color_range == AVCOL_RANGE_JPEG;
    bool bt709 = f->height >= 720;
    if (f->colorspace == AVCOL_SPC_BT709) bt709 = true;
    else if (f->colorspace == AVCOL_SPC_BT470BG || f->colorspace == AVCOL_SPC_SMPTE170M) bt709 = false;

    // DXGI colorspaces (Win10 1607+) understand BT.2020 primaries and the
    // PQ/HLG transfers, so HDR content gets driver conversion to SDR 709
    // instead of a washed-out 709 misread.
    if (d->vctx1) {
        bool bt2020 = f->colorspace == AVCOL_SPC_BT2020_NCL ||
                      f->colorspace == AVCOL_SPC_BT2020_CL;
        DXGI_COLOR_SPACE_TYPE in;
        if (bt2020 && f->color_trc == AVCOL_TRC_SMPTE2084)
            in = DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
        else if (bt2020 && f->color_trc == AVCOL_TRC_ARIB_STD_B67)
            in = DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020;
        else if (bt2020)
            in = full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020
                      : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020;
        else if (bt709)
            in = full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
                      : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
        else
            in = full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601
                      : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
        d->vctx1->VideoProcessorSetStreamColorSpace1(d->vp, 0, in);
        d->vctx1->VideoProcessorSetOutputColorSpace1(
            d->vp, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        return;
    }

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs = {};
    cs.YCbCr_Matrix = bt709 ? 1 : 0;
    cs.Nominal_Range = full ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
                            : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    d->vctx->VideoProcessorSetStreamColorSpace(d->vp, 0, &cs);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs = {};
    out_cs.RGB_Range = 0;  // full-range RGB out
    d->vctx->VideoProcessorSetOutputColorSpace(d->vp, &out_cs);
}

static RECT letterbox(int vw, int vh, int ww, int wh, bool cover = false) {
    RECT r;
    if (vw <= 0 || vh <= 0 || ww <= 0 || wh <= 0) { SetRect(&r, 0, 0, ww, wh); return r; }
    double scale = cover ? std::fmax((double)ww / vw, (double)wh / vh)
                         : std::fmin((double)ww / vw, (double)wh / vh);
    int dw = (int)(vw * scale + 0.5), dh = (int)(vh * scale + 0.5);
    int x = (ww - dw) / 2, y = (wh - dh) / 2;
    SetRect(&r, x, y, x + dw, y + dh);
    return r;
}

static void draw_overlays(D3DState* d, ID3D11Texture2D* backbuffer,
                          const SubRender& ov, const RECT& dst_rc) {
    if (!d->d2df || !d->dwf) return;
    if (ov.text.empty() && ov.osd.empty() && ov.bitmaps.empty()) return;
    const std::wstring& text = ov.text;

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

    // bitmap subtitles: map from their source coordinate space onto the
    // letterboxed video rect
    if (!ov.bitmaps.empty()) {
        d->d2drt->BeginDraw();
        for (auto& b : ov.bitmaps) {
            if (!b || b->src_w <= 0 || b->src_h <= 0) continue;
            ID2D1Bitmap* bmp = nullptr;
            D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                  D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (FAILED(d->d2drt->CreateBitmap(D2D1::SizeU(b->w, b->h),
                                              b->pixels.data(), b->w * 4, bp, &bmp)))
                continue;
            float sx = (float)(dst_rc.right - dst_rc.left) / b->src_w;
            float sy = (float)(dst_rc.bottom - dst_rc.top) / b->src_h;
            D2D1_RECT_F dest = D2D1::RectF(
                dst_rc.left + b->x * sx, dst_rc.top + b->y * sy,
                dst_rc.left + (b->x + b->w) * sx, dst_rc.top + (b->y + b->h) * sy);
            d->d2drt->DrawBitmap(bmp, dest, 1.0f,
                                 D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            bmp->Release();
        }
        d->d2drt->EndDraw();
    }

    if (text.empty() && ov.osd.empty()) return;

    int px = (int)(d->out_h / 18 * d->sub_scale);
    if (px < 12) px = 12;
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
    if (!text.empty()) {
        for (auto& off : offs) {
            D2D1_RECT_F b = box;
            b.left += off.x; b.right += off.x; b.top += off.y; b.bottom += off.y;
            d->d2drt->DrawTextW(text.c_str(), (UINT32)text.size(), d->text_fmt, b, black);
        }
        d->d2drt->DrawTextW(text.c_str(), (UINT32)text.size(), d->text_fmt, box, white);
    }
    if (!ov.osd.empty()) {
        IDWriteTextFormat* of = nullptr;
        int opx = d->out_h / 24;
        if (opx < 12) opx = 12;
        d->dwf->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                 (float)opx, L"", &of);
        if (of) {
            D2D1_RECT_F obox = D2D1::RectF(14.0f, 12.0f, d->out_w - 14.0f,
                                           12.0f + opx * 1.6f);
            for (auto& off : offs) {
                D2D1_RECT_F b = obox;
                b.left += off.x; b.right += off.x; b.top += off.y; b.bottom += off.y;
                d->d2drt->DrawTextW(ov.osd.c_str(), (UINT32)ov.osd.size(), of, b, black);
            }
            d->d2drt->DrawTextW(ov.osd.c_str(), (UINT32)ov.osd.size(), of, obox, white);
            of->Release();
        }
    }
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

ID3D11Device* VideoOut::decode_device() {
    std::lock_guard<std::mutex> lk(m_);
    return (d && d->use_vp) ? d->dev : nullptr;
}

void VideoOut::set_picture(int b, int c, int s, int h) {
    auto clamp = [](int v) { return v < -100 ? -100 : v > 100 ? 100 : v; };
    std::lock_guard<std::mutex> lk(m_);
    if (!d) return;
    d->pic[0] = clamp(b);
    d->pic[1] = clamp(c);
    d->pic[2] = clamp(h);  // pic[] order is b, c, hue, sat (filter ids)
    d->pic[3] = clamp(s);
}

void VideoOut::get_picture(int* b, int* c, int* s, int* h) {
    std::lock_guard<std::mutex> lk(m_);
    if (b) *b = d ? d->pic[0] : 0;
    if (c) *c = d ? d->pic[1] : 0;
    if (h) *h = d ? d->pic[2] : 0;
    if (s) *s = d ? d->pic[3] : 0;
}

void VideoOut::set_aspect(int mode) {
    std::lock_guard<std::mutex> lk(m_);
    if (d) d->aspect_mode = mode < 0 ? 0 : mode > 4 ? 0 : mode;
}

int VideoOut::aspect() {
    std::lock_guard<std::mutex> lk(m_);
    return d ? d->aspect_mode : 0;
}

void VideoOut::set_sub_scale(double s) {
    std::lock_guard<std::mutex> lk(m_);
    if (d) d->sub_scale = s < 0.5 ? 0.5 : s > 2.0 ? 2.0 : s;
}

double VideoOut::sub_scale() {
    std::lock_guard<std::mutex> lk(m_);
    return d ? d->sub_scale : 1.0;
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

static bool vp_blt(D3DState* d, ID3D11VideoProcessorInputView* in_view,
                   AVFrame* f, ID3D11Texture2D* back, const RECT& dst_rc, int rot) {
    set_colorspace(d, f);

    // Interlaced content: tell the processor the field order and the
    // driver deinterlaces during the blt (no combing on TV rips).
    D3D11_VIDEO_FRAME_FORMAT ff = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    if (f->flags & AV_FRAME_FLAG_INTERLACED)
        ff = (f->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST)
                 ? D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST
                 : D3D11_VIDEO_FRAME_FORMAT_INTERLACED_BOTTOM_FIELD_FIRST;
    d->vctx->VideoProcessorSetStreamFrameFormat(d->vp, 0, ff);

    // ProcAmp (brightness/contrast/hue/saturation), -100..100 mapped into
    // the driver's range on each side of its default.
    static const D3D11_VIDEO_PROCESSOR_FILTER fids[4] = {
        D3D11_VIDEO_PROCESSOR_FILTER_BRIGHTNESS,
        D3D11_VIDEO_PROCESSOR_FILTER_CONTRAST,
        D3D11_VIDEO_PROCESSOR_FILTER_HUE,
        D3D11_VIDEO_PROCESSOR_FILTER_SATURATION,
    };
    for (int i = 0; i < 4; i++) {
        if (!d->pic_ok[i]) continue;
        const D3D11_VIDEO_PROCESSOR_FILTER_RANGE& r = d->pic_range[i];
        int v = d->pic[i];
        if (v == 0) {
            d->vctx->VideoProcessorSetStreamFilter(d->vp, 0, fids[i], FALSE,
                                                   r.Default);
        } else {
            int span = v > 0 ? r.Maximum - r.Default : r.Default - r.Minimum;
            d->vctx->VideoProcessorSetStreamFilter(d->vp, 0, fids[i], TRUE,
                                                   r.Default + v * span / 100);
        }
    }

    if (d->vp_can_rotate) {
        D3D11_VIDEO_PROCESSOR_ROTATION r =
            rot == 90    ? D3D11_VIDEO_PROCESSOR_ROTATION_90
            : rot == 180 ? D3D11_VIDEO_PROCESSOR_ROTATION_180
            : rot == 270 ? D3D11_VIDEO_PROCESSOR_ROTATION_270
                         : D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY;
        d->vctx->VideoProcessorSetStreamRotation(d->vp, 0, rot != 0, r);
    } else if (rot) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            log_line("video: driver lacks VP rotation; playing unrotated");
        }
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ID3D11VideoProcessorOutputView* out_view = nullptr;
    HRESULT hr = d->vdev->CreateVideoProcessorOutputView(back, d->vpe, &ovd, &out_view);
    if (SUCCEEDED(hr)) {
        RECT src = {0, 0, f->width, f->height};
        RECT out_rc = {0, 0, d->out_w, d->out_h};
        RECT dst_copy = dst_rc;
        d->vctx->VideoProcessorSetStreamSourceRect(d->vp, 0, TRUE, &src);
        d->vctx->VideoProcessorSetStreamDestRect(d->vp, 0, TRUE, &dst_copy);
        d->vctx->VideoProcessorSetOutputTargetRect(d->vp, TRUE, &out_rc);
        D3D11_VIDEO_COLOR black = {};
        black.YCbCr.Y = 0.0625f;
        black.YCbCr.Cb = 0.5f;
        black.YCbCr.Cr = 0.5f;
        black.YCbCr.A = 1.0f;
        d->vctx->VideoProcessorSetOutputBackgroundColor(d->vp, TRUE, &black);

        D3D11_VIDEO_PROCESSOR_STREAM stream = {};
        stream.Enable = TRUE;
        stream.pInputSurface = in_view;
        hr = d->vctx->VideoProcessorBlt(d->vp, out_view, 0, 1, &stream);
        out_view->Release();
    }
    if (FAILED(hr)) log_line("video: VideoProcessorBlt failed 0x%08lx", hr);
    return SUCCEEDED(hr);
}

static bool render_vp(D3DState* d, AVFrame* f, ID3D11Texture2D* back,
                      const RECT& dst_rc, int rot) {
    // D3D11VA decoder output: a slice of the decoder's texture array on our
    // own device. Feed it to the video processor directly - no CPU copy.
    // (Works for NV12 and P010 alike; the view takes the texture's format.)
    if (f->format == AV_PIX_FMT_D3D11) {
        ID3D11Texture2D* tex = (ID3D11Texture2D*)f->data[0];
        UINT slice = (UINT)(uintptr_t)f->data[1];
        if (!tex) return false;
        D3D11_TEXTURE2D_DESC td;
        tex->GetDesc(&td);
        if (!ensure_video_processor(d, (int)td.Width, (int)td.Height)) return false;

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ivd.Texture2D.MipSlice = 0;
        ivd.Texture2D.ArraySlice = slice;
        ID3D11VideoProcessorInputView* view = nullptr;
        if (FAILED(d->vdev->CreateVideoProcessorInputView(tex, d->vpe, &ivd, &view)))
            return false;
        bool ok = vp_blt(d, view, f, back, dst_rc, rot);
        view->Release();
        return ok;
    }

    int w = f->width, h = f->height;
    // Upload as P010 when the source is >8-bit (10-bit stays 10-bit through
    // the video processor), NV12 otherwise. Both are Y plane then
    // interleaved UV at the same pitch.
    const AVPixFmtDescriptor* pd = av_pix_fmt_desc_get((AVPixelFormat)f->format);
    bool p010 = pd && pd->comp[0].depth > 8 && !d->no_p010;
    int bpp = p010 ? 2 : 1;
    int pitch = (w * bpp + 127) & ~127;
    size_t need = (size_t)pitch * h * 3 / 2;
    if (need > d->nv12_size) {
        av_free(d->nv12);
        d->nv12 = (uint8_t*)av_malloc(need);
        d->nv12_size = d->nv12 ? need : 0;
        d->nv12_pitch = pitch;
    }
    if (!d->nv12) return false;
    d->sws = sws_getCachedContext(d->sws, w, h, (AVPixelFormat)f->format,
                                  w, h, p010 ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12,
                                  SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!d->sws) return false;
    uint8_t* dst[2] = {d->nv12, d->nv12 + (size_t)pitch * h};
    int dst_stride[2] = {pitch, pitch};
    sws_scale(d->sws, f->data, f->linesize, 0, h, dst, dst_stride);

    if (!ensure_video_processor(d, w, h)) return false;
    if (!ensure_staging_input(d, p010)) {
        if (p010) {
            d->no_p010 = true;  // driver has no P010 VP input; next frame NV12
            log_line("video: P010 staging rejected, falling back to 8-bit");
        }
        return false;
    }
    d->ctx->UpdateSubresource(d->in_tex, 0, nullptr, d->nv12, pitch, 0);
    return vp_blt(d, d->in_view, f, back, dst_rc, rot);
}

static bool render_shader(D3DState* d, AVFrame* f, ID3D11Texture2D* back,
                          const RECT& dst_rc, int rot) {
    if (d->cbuf && rot != d->cbuf_rot) {
        float c[8];
        rot_constants(rot, c, c + 4);
        d->ctx->UpdateSubresource(d->cbuf, 0, nullptr, c, 0, 0);
        d->cbuf_rot = rot;
    }
    int w = f->width, h = f->height;
    // Convert to BGRA and upload as a shader resource.
    int pitch = w * 4;
    size_t need = (size_t)pitch * h;
    if (need > d->nv12_size) {
        av_free(d->nv12);
        d->nv12 = (uint8_t*)av_malloc(need);
        d->nv12_size = d->nv12 ? need : 0;
    }
    if (!d->nv12) return false;
    d->sws = sws_getCachedContext(d->sws, w, h, (AVPixelFormat)f->format,
                                  w, h, AV_PIX_FMT_BGRA, SWS_BILINEAR,
                                  nullptr, nullptr, nullptr);
    if (!d->sws) return false;
    uint8_t* dst[1] = {d->nv12};
    int dst_stride[1] = {pitch};
    sws_scale(d->sws, f->data, f->linesize, 0, h, dst, dst_stride);

    if (w != d->rgb_w || h != d->rgb_h) {
        safe_release(d->rgb_srv);
        safe_release(d->rgb_tex);
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(d->dev->CreateTexture2D(&td, nullptr, &d->rgb_tex))) return false;
        if (FAILED(d->dev->CreateShaderResourceView(d->rgb_tex, nullptr, &d->rgb_srv)))
            return false;
        d->rgb_w = w;
        d->rgb_h = h;
    }
    d->ctx->UpdateSubresource(d->rgb_tex, 0, nullptr, d->nv12, pitch, 0);

    ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(d->dev->CreateRenderTargetView(back, nullptr, &rtv))) return false;
    float clear[4] = {0, 0, 0, 1};
    d->ctx->ClearRenderTargetView(rtv, clear);
    d->ctx->OMSetRenderTargets(1, &rtv, nullptr);

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = (float)dst_rc.left;
    vp.TopLeftY = (float)dst_rc.top;
    vp.Width = (float)(dst_rc.right - dst_rc.left);
    vp.Height = (float)(dst_rc.bottom - dst_rc.top);
    vp.MaxDepth = 1.0f;
    d->ctx->RSSetViewports(1, &vp);

    d->ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d->ctx->IASetInputLayout(nullptr);
    d->ctx->VSSetConstantBuffers(0, 1, &d->cbuf);
    d->ctx->VSSetShader(d->vs, nullptr, 0);
    d->ctx->PSSetShader(d->ps, nullptr, 0);
    d->ctx->PSSetShaderResources(0, 1, &d->rgb_srv);
    d->ctx->PSSetSamplers(0, 1, &d->samp);
    d->ctx->Draw(3, 0);

    ID3D11ShaderResourceView* nullsrv = nullptr;
    d->ctx->PSSetShaderResources(0, 1, &nullsrv);
    ID3D11RenderTargetView* nullrtv = nullptr;
    d->ctx->OMSetRenderTargets(1, &nullrtv, nullptr);
    rtv->Release();
    return true;
}

bool VideoOut::render(AVFrame* f, const SubRender& overlays, int rotation_deg) {
    std::lock_guard<std::mutex> lk(m_);
    if (!d || !d->swap || !f) return false;

    int w = f->width, h = f->height;
    if (w <= 0 || h <= 0) return false;
    int rot = ((rotation_deg % 360) + 360) % 360 / 90 * 90;

    int am = d->aspect_mode;
    int disp_w = w, disp_h = h;
    if (f->sample_aspect_ratio.num > 0 && f->sample_aspect_ratio.den > 0)
        disp_w = (int)av_rescale(w, f->sample_aspect_ratio.num, f->sample_aspect_ratio.den);
    if (am == 1) { disp_w = 16; disp_h = 9; }  // forced ratios
    if (am == 2) { disp_w = 4; disp_h = 3; }
    if (rot == 90 || rot == 270) {
        int t = disp_w;
        disp_w = disp_h;
        disp_h = t;
    }
    RECT dst_rc;
    if (am == 3)
        SetRect(&dst_rc, 0, 0, d->out_w, d->out_h);  // stretch to window
    else
        dst_rc = letterbox(disp_w, disp_h, d->out_w, d->out_h, am == 4);

    ID3D11Texture2D* back = nullptr;
    if (FAILED(d->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)))
        return false;

    if (f->format == AV_PIX_FMT_D3D11 && !d->use_vp) {
        back->Release();  // hw frames only exist when decode_device() was handed out
        return false;
    }
    bool ok = d->use_vp ? render_vp(d, f, back, dst_rc, rot)
                        : render_shader(d, f, back, dst_rc, rot);
    if (ok) draw_overlays(d, back, overlays, dst_rc);
    back->Release();
    if (!ok) return false;
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
    safe_release(d->vctx1);
    safe_release(d->vctx);
    safe_release(d->vdev);
    safe_release(d->rgb_srv);
    safe_release(d->rgb_tex);
    safe_release(d->cbuf);
    safe_release(d->samp);
    safe_release(d->ps);
    safe_release(d->vs);
    safe_release(d->swap);
    safe_release(d->ctx);
    safe_release(d->dev);
    sws_freeContext(d->sws);
    av_free(d->nv12);
    delete d;
    d = nullptr;
}
