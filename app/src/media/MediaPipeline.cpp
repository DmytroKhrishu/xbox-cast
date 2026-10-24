// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "MediaPipeline.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "diag/Diag.h"
#include "portable/MediaClock.h"

namespace XboxCast { namespace Media {

namespace {

void DiagSink(LogLevel level, const char* tag, const char* message)
{
    ::XboxCast::Diag::LogRaw((::XboxCast::Diag::Level)(int)level, tag, message);
}

// FFmpeg's own warnings/errors (bitstream damage, hwaccel refusals) into the Diag log. Debug/info chatter is dropped.
void FfLogCallback(void* avcl, int level, const char* fmt, va_list vl)
{
    if (level > AV_LOG_WARNING) return;
    static thread_local int printPrefix = 1;
    char line[1024];
    av_log_format_line2(avcl, level, fmt, vl, line, sizeof(line), &printPrefix);
    size_t n = std::strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (!n) return;
    ::XboxCast::Diag::LogRaw(level <= AV_LOG_ERROR ? ::XboxCast::Diag::Level::Error : ::XboxCast::Diag::Level::Warn,
                             "video", line);
}

const int64_t kDeviceRetryNs = 1000000000LL;

} // namespace

MediaPipeline::MediaPipeline()
{
    SetLogSink(&DiagSink);
    av_log_set_callback(&FfLogCallback);
    MLOGI("video", "media pipeline created (libavcodec %s)", LIBAVCODEC_IDENT);

    m_renderer.reset(new VideoRenderer([this] { return CurrentDevice(); },
                                       [this](IDXGISwapChain1* sc) { BindSwapChain(sc); }));
    m_decoder.reset(new VideoDecoder([this] { return CurrentDevice(); }, m_renderer.get()));
    m_audio.reset(new AudioOutput());
    m_audio->SetVideoLatencySource([this] { return m_renderer->VideoLatencyNs(); });

    ::XboxCast::Diag::SetStatusProvider("media", [this]() {
        try {
            winrt::Windows::Data::Json::JsonObject o{nullptr};
            if (winrt::Windows::Data::Json::JsonObject::TryParse(winrt::to_hstring(ToJson(Stats())), o) && o) return o;
        } catch (...) {
        }
        return winrt::Windows::Data::Json::JsonObject();
    });
}

MediaPipeline::~MediaPipeline()
{
    ::XboxCast::Diag::SetStatusProvider("media", nullptr);
    Stop();
    if (m_panel) {
        try {
            m_panel.SizeChanged(m_sizeToken);
            m_panel.CompositionScaleChanged(m_scaleToken);
        } catch (...) {
        }
    }
}

// ---------------------------------------------------------------- device + XAML

std::shared_ptr<D3DDevice> MediaPipeline::CurrentDevice()
{
    std::lock_guard<std::mutex> lk(m_devMu);
    if (m_device && m_device->CheckLost("pipeline")) {
        MLOGW("render", "device gen %u removed: a new device will be created", m_device->Generation());
    }
    if (m_device && !m_device->Lost()) return m_device;
    const int64_t now = NowNs();
    if (m_lastDeviceAttemptNs && now - m_lastDeviceAttemptNs < kDeviceRetryNs) return nullptr;
    m_lastDeviceAttemptNs = now;
    m_device.reset(); // decoder + renderer hold their own refs until they notice the loss
    auto d = D3DDevice::Create(++m_devGen);
    if (d) {
        m_device = d;
        m_lastDeviceAttemptNs = 0;
        MLOGI("render", "D3D11 device gen %u: %s%s", d->Generation(), d->Description().c_str(),
              d->IsWarp() ? " (WARP software rasterizer)" : "");
    }
    return m_device;
}

void MediaPipeline::BindSwapChain(IDXGISwapChain1* sc)
{
    // Render thread. ISwapChainPanelNative::SetSwapChain must run on the UI thread: post it, never wait for it
    // (the UI thread may itself be blocked in Stop() joining this thread).
    winrt::com_ptr<IDXGISwapChain1> keep;
    keep.copy_from(sc);
    auto panel = m_panel;
    auto dispatcher = m_dispatcher;
    if (!panel || !dispatcher) {
        MLOGW("render", "swap chain %s before Attach(): not bound", sc ? "ready" : "released");
        return;
    }
    auto apply = [panel, keep]() {
        try {
            auto native = panel.as<ISwapChainPanelNative>();
            HRESULT hr = native->SetSwapChain(keep.get());
            if (FAILED(hr)) MLOGE("render", "ISwapChainPanelNative::SetSwapChain failed: %s", HrName(hr));
            else MLOGI("render", "swap chain %s SwapChainPanel", keep ? "bound to" : "unbound from");
        } catch (winrt::hresult_error const& e) {
            MLOGE("render", "SetSwapChain threw 0x%08x", (unsigned)e.code());
        }
    };
    if (dispatcher.HasThreadAccess()) apply();
    else dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::High, apply);
}

void MediaPipeline::UpdatePanelSize()
{
    if (!m_panel) return;
    const float w = (float)m_panel.ActualWidth();
    const float h = (float)m_panel.ActualHeight();
    const float sx = m_panel.CompositionScaleX();
    const float sy = m_panel.CompositionScaleY();
    m_renderer->SetPanelSize(w, h, sx, sy);
}

void MediaPipeline::Attach(winrt::Windows::UI::Xaml::Controls::SwapChainPanel const& panel)
{
    if (m_panel) {
        try {
            m_panel.SizeChanged(m_sizeToken);
            m_panel.CompositionScaleChanged(m_scaleToken);
        } catch (...) {
        }
    }
    m_panel = panel;
    m_dispatcher = panel ? panel.Dispatcher() : nullptr;
    if (!panel) return;
    m_sizeToken = panel.SizeChanged([this](auto const&, auto const&) { UpdatePanelSize(); });
    m_scaleToken = panel.CompositionScaleChanged([this](auto const&, auto const&) { UpdatePanelSize(); });
    UpdatePanelSize();
    MLOGI("render", "attached to SwapChainPanel %.0fx%.0f dips, composition scale %.2fx%.2f", panel.ActualWidth(),
          panel.ActualHeight(), panel.CompositionScaleX(), panel.CompositionScaleY());
}

// ---------------------------------------------------------------- lifecycle

void MediaPipeline::EnsureVideoStarted()
{
    if (m_videoStarted.load()) return;
    std::unique_lock<std::mutex> lk(m_lifeMu, std::try_to_lock);
    if (!lk.owns_lock() || m_videoStarted.load()) return; // Stop() in progress or another thread won
    m_renderer->Start();
    m_decoder->Start();
    m_videoStarted = true;
    MLOGI("video", "video path started");
}

void MediaPipeline::EnsureAudioStarted()
{
    if (m_audioStarted.load()) return;
    std::unique_lock<std::mutex> lk(m_lifeMu, std::try_to_lock);
    if (!lk.owns_lock() || m_audioStarted.load()) return;
    m_audio->Start();
    m_audioStarted = true;
    MLOGI("audio", "audio path started");
}

void MediaPipeline::Stop()
{
    std::lock_guard<std::mutex> lk(m_lifeMu);
    const bool v = m_videoStarted.exchange(false);
    const bool a = m_audioStarted.exchange(false);
    if (v) {
        m_decoder->Stop();
        m_renderer->Stop();
    }
    if (a) m_audio->Stop();
    {
        std::lock_guard<std::mutex> dl(m_devMu);
        m_device.reset(); // idle: no GPU memory held (decoder + renderer dropped their refs in Stop)
    }
    if (v || a) MLOGI("video", "media pipeline stopped (video %d, audio %d): idle", v ? 1 : 0, a ? 1 : 0);
}

// ---------------------------------------------------------------- controls

void MediaPipeline::SetViewMode(ViewMode m)
{
    m_viewMode = (int)m;
    m_renderer->SetViewMode(m);
}

void MediaPipeline::SetZoom(float z) { m_renderer->SetZoom(z); }
void MediaPipeline::Pan(float dx, float dy) { m_renderer->Pan(dx, dy); }
void MediaPipeline::ResetPan() { m_renderer->ResetPan(); }
void MediaPipeline::SetAutoCrop(bool on) { m_renderer->SetAutoCrop(on); }
void MediaPipeline::SetAudioEnabled(bool on) { m_audio->SetEnabled(on); }
void MediaPipeline::SetAudioOffsetMs(int ms) { m_audio->SetOffsetMs(ms); }

// ---------------------------------------------------------------- sinks (castcore threads)

void MediaPipeline::OnFormat(xc_video_codec codec, int w, int h)
{
    EnsureVideoStarted();
    m_decoder->OnFormat(codec, w, h);
}

void MediaPipeline::OnFrame(const xc_video_frame& frame)
{
    EnsureVideoStarted();
    m_decoder->OnFrame(frame);
}

void MediaPipeline::OnFlush()
{
    if (m_videoStarted.load()) m_decoder->OnFlush();
    if (m_audioStarted.load()) m_audio->OnFlush();
}

void MediaPipeline::OnFormat(xc_audio_codec codec, int rate, int ch, const uint8_t* cfg, size_t n)
{
    EnsureAudioStarted();
    m_audio->OnFormat(codec, rate, ch, cfg, n);
}

void MediaPipeline::OnPacket(const xc_audio_packet& packet)
{
    EnsureAudioStarted();
    m_audio->OnPacket(packet);
}

void MediaPipeline::OnVolume(float db) { m_audio->OnVolume(db); }

// ---------------------------------------------------------------- stats

MediaStats MediaPipeline::Stats() const
{
    MediaStats s;
    VideoDecodeStats d = m_decoder->Stats();
    RenderStats r = m_renderer->Stats();
    AudioOutStats a = m_audio->Stats();

    s.videoActive = m_videoStarted.load() && d.active;
    s.codec = d.codec;
    s.decoder = d.decoder;
    s.pixelFormat = r.pixelFormat.empty() ? d.swFormat : r.pixelFormat;
    s.colorMatrix = r.colorMatrix;
    s.codedWidth = d.codedW;
    s.codedHeight = d.codedH;
    s.viewWidth = r.viewW;
    s.viewHeight = r.viewH;
    s.inputFps = d.inputFps;
    s.decodeFps = d.decodeFps;
    s.renderFps = r.renderFps;
    s.decodeMsAvg = d.decodeMsAvg;
    s.decodeMsMax = d.decodeMsMax;
    s.decodeInOutMsAvg = d.inOutMsAvg;
    s.decodeInOutMsMax = d.inOutMsMax;
    s.decodeHeldMax = d.heldMax;
    s.spsRewritten = d.spsRewritten;
    s.latencyMsAvg = r.latencyMsAvg;
    s.latencyMsMax = r.latencyMsMax;
    s.presentMsAvg = r.presentMsAvg;
    s.decodeQueue = d.queue;
    s.renderQueue = r.pending;
    s.framesIn = d.framesIn;
    s.framesDecoded = d.framesDecoded;
    s.framesPresented = r.presented;
    s.framesDroppedQueue = d.droppedQueue;
    s.framesDroppedRender = d.skippedBacklog + r.dropped;
    s.decodeErrors = d.errors;
    s.viewMode = ViewModeName((ViewMode)m_viewMode.load());
    s.resolvedMode = ViewModeName(r.resolved);
    s.zoom = m_renderer->Zoom();
    s.autoCrop = m_renderer->AutoCrop();
    s.crop = r.crop;

    s.audioActive = m_audioStarted.load() && a.active;
    s.audioEnabled = m_audio->Enabled();
    s.audioCodec = a.codec;
    s.audioRate = a.rate;
    s.audioChannels = a.channels;
    s.outputRate = a.outputRate;
    s.audioBufferMs = a.bufferMs;
    s.audioTargetMs = a.targetMs;
    s.audioJitterMs = a.jitterMs;
    s.audioSlewPpm = a.slewPpm;
    s.audioArrivalLagMs = a.arrivalLagMs;
    s.audioSyncErrorMs = a.syncErrorMs;
    s.audioOffsetMs = m_audio->OffsetMs();
    s.volumeDb = a.volumeDb;
    s.audioPackets = a.packets;
    s.audioDecodeErrors = a.decodeErrors;
    s.audioUnderruns = a.underruns;
    s.audioResyncs = a.resyncs;
    s.audioDropped = a.dropped;
    return s;
}

}} // namespace XboxCast::Media
