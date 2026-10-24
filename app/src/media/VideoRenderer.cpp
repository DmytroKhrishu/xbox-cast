// SPDX-License-Identifier: GPL-3.0-or-later
// Swap chain setup and direct sampling of decoder surfaces follow moonlight-xbox (GPL-3.0,
// github.com/TheElixZammuto/moonlight-xbox): Common/DeviceResources.cpp, Streaming/VideoRenderer.cpp.
#include "pch.h"
#include "VideoRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "portable/MediaClock.h"
#include "shaders/crop_ps.h"
#include "shaders/video_ps.h"
#include "shaders/video_vs.h"

namespace XboxCast { namespace Media {

namespace {

// Same layout as cbuffer VideoCb in shaders/video.hlsli (144 bytes).
struct VideoCb {
    float dstRect[4];
    float srcRect[4];
    float cscRow0[4];
    float cscRow1[4];
    float cscRow2[4];
    float cscOff[4];
    float lumaSize[4];
    float aux[4];
    uint32_t mode[4];
};
static_assert(sizeof(VideoCb) == 144, "VideoCb must match shaders/video.hlsli");

const DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
const UINT kSwapChainFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
const int64_t kCropIntervalNs = 500000000LL;
const int64_t kLogIntervalNs = 10000000000LL;

template <typename T>
winrt::com_ptr<T> QueryAs(IUnknown* u)
{
    winrt::com_ptr<T> r;
    if (u) u->QueryInterface(__uuidof(T), r.put_void());
    return r;
}

} // namespace

VideoRenderer::VideoRenderer(std::function<std::shared_ptr<D3DDevice>()> getDevice,
                             std::function<void(IDXGISwapChain1*)> bindSwapChain)
    : m_getDevice(std::move(getDevice)), m_bindSwapChain(std::move(bindSwapChain))
{
}

VideoRenderer::~VideoRenderer() { Stop(); }

void VideoRenderer::Start()
{
    if (m_running.exchange(true)) return;
    m_thread = std::thread([this] { ThreadMain(); });
}

void VideoRenderer::Stop()
{
    if (!m_running.exchange(false)) return;
    m_mailbox.Wake();
    if (m_thread.joinable()) m_thread.join();
    m_mailbox.Clear();
}

bool VideoRenderer::Submit(VideoFramePtr frame)
{
    bool replaced = m_mailbox.Put(std::move(frame));
    if (replaced) m_dropped++;
    return replaced;
}

void VideoRenderer::ResetStream()
{
    m_resetStream = true;
    m_mailbox.Wake();
}

void VideoRenderer::SetPanelSize(float widthDips, float heightDips, float scaleX, float scaleY)
{
    {
        std::lock_guard<std::mutex> lk(m_paramMu);
        m_panelW = widthDips;
        m_panelH = heightDips;
        m_scaleX = scaleX > 0 ? scaleX : 1.0f;
        m_scaleY = scaleY > 0 ? scaleY : 1.0f;
    }
    m_sizeDirty = true;
    m_mailbox.Wake();
}

void VideoRenderer::SetViewMode(ViewMode m)
{
    {
        std::lock_guard<std::mutex> lk(m_paramMu);
        if (m_mode == m) return;
        m_mode = m;
    }
    MLOGI("render", "view mode -> %s", ViewModeName(m));
    m_paramsDirty = true;
    m_mailbox.Wake();
}

void VideoRenderer::SetZoom(float z)
{
    z = (std::max)(1.0f, (std::min)(3.0f, z));
    {
        std::lock_guard<std::mutex> lk(m_paramMu);
        if (std::fabs(m_zoom - z) < 1e-4f) return;
        m_zoom = z;
    }
    m_paramsDirty = true;
    m_mailbox.Wake();
}

void VideoRenderer::Pan(float dx, float dy)
{
    {
        std::lock_guard<std::mutex> lk(m_paramMu);
        float x = m_panX + dx, y = m_panY + dy;
        if (m_lastScale > 0) {
            // clamp against the layout currently on screen: the pan point never moves the content off the view
            ClampPan(m_lastParams, m_lastScale, &x, &y);
        } else {
            x = (std::max)(0.0f, (std::min)(1.0f, x));
            y = (std::max)(0.0f, (std::min)(1.0f, y));
        }
        m_panX = x;
        m_panY = y;
    }
    m_paramsDirty = true;
    m_mailbox.Wake();
}

void VideoRenderer::ResetPan()
{
    {
        std::lock_guard<std::mutex> lk(m_paramMu);
        m_panX = 0.5f;
        m_panY = 0.5f;
    }
    m_paramsDirty = true;
    m_mailbox.Wake();
}

void VideoRenderer::SetAutoCrop(bool on)
{
    if (m_autoCrop.exchange(on) == on) return;
    MLOGI("render", "auto-crop %s", on ? "on" : "off");
    m_paramsDirty = true;
    m_mailbox.Wake();
}

void VideoRenderer::SetSyncInterval(int interval)
{
    interval = interval <= 0 ? 0 : 1;
    if (m_syncInterval.exchange(interval) != interval) MLOGI("render", "Present sync interval -> %d", interval);
}

ViewMode VideoRenderer::Mode() const
{
    std::lock_guard<std::mutex> lk(m_paramMu);
    return m_mode;
}

float VideoRenderer::Zoom() const
{
    std::lock_guard<std::mutex> lk(m_paramMu);
    return m_zoom;
}

RenderStats VideoRenderer::Stats()
{
    std::lock_guard<std::mutex> lk(m_statsMu);
    const int64_t now = NowNs();
    m_presentWin.Tick(now);
    m_latWin.Tick(now);
    RenderStats s = m_stats;
    s.renderFps = m_presentWin.Rate();
    s.presentMsAvg = m_presentWin.Avg();
    s.latencyMsAvg = m_latWin.Avg();
    s.latencyMsMax = m_latWin.Max();
    s.dropped = m_dropped.load();
    s.pending = m_mailbox.Has() ? 1 : 0;
    s.syncInterval = m_syncInterval.load();
    return s;
}

// ------------------------------------------------------------------------------------------------ render thread

void VideoRenderer::HandleD3DError(const char* where, HRESULT hr)
{
    if (IsDeviceLost(hr)) {
        if (m_dev) m_dev->MarkLost(where, hr);
    } else {
        MLOGE("render", "%s failed: %s", where, HrName(hr));
    }
}

bool VideoRenderer::EnsureDevice()
{
    if (m_dev && !m_dev->Lost()) return true;
    if (m_dev) {
        MLOGW("render", "device gen %u lost: releasing swap chain and every GPU resource", m_dev->Generation());
        ReleaseDeviceResources();
        ReleaseSwapChain(true);
        m_current.reset(); // its surface belongs to the dead device
        m_dev.reset();
    }
    auto d = m_getDevice ? m_getDevice() : nullptr;
    if (!d || d->Lost()) return false;
    m_dev = d;
    if (!CreatePipelineObjects()) {
        MLOGE("render", "pipeline objects could not be created on device gen %u", m_dev->Generation());
        ReleaseDeviceResources();
        m_dev.reset();
        return false;
    }
    m_sizeDirty = true;
    return true;
}

bool VideoRenderer::CreatePipelineObjects()
{
    ID3D11Device1* dev = m_dev->Device();
    HRESULT hr = dev->CreateVertexShader(g_video_vs, sizeof(g_video_vs), nullptr, m_vs.put());
    if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(g_video_ps, sizeof(g_video_ps), nullptr, m_ps.put());
    if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(g_crop_ps, sizeof(g_crop_ps), nullptr, m_cropPs.put());
    if (FAILED(hr)) {
        HandleD3DError("CreateShader", hr);
        return false;
    }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(VideoCb);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = dev->CreateBuffer(&bd, nullptr, m_cb.put());
    if (FAILED(hr)) {
        HandleD3DError("CreateBuffer(cb)", hr);
        return false;
    }
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    hr = dev->CreateSamplerState(&sd, m_sampler.put());
    if (FAILED(hr)) {
        HandleD3DError("CreateSamplerState", hr);
        return false;
    }
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    hr = dev->CreateRasterizerState(&rd, m_raster.put());
    if (FAILED(hr)) {
        HandleD3DError("CreateRasterizerState", hr);
        return false;
    }
    // crop target + 2 staging textures (ping-pong: a readback still in flight never blocks the next copy)
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = CropDetector::kSampleW;
    td.Height = CropDetector::kSampleH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    hr = dev->CreateTexture2D(&td, nullptr, m_cropRt.put());
    if (SUCCEEDED(hr)) hr = dev->CreateRenderTargetView(m_cropRt.get(), nullptr, m_cropRtv.put());
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (int i = 0; i < 2 && SUCCEEDED(hr); i++) hr = dev->CreateTexture2D(&td, nullptr, m_cropStaging[i].put());
    if (FAILED(hr)) {
        // crop detection is optional: log and carry on without it
        MLOGE("render", "crop readback textures failed: %s (auto-crop unavailable on this device)", HrName(hr));
        m_cropRtv = nullptr;
        m_cropRt = nullptr;
        m_cropStaging[0] = nullptr;
        m_cropStaging[1] = nullptr;
    }
    m_cropPending = -1;
    MLOGI("render", "pipeline ready on device gen %u: quad VS, YUV PS (Catmull-Rom luma when upscaling), crop pass %dx%d",
          m_dev->Generation(), CropDetector::kSampleW, CropDetector::kSampleH);
    return true;
}

void VideoRenderer::ReleaseDeviceResources()
{
    if (m_dev) {
        std::lock_guard<std::recursive_mutex> lk(m_dev->Mutex());
        m_dev->Context()->ClearState();
    }
    m_srvCache.clear();
    if (m_srvCachePool) av_buffer_unref(&m_srvCachePool);
    for (int i = 0; i < 3; i++) {
        m_upSrv[i] = nullptr;
        m_upTex[i] = nullptr;
    }
    m_upW = m_upH = 0;
    m_upFmt = -1;
    m_cropRtv = nullptr;
    m_cropRt = nullptr;
    m_cropStaging[0] = nullptr;
    m_cropStaging[1] = nullptr;
    m_cropPending = -1;
    m_vs = nullptr;
    m_ps = nullptr;
    m_cropPs = nullptr;
    m_cb = nullptr;
    m_sampler = nullptr;
    m_raster = nullptr;
}

void VideoRenderer::ReleaseSwapChain(bool unbind)
{
    m_rtv = nullptr;
    if (m_waitable) {
        CloseHandle(m_waitable);
        m_waitable = nullptr;
    }
    m_slotAcquired = false;
    if (m_swapChain && unbind && m_bindSwapChain) m_bindSwapChain(nullptr);
    m_swapChain = nullptr;
    m_scW = m_scH = 0;
}

bool VideoRenderer::EnsureSwapChain()
{
    const bool dirty = m_sizeDirty.exchange(false);
    if (!dirty && m_swapChain) return true;
    float pw, ph, sx, sy;
    {
        std::lock_guard<std::mutex> lk(m_paramMu);
        pw = m_panelW;
        ph = m_panelH;
        sx = m_scaleX;
        sy = m_scaleY;
    }
    const int w = (int)std::lround(pw * sx);
    const int h = (int)std::lround(ph * sy);
    if (w < 8 || h < 8) return m_swapChain != nullptr; // panel not laid out yet
    if (m_swapChain && w == m_scW && h == m_scH && sx == m_scScaleX && sy == m_scScaleY) return true;

    std::lock_guard<std::recursive_mutex> lk(m_dev->Mutex());
    ID3D11DeviceContext1* ctx = m_dev->Context();
    HRESULT hr = S_OK;
    if (m_swapChain) {
        m_rtv = nullptr;
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        ctx->Flush();
        hr = m_swapChain->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, kSwapChainFlags);
        if (FAILED(hr)) {
            HandleD3DError("ResizeBuffers", hr);
            ReleaseSwapChain(true);
            if (IsDeviceLost(hr)) return false;
        } else {
            MLOGI("render", "swap chain resized to %dx%d (panel %.0fx%.0f dips x scale %.2f/%.2f)", w, h, pw, ph, sx,
                  sy);
        }
    }
    if (!m_swapChain) {
        winrt::com_ptr<IDXGIDevice> dxgiDev = QueryAs<IDXGIDevice>(m_dev->Device());
        winrt::com_ptr<IDXGIAdapter> adapter;
        winrt::com_ptr<IDXGIFactory2> factory;
        hr = dxgiDev ? dxgiDev->GetAdapter(adapter.put()) : E_NOINTERFACE;
        if (SUCCEEDED(hr)) hr = adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void());
        if (FAILED(hr)) {
            HandleD3DError("DXGI factory", hr);
            return false;
        }
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = (UINT)w;
        desc.Height = (UINT)h;
        desc.Format = kBackBufferFormat;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.Flags = kSwapChainFlags;
        winrt::com_ptr<IDXGISwapChain1> sc1;
        hr = factory->CreateSwapChainForComposition(m_dev->Device(), &desc, nullptr, sc1.put());
        if (FAILED(hr)) {
            HandleD3DError("CreateSwapChainForComposition", hr);
            return false;
        }
        m_swapChain = QueryAs<IDXGISwapChain2>(sc1.get());
        if (!m_swapChain) {
            MLOGE("render", "IDXGISwapChain2 not available");
            return false;
        }
        hr = m_swapChain->SetMaximumFrameLatency(1);
        if (FAILED(hr)) MLOGW("render", "SetMaximumFrameLatency(1) failed: %s", HrName(hr));
        m_waitable = m_swapChain->GetFrameLatencyWaitableObject();
        m_slotAcquired = false;
        if (m_bindSwapChain) m_bindSwapChain(sc1.get());
        MLOGI("render",
              "swap chain created %dx%d (panel %.0fx%.0f dips x scale %.2f/%.2f) BGRA8 FLIP_SEQUENTIAL 2 buffers, "
              "waitable, max frame latency 1, sync interval %d",
              w, h, pw, ph, sx, sy, m_syncInterval.load());
    }
    // The panel scales its content by the composition scale; the swap chain is already at physical pixels.
    DXGI_MATRIX_3X2_F inv = {};
    inv._11 = 1.0f / sx;
    inv._22 = 1.0f / sy;
    hr = m_swapChain->SetMatrixTransform(&inv);
    if (FAILED(hr)) MLOGW("render", "SetMatrixTransform failed: %s", HrName(hr));

