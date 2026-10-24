// xbox-cast media: D3D11 renderer on a XAML SwapChainPanel.
//
// * Composition swap chain, FLIP_SEQUENTIAL, 2 buffers, FRAME_LATENCY_WAITABLE_OBJECT + SetMaximumFrameLatency(1):
//   at most one frame is ever queued for scan-out. Sized from the panel (ActualSize x CompositionScale), never a fixed
//   1080p: a 4K source is decoded at source size and scaled on the GPU into whatever the panel is.
// * Render thread: acquire the swap chain's frame slot (waitable object), then wait for the newest decoded picture
//   (or an animation tick / resize / mode change), draw, Present(syncInterval, 0). The decode thread hands pictures
//   over through a one-slot "latest wins" mailbox: a picture that is overtaken before it was drawn is dropped, so the
//   screen always shows the newest decoded frame and nothing ever queues behind it.
// * Decoder surfaces (D3D11VA NV12 / P010 texture arrays) are sampled directly through per-slice SRVs (no copy).
//   Software frames (fallback) are uploaded into dynamic textures (yuv420p, yuv420p10, nv12, p010).
// * YUV -> RGB in the pixel shader with the stream's matrix and range; Catmull-Rom luma when upscaling.
// * View transform (Fit / Fill / Zoom / Auto) from portable/ViewTransform, animated 300 ms ease-out, never pops.
// * Auto-crop: every 500 ms the visible luma is GPU-downsampled to 256x144 (R8) and copied to a staging texture; the
//   CPU maps it on a later frame with DO_NOT_WAIT (never stalls) and feeds portable/CropDetector.
//
// Swap chain setup (waitable object, max latency 1, inverse composition scale) and direct sampling of decoder surfaces
// follow moonlight-xbox (GPL-3.0, github.com/TheElixZammuto/moonlight-xbox): Common/DeviceResources.cpp,
// Streaming/FFmpegDecoder.cpp, Streaming/VideoRenderer.cpp.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "D3DDevice.h"
#include "MediaPlatform.h"
#include "VideoFrame.h"
#include "portable/ColorMatrix.h"
#include "portable/CropDetector.h"
#include "portable/MediaStats.h"
#include "portable/Queues.h"
#include "portable/ViewTransform.h"

namespace XboxCast { namespace Media {

struct RenderStats {
    double renderFps = 0;                      // new pictures presented per second
    double latencyMsAvg = 0, latencyMsMax = 0; // Present() return - pts, last second
    double presentMsAvg = 0;                   // draw + Present() call duration
    uint64_t presented = 0;
    uint64_t dropped = 0;                      // decoded pictures overtaken before they were drawn
    int pending = 0;                           // picture waiting in the mailbox (0/1)
    int viewW = 0, viewH = 0;
    RectF crop = RectF::Unit();
    ViewMode resolved = ViewMode::Fit;
    std::string pixelFormat;
    std::string colorMatrix;
    int syncInterval = 1;
};

class VideoRenderer {
public:
    // getDevice: the current shared device (the pipeline recreates it after a loss; never blocks for long).
    // bindSwapChain: hands the swap chain to the SwapChainPanel (the callee marshals to the UI thread). nullptr = unbind.
    VideoRenderer(std::function<std::shared_ptr<D3DDevice>()> getDevice,
                  std::function<void(IDXGISwapChain1*)> bindSwapChain);
    ~VideoRenderer();

    void Start();
    void Stop(); // joins the render thread, releases every D3D resource, unbinds the swap chain

    // Decode thread. Returns true if an undrawn picture was replaced (counted as a dropped frame).
    bool Submit(VideoFramePtr frame);
    // New stream / flush: forget crop history. The screen keeps the last picture until the next one arrives (no flash
    // to black on rotation).
    void ResetStream();

    // Any thread (UI).
    void SetPanelSize(float widthDips, float heightDips, float scaleX, float scaleY);
    void SetViewMode(ViewMode m);
    void SetZoom(float z);
    void Pan(float dx, float dy);
    void ResetPan();
    void SetAutoCrop(bool on);
    void SetSyncInterval(int interval); // 1 (default) or 0; see docs/media.md "Present"

    RenderStats Stats();
    // Smoothed (present - pts) in ns for A/V sync; 0 when no video is on screen.
    int64_t VideoLatencyNs() const { return m_videoLatencyNs.load(); }
    ViewMode Mode() const;
    float Zoom() const;
    bool AutoCrop() const { return m_autoCrop.load(); }

private:
    struct Planes {
        ID3D11ShaderResourceView* srv[4] = {};  // t0 Y, t1 UV (2-plane), t2 U, t3 V (3-plane)
        bool planar = false;
        int texW = 0, texH = 0;                 // allocated luma texture size (decoder surfaces are aligned)
        SampleStorage storage = SampleStorage::Unorm8;
        int bits = 8;
        const char* name = "?";
    };

