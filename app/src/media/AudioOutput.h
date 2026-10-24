// xbox-cast media: audio path. castcore packets -> FFmpeg decode (AAC-ELD / ALAC / AAC-LC / PCM) -> float stereo at the
// mastering voice's rate (swresample) -> XAudio2 source voice.
//
// Threads: castcore calls OnFormat/OnPacket/OnFlush/OnVolume and returns at once (copy into a FIFO). One audio thread
// decodes, plans and submits. XAudio2 runs its own mixing thread.
//
// Timing (portable/AudioScheduler.h): a sample with pts P should be heard at P + D,
//   D = max(arrival lag + 50 ms jitter target, video latency) + audioOffsetMs.
// The write cursor = now + (frames submitted - frames played) / rate. Each decoded chunk is compared with it: early ->
// silence is inserted, late -> the start is skipped (or the chunk dropped), within +-20 ms nothing happens. Every
// correction and every restart after an underrun ramps in over 5 ms (no clicks); a flush ramps the queued tail out.
//
// Volume: OnVolume(db) -> DbToGain (-144 = mute, -30..0 dB) on the source voice. SetAudioEnabled(false) stops
// submitting and flushes the voice (packets are still parsed so the clock model stays warm).
// Engine loss (headset/HDMI change, XAUDIO2_E_DEVICE_INVALIDATED): OnCriticalError flags it, the audio thread rebuilds
// the engine and reopens the decoder at the new device rate.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "MediaPlatform.h"
#include "portable/AudioScheduler.h"
#include "portable/FfAudioDecoder.h"
#include "portable/MediaStats.h"
#include "portable/Queues.h"

namespace XboxCast { namespace Media {

struct AudioOutStats {
    bool active = false;
    std::string codec;
    int rate = 0, channels = 0, outputRate = 0;
    double bufferMs = 0, targetMs = 0, arrivalLagMs = 0, syncErrorMs = 0;
    float volumeDb = 0;
    uint64_t packets = 0, decodeErrors = 0, underruns = 0, resyncs = 0, dropped = 0;
};

class AudioOutput : public IXAudio2EngineCallback {
public:
    AudioOutput();
    virtual ~AudioOutput();

    void Start();
    void Stop();

    // castcore threads
    void OnFormat(xc_audio_codec codec, int rate, int ch, const uint8_t* cfg, size_t n);
    void OnPacket(const xc_audio_packet& p);
    void OnFlush();
    void OnVolume(float db);

    // any thread
    void SetEnabled(bool on);
    void SetOffsetMs(int ms);
    // Smoothed video latency (present - pts) source, read on the audio thread; set before Start().
    void SetVideoLatencySource(std::function<int64_t()> f) { m_videoLatency = std::move(f); }
    bool Enabled() const { return m_enabled.load(); }
    int OffsetMs() const { return m_offsetMs.load(); }
    AudioOutStats Stats();

    // IXAudio2EngineCallback (XAudio2 thread)
    void STDMETHODCALLTYPE OnProcessingPassStart() override {}
    void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnCriticalError(HRESULT error) override;

private:
    struct Item {
        enum Kind { Packet, Format, Flush } kind = Packet;
        std::vector<uint8_t> data;
        int64_t ptsNs = 0, arrivalNs = 0;
        xc_audio_codec codec = XC_AUDIO_AAC_ELD;
        int rate = 0, ch = 0;
    };

    void ThreadMain();
    bool EnsureEngine();
    void ReleaseEngine();
    bool OpenDecoder();
    void Handle(Item& it);
    void SubmitChunk(const float* pcm, int frames, int64_t ptsNs);
    bool SubmitFrames(const float* pcm, int frames, bool fadeIn, bool fadeOut);
    void FlushVoice(bool fadeTail);
    int64_t QueuedFrames();

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    BoundedQueue<Item> m_queue{256};
    std::atomic<bool> m_enabled{true};
    std::atomic<int> m_offsetMs{0};
    std::atomic<float> m_volumeDb{0.0f};
    std::atomic<bool> m_volumeDirty{true};
    std::atomic<bool> m_engineLost{false};
    std::function<int64_t()> m_videoLatency;

    // audio thread
    winrt::com_ptr<IXAudio2> m_xa;
    IXAudio2MasteringVoice* m_master = nullptr;
    IXAudio2SourceVoice* m_voice = nullptr;
    int m_deviceRate = 48000;
    static const int kChannels = 2;
    static const int kSlots = 48;                 // < XAUDIO2_MAX_QUEUED_BUFFERS
    std::vector<float> m_slot[kSlots];
    uint64_t m_slotNext = 0;
    uint64_t m_submittedFrames = 0;               // since the voice was (re)started
    bool m_playing = false;                       // something was submitted since the last flush/underrun
    FfAudioDecoder m_dec;
    AudioScheduler m_sched;
    bool m_haveFormat = false;
    xc_audio_codec m_codec = XC_AUDIO_AAC_ELD;
    int m_rate = 44100, m_ch = 2;
    std::vector<uint8_t> m_cfg;
    std::vector<float> m_scratch;
    bool m_wasEnabled = true;
    int64_t m_lastLogNs = 0;

    std::mutex m_statsMu;
    AudioOutStats m_stats;
};

}} // namespace XboxCast::Media