    winrt::com_ptr<ID3D11Texture2D> back;
    hr = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), back.put_void());
    if (SUCCEEDED(hr)) hr = m_dev->Device()->CreateRenderTargetView(back.get(), nullptr, m_rtv.put());
    if (FAILED(hr)) {
        HandleD3DError("back buffer RTV", hr);
        ReleaseSwapChain(true);
        return false;
    }
    m_scW = w;
    m_scH = h;
    m_scScaleX = sx;
    m_scScaleY = sy;
    {
        std::lock_guard<std::mutex> slk(m_statsMu);
        m_stats.viewW = w;
        m_stats.viewH = h;
    }
    // new view size: layout jumps to the new geometry (animating pixel rects across a resize looks wrong)
    UpdateTarget(NowNs(), true);
    return true;
}

// ---------------------------------------------------------------- frame planes

bool VideoRenderer::PrepareHwPlanes(const AVFrame* fr, Planes* p)
{
    auto* tex = reinterpret_cast<ID3D11Texture2D*>(fr->data[0]);
    const int slice = (int)(intptr_t)fr->data[1];
    if (!tex || !fr->hw_frames_ctx) return false;
    if (!m_srvCachePool || m_srvCachePool->data != fr->hw_frames_ctx->data) {
        // new frame pool (new stream / format change): SRVs over the old surfaces are useless
        m_srvCache.clear();
        if (m_srvCachePool) av_buffer_unref(&m_srvCachePool);
        m_srvCachePool = av_buffer_ref(fr->hw_frames_ctx);
    }
    D3D11_TEXTURE2D_DESC td = {};
    tex->GetDesc(&td);
    const bool tenBit = td.Format == DXGI_FORMAT_P010 || td.Format == DXGI_FORMAT_P016;
    if (td.Format != DXGI_FORMAT_NV12 && !tenBit) {
        MLOGE("render", "unsupported decoder surface format %d", (int)td.Format);
        return false;
    }
    auto key = std::make_pair(tex, slice);
    auto it = m_srvCache.find(key);
    if (it == m_srvCache.end()) {
        SrvPair pair;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray.MostDetailedMip = 0;
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.FirstArraySlice = (UINT)slice;
        sd.Texture2DArray.ArraySize = 1;
        sd.Format = tenBit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
        HRESULT hr = m_dev->Device()->CreateShaderResourceView(tex, &sd, pair.y.put());
        if (SUCCEEDED(hr)) {
            sd.Format = tenBit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
            hr = m_dev->Device()->CreateShaderResourceView(tex, &sd, pair.uv.put());
        }
        if (FAILED(hr)) {
            HandleD3DError("SRV on decoder surface (needs D3D11_BIND_SHADER_RESOURCE on the pool)", hr);
            return false;
        }
        if (m_srvCache.empty())
            MLOGI("render", "sampling decoder surfaces directly: %s array %ux%u x%u slices",
                  tenBit ? "P010" : "NV12", td.Width, td.Height, td.ArraySize);
        it = m_srvCache.emplace(key, std::move(pair)).first;
    }
    p->srv[0] = it->second.y.get();
    p->srv[1] = it->second.uv.get();
    p->planar = false;
    p->texW = (int)td.Width;
    p->texH = (int)td.Height;
    p->storage = tenBit ? SampleStorage::Unorm16Msb : SampleStorage::Unorm8;
    p->bits = tenBit ? 10 : 8;
    p->name = tenBit ? "p010 (d3d11va)" : "nv12 (d3d11va)";
    return true;
}

