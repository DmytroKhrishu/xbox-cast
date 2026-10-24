// xbox-cast media: MediaPipeline (docs/contract.md section 3). The IVideoSink + IAudioSink that ReceiverHost feeds, and the
// view controls MainPage drives. Owns the shared D3D11 device, the video decode thread, the render thread and the audio
// thread. See docs/media.md for the pipeline diagram, threading and latency budget.
//
// Lifecycle: default-constructible (ui/Bridge.cpp creates it on page load). Attach(panel) on the UI thread. Threads
// start lazily on the first OnFormat/OnFrame/OnPacket of a session; Stop() releases decoder, swap chain and audio
// voice and returns to idle; the next session starts them again.
//
// Additive to contract section 3 (requested by ui, docs/ui.md "Requests to media"): ResetPan().
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <memory>
#include <mutex>

#include "core/ReceiverHost.h"
#include "AudioOutput.h"
#include "D3DDevice.h"
#include "MediaPlatform.h"
#include "VideoDecoder.h"
#include "VideoRenderer.h"
#include "portable/MediaStats.h"
#include "portable/ViewTransform.h"

namespace XboxCast {
namespace Media {

class MediaPipeline final : public IVideoSink, public IAudioSink {
public:
    MediaPipeline();
    ~MediaPipeline();
    MediaPipeline(const MediaPipeline&) = delete;
    MediaPipeline& operator=(const MediaPipeline&) = delete;

    void Attach(winrt::Windows::UI::Xaml::Controls::SwapChainPanel const& panel); // UI thread
    void SetViewMode(ViewMode m);
    void SetZoom(float z);          // 1.0 .. 3.0, applies in Zoom mode
    void Pan(float dx, float dy);   // normalized, clamped to content
    void ResetPan();                // centre (additive)
    void SetAutoCrop(bool on);
    void SetAudioEnabled(bool on);
    void SetAudioOffsetMs(int ms);
    MediaStats Stats() const;
    void Stop();

    // IVideoSink (castcore threads)
    void OnFormat(xc_video_codec codec, int w, int h) override;
    void OnFrame(const xc_video_frame& frame) override;
    void OnFlush() override;               // shared by both sinks: video and audio flush together

    // IAudioSink (castcore threads)
    void OnFormat(xc_audio_codec codec, int rate, int ch, const uint8_t* cfg, size_t n) override;
    void OnPacket(const xc_audio_packet& packet) override;
    void OnVolume(float db) override;

private:
    std::shared_ptr<D3DDevice> CurrentDevice();
    void BindSwapChain(IDXGISwapChain1* sc);
    void EnsureVideoStarted();
    void EnsureAudioStarted();
    void UpdatePanelSize();

    // device (shared by decoder + renderer; recreated after a loss)
    std::mutex m_devMu;
    std::shared_ptr<D3DDevice> m_device;
    uint32_t m_devGen = 0;
    int64_t m_lastDeviceAttemptNs = 0;

    // XAML (UI thread)
    winrt::Windows::UI::Xaml::Controls::SwapChainPanel m_panel{nullptr};
    winrt::Windows::UI::Core::CoreDispatcher m_dispatcher{nullptr};
    winrt::event_token m_sizeToken{}, m_scaleToken{};

    std::unique_ptr<VideoRenderer> m_renderer;
    std::unique_ptr<VideoDecoder> m_decoder;
    std::unique_ptr<AudioOutput> m_audio;

    mutable std::mutex m_lifeMu;
    std::atomic<bool> m_videoStarted{false};
    std::atomic<bool> m_audioStarted{false};
    std::atomic<int> m_viewMode{(int)ViewMode::Auto};
};

} // namespace Media

// ui/Bridge.cpp names these ::XboxCast::MediaPipeline / ::XboxCast::ViewMode (contract section 3).
using Media::MediaPipeline;
using Media::MediaStats;
using Media::ViewMode;

} // namespace XboxCast