    void ThreadMain();
    bool EnsureDevice();
    bool EnsureSwapChain();
    void ReleaseSwapChain(bool unbind);
    void ReleaseDeviceResources();
    bool CreatePipelineObjects();
    bool PrepareHwPlanes(const AVFrame* fr, Planes* p);
    bool UploadSoftwareFrame(const AVFrame* fr, Planes* p);
    bool EnsureUploadTextures(int w, int h, int fmt);
    void UpdateTarget(int64_t nowNs, bool snap);
    bool RenderOnce(int64_t nowNs, bool newPicture);
    void RunCropPass(const Planes& p, int frameW, int frameH, int64_t nowNs);
    void ReadCropResult();
    void HandleD3DError(const char* where, HRESULT hr);

    std::function<std::shared_ptr<D3DDevice>()> m_getDevice;
    std::function<void(IDXGISwapChain1*)> m_bindSwapChain;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    LatestMailbox<VideoFramePtr> m_mailbox;
    std::atomic<bool> m_resetStream{false};
    std::atomic<uint64_t> m_dropped{0};

    // ---- render thread only
    std::shared_ptr<D3DDevice> m_dev;
    winrt::com_ptr<IDXGISwapChain2> m_swapChain;
    HANDLE m_waitable = nullptr;
    bool m_slotAcquired = false;       // waited on the waitable object, not presented yet
    winrt::com_ptr<ID3D11RenderTargetView> m_rtv;
    int m_scW = 0, m_scH = 0;
    float m_scScaleX = 1, m_scScaleY = 1;
    winrt::com_ptr<ID3D11VertexShader> m_vs;
    winrt::com_ptr<ID3D11PixelShader> m_ps, m_cropPs;
    winrt::com_ptr<ID3D11Buffer> m_cb;
    winrt::com_ptr<ID3D11SamplerState> m_sampler;
    winrt::com_ptr<ID3D11RasterizerState> m_raster;
    // SRVs over decoder surfaces: (texture, array slice) -> {Y, UV}; cleared when the frame pool changes
    struct SrvPair { winrt::com_ptr<ID3D11ShaderResourceView> y, uv; };
    std::map<std::pair<ID3D11Texture2D*, int>, SrvPair> m_srvCache;
    AVBufferRef* m_srvCachePool = nullptr; // ref on the hw_frames_ctx the cache belongs to
    // software upload textures (Y, U/UV, V)
    winrt::com_ptr<ID3D11Texture2D> m_upTex[3];
    winrt::com_ptr<ID3D11ShaderResourceView> m_upSrv[3];
    int m_upW = 0, m_upH = 0, m_upFmt = -1;
    // crop readback
    winrt::com_ptr<ID3D11Texture2D> m_cropRt;
    winrt::com_ptr<ID3D11RenderTargetView> m_cropRtv;
    winrt::com_ptr<ID3D11Texture2D> m_cropStaging[2];
    int m_cropPending = -1;            // staging slot holding an unread result
    int m_cropNext = 0;
    int64_t m_cropLastNs = 0;
    bool m_cropFullRange = false;
    CropDetector m_crop;

    VideoFramePtr m_current;           // picture on screen (kept: its decoder surface is being sampled)
    int m_frameW = 0, m_frameH = 0;    // visible size of the current stream
    LayoutAnimator m_anim;
    bool m_wasAnimating = false;       // draw one last frame at the exact target when an animation ends
    int64_t m_lastLogNs = 0;

    // ---- shared with the UI thread (m_paramMu)
    mutable std::mutex m_paramMu;
    ViewMode m_mode = ViewMode::Auto;
    float m_zoom = 1.0f;
    float m_panX = 0.5f, m_panY = 0.5f;
    RectF m_content = RectF::Unit();   // accepted crop rect (render thread writes)
    float m_panelW = 0, m_panelH = 0, m_scaleX = 1, m_scaleY = 1;
    ViewParams m_lastParams;           // last layout inputs, for clamping Pan on the UI thread
    float m_lastScale = 0;
    std::atomic<bool> m_autoCrop{true};
    std::atomic<bool> m_sizeDirty{false};
    std::atomic<bool> m_paramsDirty{false};
    std::atomic<int> m_syncInterval{1};

    // ---- stats (m_statsMu)
    std::mutex m_statsMu;
    RenderStats m_stats;
    RateWindow m_presentWin, m_latWin;
    std::atomic<int64_t> m_videoLatencyNs{0};
    double m_latEmaNs = 0;
};

}} // namespace XboxCast::Media