bool VideoRenderer::EnsureUploadTextures(int w, int h, int fmt)
{
    if (m_upTex[0] && m_upW == w && m_upH == h && m_upFmt == fmt) return true;
    for (int i = 0; i < 3; i++) {
        m_upSrv[i] = nullptr;
        m_upTex[i] = nullptr;
    }
    const AVPixelFormat f = (AVPixelFormat)fmt;
    const bool planar = f == AV_PIX_FMT_YUV420P || f == AV_PIX_FMT_YUVJ420P || f == AV_PIX_FMT_YUV420P10;
    const bool wide = f == AV_PIX_FMT_YUV420P10 || f == AV_PIX_FMT_P010;
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    struct PlaneDesc { int w, h; DXGI_FORMAT f; };
    PlaneDesc pd[3];
    int n;
    if (planar) {
        DXGI_FORMAT one = wide ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
        pd[0] = {w, h, one};
        pd[1] = {cw, ch, one};
        pd[2] = {cw, ch, one};
        n = 3;
    } else {
        pd[0] = {w, h, wide ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM};
        pd[1] = {cw, ch, wide ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM};
        n = 2;
    }
    for (int i = 0; i < n; i++) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = (UINT)pd[i].w;
        td.Height = (UINT)pd[i].h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = pd[i].f;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr = m_dev->Device()->CreateTexture2D(&td, nullptr, m_upTex[i].put());
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = pd[i].f;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.ArraySize = 1;
        if (SUCCEEDED(hr)) hr = m_dev->Device()->CreateShaderResourceView(m_upTex[i].get(), &sd, m_upSrv[i].put());
        if (FAILED(hr)) {
            HandleD3DError("software upload texture", hr);
            m_upFmt = -1;
            return false;
        }
    }
    m_upW = w;
    m_upH = h;
    m_upFmt = fmt;
    MLOGI("render", "software frame upload textures %dx%d %s (%d planes)", w, h, av_get_pix_fmt_name(f), n);
    return true;
}

bool VideoRenderer::UploadSoftwareFrame(const AVFrame* fr, Planes* p)
{
    const AVPixelFormat f = (AVPixelFormat)fr->format;
    const bool planar = f == AV_PIX_FMT_YUV420P || f == AV_PIX_FMT_YUVJ420P || f == AV_PIX_FMT_YUV420P10;
    const bool semi = f == AV_PIX_FMT_NV12 || f == AV_PIX_FMT_P010;
    if (!planar && !semi) {
        static int warned = 0;
        if (warned++ < 3) MLOGE("render", "software frame format %s not supported by the renderer", av_get_pix_fmt_name(f));
        return false;
    }
    if (!EnsureUploadTextures(fr->width, fr->height, fr->format)) return false;
    const bool wide = f == AV_PIX_FMT_YUV420P10 || f == AV_PIX_FMT_P010;
    const int bpp = wide ? 2 : 1;
    const int cw = (fr->width + 1) / 2, ch = (fr->height + 1) / 2;
    const int n = planar ? 3 : 2;
    ID3D11DeviceContext1* ctx = m_dev->Context();
    for (int i = 0; i < n; i++) {
        const int rows = i == 0 ? fr->height : ch;
        const int rowBytes = i == 0 ? fr->width * bpp : (planar ? cw * bpp : cw * 2 * bpp);
        D3D11_MAPPED_SUBRESOURCE m = {};
        HRESULT hr = ctx->Map(m_upTex[i].get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        if (FAILED(hr)) {
            HandleD3DError("Map(upload)", hr);
            return false;
        }
        const uint8_t* src = fr->data[i];
        uint8_t* dst = static_cast<uint8_t*>(m.pData);
        for (int y = 0; y < rows; y++) std::memcpy(dst + (size_t)y * m.RowPitch, src + (size_t)y * fr->linesize[i], rowBytes);
        ctx->Unmap(m_upTex[i].get(), 0);
    }
    if (planar) {
        p->srv[0] = m_upSrv[0].get();
        p->srv[2] = m_upSrv[1].get();
        p->srv[3] = m_upSrv[2].get();
    } else {
        p->srv[0] = m_upSrv[0].get();
        p->srv[1] = m_upSrv[1].get();
    }
    p->planar = planar;
    p->texW = fr->width;
    p->texH = fr->height;
    if (f == AV_PIX_FMT_YUV420P10) p->storage = SampleStorage::Unorm16Lsb;
    else if (f == AV_PIX_FMT_P010) p->storage = SampleStorage::Unorm16Msb;
    else p->storage = SampleStorage::Unorm8;
    p->bits = wide ? 10 : 8;
    p->name = av_get_pix_fmt_name(f);
    return true;
}

// ---------------------------------------------------------------- layout

void VideoRenderer::UpdateTarget(int64_t nowNs, bool snap)
{
    if (m_frameW <= 0 || m_frameH <= 0 || m_scW <= 0 || m_scH <= 0) return;
    ViewParams p;
    p.frameW = m_frameW;
    p.frameH = m_frameH;
    p.viewW = m_scW;
    p.viewH = m_scH;
    {
        std::lock_guard<std::mutex> lk(m_paramMu);
        p.mode = m_mode;
        p.zoom = m_zoom;
        p.panX = m_panX;
        p.panY = m_panY;
        p.content = m_autoCrop.load() ? m_content : RectF::Unit();
        Layout target = ComputeLayout(p);
        // keep the stored pan clamped to what is actually reachable, so the UI's next Pan starts from the real point
        ClampPan(p, target.scale, &m_panX, &m_panY);
        m_lastParams = p;
        m_lastScale = target.scale;
        m_anim.SetTarget(target, nowNs, snap || !m_anim.HasTarget());
        std::lock_guard<std::mutex> slk(m_statsMu);
        m_stats.resolved = target.resolvedMode;
        m_stats.crop = p.content;
    }
}

// ---------------------------------------------------------------- crop

void VideoRenderer::RunCropPass(const Planes& p, int frameW, int frameH, int64_t nowNs)
{
    if (!m_cropRtv || !m_autoCrop.load()) return;
    if (m_cropPending >= 0) return;                       // previous result not read yet
    if (nowNs - m_cropLastNs < kCropIntervalNs) return;
    m_cropLastNs = nowNs;
    ID3D11DeviceContext1* ctx = m_dev->Context();
    VideoCb cb = {};
    cb.dstRect[0] = -1;
    cb.dstRect[1] = 1;
    cb.dstRect[2] = 1;
    cb.dstRect[3] = -1;
    const float u1 = (float)frameW / (float)p.texW, v1 = (float)frameH / (float)p.texH;
    cb.srcRect[0] = 0;
    cb.srcRect[1] = 0;
    cb.srcRect[2] = u1;
    cb.srcRect[3] = v1;
    cb.aux[0] = u1 / CropDetector::kSampleW;
    cb.aux[1] = v1 / CropDetector::kSampleH;
    // raw sampler value -> 8-bit-equivalent level in [0,1]
    cb.aux[2] = p.storage == SampleStorage::Unorm16Lsb ? 65535.0f / 1023.0f : 1.0f;
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(ctx->Map(m_cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    std::memcpy(m.pData, &cb, sizeof cb);
    ctx->Unmap(m_cb.get(), 0);
    ID3D11RenderTargetView* rtv = m_cropRtv.get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = {0, 0, (float)CropDetector::kSampleW, (float)CropDetector::kSampleH, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->PSSetShader(m_cropPs.get(), nullptr, 0);
    ctx->Draw(4, 0);
    ctx->CopyResource(m_cropStaging[m_cropNext].get(), m_cropRt.get());
    m_cropPending = m_cropNext;
    m_cropNext ^= 1;
}

void VideoRenderer::ReadCropResult()
{
    if (m_cropPending < 0 || !m_cropStaging[m_cropPending]) return;
    ID3D11DeviceContext1* ctx = m_dev->Context();
    D3D11_MAPPED_SUBRESOURCE m = {};
    HRESULT hr = ctx->Map(m_cropStaging[m_cropPending].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return; // try again next frame
    if (FAILED(hr)) {
        HandleD3DError("Map(crop staging)", hr);
        m_cropPending = -1;
        return;
    }
    const bool changed = m_crop.Push(static_cast<const uint8_t*>(m.pData), CropDetector::kSampleW,
                                     CropDetector::kSampleH, (int)m.RowPitch, m_cropFullRange);
    ctx->Unmap(m_cropStaging[m_cropPending].get(), 0);
    m_cropPending = -1;
    if (changed) {
        RectF c = m_crop.Content();
        {
            std::lock_guard<std::mutex> lk(m_paramMu);
            m_content = c;
        }
        MLOGI("render", "crop -> x %.3f y %.3f w %.3f h %.3f (frame %dx%d: content %dx%d)", c.x, c.y, c.w, c.h,
              m_frameW, m_frameH, (int)std::lround(c.w * m_frameW), (int)std::lround(c.h * m_frameH));
        UpdateTarget(NowNs(), false);
    }
}

// ---------------------------------------------------------------- draw

bool VideoRenderer::RenderOnce(int64_t nowNs, bool newPicture)
{
    std::lock_guard<std::recursive_mutex> lk(m_dev->Mutex());
    ID3D11DeviceContext1* ctx = m_dev->Context();
    const int64_t t0 = NowNs();

    ReadCropResult();

    const float black[4] = {0, 0, 0, 1};
    ctx->ClearRenderTargetView(m_rtv.get(), black);

    Planes planes;
    bool havePlanes = false;
    const AVFrame* fr = m_current ? m_current->frame : nullptr;
    if (fr) {
        if (m_current->hw) {
            havePlanes = m_current->deviceGen == m_dev->Generation() && PrepareHwPlanes(fr, &planes);
        } else if (newPicture || m_upFmt != fr->format || m_upW != fr->width || m_upH != fr->height) {
            havePlanes = UploadSoftwareFrame(fr, &planes);
        } else {
            // redraw (animation) of an already uploaded software frame
            const AVPixelFormat f = (AVPixelFormat)fr->format;
            planes.planar = f == AV_PIX_FMT_YUV420P || f == AV_PIX_FMT_YUVJ420P || f == AV_PIX_FMT_YUV420P10;
            planes.srv[0] = m_upSrv[0].get();
            if (planes.planar) {
                planes.srv[2] = m_upSrv[1].get();
                planes.srv[3] = m_upSrv[2].get();
            } else {
                planes.srv[1] = m_upSrv[1].get();
            }
            planes.texW = m_upW;
            planes.texH = m_upH;
            planes.storage = f == AV_PIX_FMT_YUV420P10 ? SampleStorage::Unorm16Lsb
                             : f == AV_PIX_FMT_P010   ? SampleStorage::Unorm16Msb
                                                      : SampleStorage::Unorm8;
            planes.bits = (f == AV_PIX_FMT_YUV420P10 || f == AV_PIX_FMT_P010) ? 10 : 8;
            planes.name = av_get_pix_fmt_name(f);
            havePlanes = planes.srv[0] != nullptr;
        }
    }

    if (havePlanes) {
        const bool fullRange = fr->color_range == AVCOL_RANGE_JPEG || fr->format == AV_PIX_FMT_YUVJ420P;
        const YuvMatrix matrix = MatrixFromAvColorspace((int)fr->colorspace, fr->height);
        const CscConstants csc = ComputeCsc(matrix, fullRange, planes.bits, planes.storage);
        m_cropFullRange = fullRange;
        if (newPicture && m_current->streamStart) {
            char cm[64];
            std::snprintf(cm, sizeof cm, "%s %s", YuvMatrixName(matrix), fullRange ? "full" : "limited");
            MLOGI("render", "stream picture %dx%d %s, %s, surface %dx%d, view %dx%d", fr->width, fr->height, planes.name,
                  cm, planes.texW, planes.texH, m_scW, m_scH);
            std::lock_guard<std::mutex> slk(m_statsMu);
            m_stats.pixelFormat = planes.name;
            m_stats.colorMatrix = cm;
        }

        // common pipeline state
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx->VSSetShader(m_vs.get(), nullptr, 0);
        ID3D11Buffer* cbs[1] = {m_cb.get()};
        ctx->VSSetConstantBuffers(0, 1, cbs);
        ctx->PSSetConstantBuffers(0, 1, cbs);
        ID3D11SamplerState* samp = m_sampler.get();
        ctx->PSSetSamplers(0, 1, &samp);
        ctx->RSSetState(m_raster.get());
        ctx->PSSetShaderResources(0, 4, planes.srv);

        // auto-crop sample (separate small target, before the main draw so the main draw ends on the back buffer)
        RunCropPass(planes, fr->width, fr->height, nowNs);

        Layout l = m_anim.Current(nowNs);
        DrawRects dr = ComputeDrawRects(l, m_scW, m_scH);
        if (!dr.dst.Empty() && !dr.src.Empty()) {
            const float su = (float)fr->width / (float)planes.texW, sv = (float)fr->height / (float)planes.texH;
            VideoCb cb = {};
            cb.dstRect[0] = dr.dst.x / m_scW * 2.0f - 1.0f;
            cb.dstRect[1] = 1.0f - dr.dst.y / m_scH * 2.0f;
            cb.dstRect[2] = dr.dst.Right() / m_scW * 2.0f - 1.0f;
            cb.dstRect[3] = 1.0f - dr.dst.Bottom() / m_scH * 2.0f;
            cb.srcRect[0] = dr.src.x * su;
            cb.srcRect[1] = dr.src.y * sv;
            cb.srcRect[2] = dr.src.Right() * su;
            cb.srcRect[3] = dr.src.Bottom() * sv;
            for (int i = 0; i < 3; i++) {
                cb.cscRow0[i] = csc.m[i];
                cb.cscRow1[i] = csc.m[3 + i];
                cb.cscRow2[i] = csc.m[6 + i];
                cb.cscOff[i] = csc.off[i];
            }
            cb.lumaSize[0] = (float)planes.texW;
            cb.lumaSize[1] = (float)planes.texH;
            cb.lumaSize[2] = 1.0f / planes.texW;
            cb.lumaSize[3] = 1.0f / planes.texH;
            cb.mode[0] = planes.planar ? 1u : 0u;
            // destination pixels per source pixel > 1.02: upscaling, use the sharper luma filter
            const float scale = dr.dst.w / (dr.src.w * fr->width);
            cb.mode[1] = scale > 1.02f ? 1u : 0u;
            D3D11_MAPPED_SUBRESOURCE m = {};
            HRESULT hr = ctx->Map(m_cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
            if (SUCCEEDED(hr)) {
                std::memcpy(m.pData, &cb, sizeof cb);
                ctx->Unmap(m_cb.get(), 0);
                ID3D11RenderTargetView* rtv = m_rtv.get();
                ctx->OMSetRenderTargets(1, &rtv, nullptr);
                D3D11_VIEWPORT vp = {0, 0, (float)m_scW, (float)m_scH, 0, 1};
                ctx->RSSetViewports(1, &vp);
                ctx->PSSetShader(m_ps.get(), nullptr, 0);
                ctx->Draw(4, 0);
            }
        }
        // don't keep decoder surfaces bound (the decoder writes them)
        ID3D11ShaderResourceView* none[4] = {};
        ctx->PSSetShaderResources(0, 4, none);
    }

    HRESULT hr = m_swapChain->Present((UINT)m_syncInterval.load(), 0);
    m_slotAcquired = false;
    if (FAILED(hr)) {
        HandleD3DError("Present", hr);
        if (!IsDeviceLost(hr)) ReleaseSwapChain(true);
        return false;
    }
    const int64_t t1 = NowNs();
    if (newPicture && m_current) {
        const int64_t lat = t1 - m_current->ptsNs;
        std::lock_guard<std::mutex> slk(m_statsMu);
        m_stats.presented++;
        m_presentWin.Add(t1, NsToMs(t1 - t0));
        if (lat > 0 && lat < 5000000000LL) {
            m_latWin.Add(t1, NsToMs(lat));
            m_latEmaNs = m_latEmaNs <= 0 ? (double)lat : m_latEmaNs * 0.95 + (double)lat * 0.05;
            m_videoLatencyNs = (int64_t)m_latEmaNs;
        }
    }
    if (t1 - m_lastLogNs >= kLogIntervalNs && m_current) {
        m_lastLogNs = t1;
        RenderStats s = Stats();
        MLOGI("render", "%.1f fps, latency pts->present avg %.1f max %.1f ms, draw+present %.2f ms, dropped %llu, view %s",
              s.renderFps, s.latencyMsAvg, s.latencyMsMax, s.presentMsAvg, (unsigned long long)s.dropped,
              ViewModeName(s.resolved));
    }
    return true;
}

void VideoRenderer::ThreadMain()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    MLOGI("render", "render thread started");
    while (m_running.load()) {
        if (!EnsureDevice()) {
            m_mailbox.Wait(std::chrono::milliseconds(100));
            VideoFramePtr drop;
            m_mailbox.Take(drop); // no device: nothing can be shown, keep the mailbox empty
            continue;
        }
        if (m_resetStream.exchange(false)) {
            m_crop.Reset();
            m_cropPending = -1;
            m_cropLastNs = 0;
            std::lock_guard<std::mutex> lk(m_paramMu);
            m_content = RectF::Unit();
        }
        if (!EnsureSwapChain() || !m_swapChain) {
            // no surface yet (panel not laid out / device lost): keep only the newest picture so it shows at once
            if (m_mailbox.Wait(std::chrono::milliseconds(50))) {
                VideoFramePtr f;
                if (m_mailbox.Take(f)) m_current = std::move(f);
            }
            continue;
        }
        // 1. frame slot: with max frame latency 1 this returns once the previous frame is on its way to the screen
        if (!m_slotAcquired) {
            DWORD w = WaitForSingleObjectEx(m_waitable, 100, TRUE);
            if (w != WAIT_OBJECT_0) continue; // timeout: re-check running / size / device
            m_slotAcquired = true;
        }
        // 2. the newest picture (or a reason to redraw)
        int64_t now = NowNs();
        const bool animating = m_anim.Animating(now);
        const bool animEnded = m_wasAnimating && !animating;
        m_wasAnimating = animating;
        bool params = m_paramsDirty.exchange(false);
        if (!animating && !animEnded && !params && !m_mailbox.Has()) {
            m_mailbox.Wait(std::chrono::milliseconds(100));
            params = m_paramsDirty.exchange(false) || params;
            if (m_sizeDirty.load() || m_resetStream.load()) continue;
        }
        VideoFramePtr f;
        const bool newPicture = m_mailbox.Take(f);
        now = NowNs();
        if (newPicture) {
            const bool geometry = !m_current || f->streamStart || f->frame->width != m_frameW ||
                                  f->frame->height != m_frameH;
            m_current = std::move(f);
            if (geometry) {
                m_frameW = m_current->frame->width;
                m_frameH = m_current->frame->height;
                UpdateTarget(now, true); // new stream geometry (rotation): snap, the old layout means nothing now
            }
        }
        if (params) UpdateTarget(now, false);
        if (!newPicture && !params && !animating && !animEnded) continue; // spurious wake: keep the slot
        RenderOnce(now, newPicture);
    }
    ReleaseDeviceResources();
    ReleaseSwapChain(true);
    m_current.reset();
    m_dev.reset();
    MLOGI("render", "render thread stopped (presented %llu, dropped %llu)", (unsigned long long)m_stats.presented,
          (unsigned long long)m_dropped.load());
}

}} // namespace XboxCast::Media
