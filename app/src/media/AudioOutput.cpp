// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "AudioOutput.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "portable/MediaClock.h"

namespace XboxCast { namespace Media {

namespace {
const int64_t kLogIntervalNs = 10000000000LL;
const int kRampMs = 5;
}

AudioOutput::AudioOutput() = default;

AudioOutput::~AudioOutput() { Stop(); }

void AudioOutput::Start()
{
    if (m_running.exchange(true)) return;
    m_queue.Reopen();
    m_thread = std::thread([this] { ThreadMain(); });
}

void AudioOutput::Stop()
{
    if (!m_running.exchange(false)) return;
    m_queue.Close();
    if (m_thread.joinable()) m_thread.join();
    m_queue.Clear();
}

// ---------------------------------------------------------------- castcore threads

void AudioOutput::OnFormat(xc_audio_codec codec, int rate, int ch, const uint8_t* cfg, size_t n)
{
    Item it;
    it.kind = Item::Format;
    it.codec = codec;
    it.rate = rate;
    it.ch = ch;
    if (cfg && n) it.data.assign(cfg, cfg + n);
    MLOGI("audio", "format %s %d Hz %d ch, config %zu bytes", FfAudioDecoder::CodecName(codec), rate, ch, n);
    m_queue.PushEvict(std::move(it));
}

void AudioOutput::OnPacket(const xc_audio_packet& p)
{
    if (!p.data || !p.size) return;
    Item it;
    it.kind = Item::Packet;
    it.data.assign(p.data, p.data + p.size);
    it.ptsNs = p.pts_ns;
    it.arrivalNs = NowNs();
    if (m_queue.PushEvict(std::move(it)) > 0) {
        std::lock_guard<std::mutex> lk(m_statsMu);
        m_stats.dropped++;
    }
}

void AudioOutput::OnFlush()
{
    m_queue.RemoveIf([](const Item& q) { return q.kind == Item::Packet; });
    Item it;
    it.kind = Item::Flush;
    m_queue.PushEvict(std::move(it));
}

void AudioOutput::OnVolume(float db)
{
    m_volumeDb = db;
    m_volumeDirty = true;
    MLOGI("audio", "volume %.1f dB -> gain %.3f", db, DbToGain(db));
}

void AudioOutput::SetEnabled(bool on)
{
    if (m_enabled.exchange(on) != on) MLOGI("audio", "audio %s", on ? "enabled" : "disabled");
}

void AudioOutput::SetOffsetMs(int ms)
{
    ms = (std::max)(-300, (std::min)(300, ms));
    if (m_offsetMs.exchange(ms) != ms) MLOGI("audio", "audio offset %d ms", ms);
}

void STDMETHODCALLTYPE AudioOutput::OnCriticalError(HRESULT error)
{
    MLOGE("audio", "XAudio2 critical error %s (audio device change?): engine will be rebuilt", HrName(error));
    m_engineLost = true;
}

AudioOutStats AudioOutput::Stats()
{
    std::lock_guard<std::mutex> lk(m_statsMu);
    AudioOutStats s = m_stats;
    s.volumeDb = m_volumeDb.load();
    return s;
}

// ---------------------------------------------------------------- audio thread

void AudioOutput::ReleaseEngine()
{
    if (m_voice) {
        m_voice->DestroyVoice();
        m_voice = nullptr;
    }
    if (m_master) {
        m_master->DestroyVoice();
        m_master = nullptr;
    }
    if (m_xa) {
        m_xa->UnregisterForCallbacks(this);
        m_xa->StopEngine();
        m_xa = nullptr;
    }
    m_submittedFrames = 0;
    m_playing = false;
}

bool AudioOutput::EnsureEngine()
{
    if (m_engineLost.exchange(false) && m_xa) {
        ReleaseEngine();
        m_dec.Close(); // output rate may change
    }
    if (m_voice) return true;
    HRESULT hr = S_OK;
    if (!m_xa) {
        hr = XAudio2Create(m_xa.put(), 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(hr)) {
            MLOGE("audio", "XAudio2Create failed: %s", HrName(hr));
            m_xa = nullptr;
            return false;
        }
        m_xa->RegisterForCallbacks(this);
        hr = m_xa->CreateMasteringVoice(&m_master, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, nullptr,
                                        nullptr, AudioCategory_Media);
        if (FAILED(hr)) {
            MLOGE("audio", "CreateMasteringVoice failed: %s", HrName(hr));
            ReleaseEngine();
            return false;
        }
        XAUDIO2_VOICE_DETAILS d = {};
        m_master->GetVoiceDetails(&d);
        m_deviceRate = d.InputSampleRate > 0 ? (int)d.InputSampleRate : 48000;
        DWORD mask = 0;
        m_master->GetChannelMask(&mask);
        MLOGI("audio", "XAudio2 mastering voice %u Hz, %u ch (mask 0x%x)", d.InputSampleRate, d.InputChannels,
              (unsigned)mask);
    }
    WAVEFORMATEX wf = {};
    wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wf.nChannels = kChannels;
    wf.nSamplesPerSec = (DWORD)m_deviceRate;
    wf.wBitsPerSample = 32;
    wf.nBlockAlign = (WORD)(kChannels * 4);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    hr = m_xa->CreateSourceVoice(&m_voice, &wf, XAUDIO2_VOICE_NOPITCH, XAUDIO2_DEFAULT_FREQ_RATIO, nullptr, nullptr,
                                 nullptr);
    if (FAILED(hr)) {
        MLOGE("audio", "CreateSourceVoice(float %d Hz stereo) failed: %s", m_deviceRate, HrName(hr));
        ReleaseEngine();
        return false;
    }
    m_voice->SetVolume(DbToGain(m_volumeDb.load()));
    m_volumeDirty = false;
    m_voice->Start(0);
    m_submittedFrames = 0;
    m_playing = false;
    AudioSchedulerConfig sc;
    sc.sampleRate = m_deviceRate;
    m_sched.Configure(sc);
    {
        std::lock_guard<std::mutex> lk(m_statsMu);
        m_stats.outputRate = m_deviceRate;
    }
    return true;
}

int64_t AudioOutput::QueuedFrames()
{
    if (!m_voice) return 0;
    XAUDIO2_VOICE_STATE st = {};
    m_voice->GetState(&st, 0);
    if (st.BuffersQueued == 0) return 0;
    const int64_t q = (int64_t)m_submittedFrames - (int64_t)st.SamplesPlayed;
    return q > 0 ? q : 0;
}

void AudioOutput::FlushVoice(bool fadeTail)
{
    if (!m_voice) return;
    if (fadeTail && m_playing) {
        // XAudio2 ramps volume changes over one processing pass: fade the queued tail instead of cutting it
        m_voice->SetVolume(0.0f);
        Sleep(15);
    }
    // recreate the voice: SamplesPlayed restarts at 0 and the write-cursor bookkeeping stays exact
    m_voice->DestroyVoice();
    m_voice = nullptr;
    EnsureEngine();
}

bool AudioOutput::SubmitFrames(const float* pcm, int frames, bool fadeIn, bool fadeOut)
{
    if (!m_voice || frames <= 0) return false;
    XAUDIO2_VOICE_STATE st = {};
    m_voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (st.BuffersQueued >= kSlots - 1) {
        std::lock_guard<std::mutex> lk(m_statsMu);
        m_stats.dropped++;
        return false;
    }
    std::vector<float>& buf = m_slot[m_slotNext++ % kSlots];
    buf.resize((size_t)frames * kChannels);
    if (pcm) std::memcpy(buf.data(), pcm, buf.size() * sizeof(float));
    else std::fill(buf.begin(), buf.end(), 0.0f);
    const int ramp = m_deviceRate * kRampMs / 1000;
    if (fadeIn && pcm) ApplyFadeIn(buf.data(), frames, kChannels, ramp);
    if (fadeOut && pcm) ApplyFadeOut(buf.data(), frames, kChannels, ramp);
    XAUDIO2_BUFFER xb = {};
    xb.AudioBytes = (UINT32)(buf.size() * sizeof(float));
    xb.pAudioData = reinterpret_cast<const BYTE*>(buf.data());
    HRESULT hr = m_voice->SubmitSourceBuffer(&xb, nullptr);
    if (FAILED(hr)) {
        MLOGE("audio", "SubmitSourceBuffer failed: %s", HrName(hr));
        if (hr == XAUDIO2_E_DEVICE_INVALIDATED) m_engineLost = true;
        return false;
    }
    m_submittedFrames += (uint64_t)frames;
    m_playing = true;
    return true;
}

void AudioOutput::SubmitChunk(const float* pcm, int frames, int64_t ptsNs)
{
    m_sched.SetOffsetMs(m_offsetMs.load());
    m_sched.SetVideoLatencyNs(m_videoLatency ? m_videoLatency() : 0);
    const int64_t queued = QueuedFrames();
    const bool starved = queued == 0;
    const int64_t now = NowNs();
    const int64_t cursor = now + queued * 1000000000LL / m_deviceRate;
    AudioPlan p = m_sched.Plan(ptsNs, frames, cursor, starved);
    m_dec.SetCompensationPpm(p.slewPpm); // applies from the next decoded packet on
    {
        std::lock_guard<std::mutex> lk(m_statsMu);
        // a dry voice after a sender pause / stream start is not an underrun (the scheduler tells them apart)
        if (p.underrun) {
            m_stats.underruns++;
            if (m_stats.underruns <= 5 || (m_stats.underruns % 50) == 0)
                MLOGW("audio", "underrun #%llu (output ran dry): jitter target now %.0f ms, resuming after a %.0f ms refill",
                      (unsigned long long)m_stats.underruns, NsToMs(m_sched.JitterTargetNs()),
                      p.silenceFrames * 1000.0 / m_deviceRate);
        }
        if (p.resync) m_stats.resyncs++;
        m_stats.syncErrorMs = NsToMs(m_sched.LastErrorNs());
        m_stats.slewPpm = p.slewPpm;
    }
    if (starved) m_playing = false;
    if (p.drop) {
        std::lock_guard<std::mutex> lk(m_statsMu);
        m_stats.dropped++;
        return;
    }
    if (p.silenceFrames > 0) {
        // fade out what is queued? it ends on its own; the silence just delays the next chunk. Submit in pieces.
        int left = p.silenceFrames;
        while (left > 0) {
            int n = (std::min)(left, m_deviceRate / 50); // 20 ms pieces
            if (!SubmitFrames(nullptr, n, false, false)) break;
            left -= n;
        }
    }
    const int skip = (std::min)(p.skipFrames, frames);
    SubmitFrames(pcm + (size_t)skip * kChannels, frames - skip, p.fadeIn, false);
}

bool AudioOutput::OpenDecoder()
{
    m_dec.Close();
    if (!m_haveFormat) return false;
    bool ok = m_dec.Open(m_codec, m_rate, m_ch, m_cfg.empty() ? nullptr : m_cfg.data(), m_cfg.size(), m_deviceRate,
                         kChannels);
    std::lock_guard<std::mutex> lk(m_statsMu);
    m_stats.codec = FfAudioDecoder::CodecName(m_codec);
    m_stats.rate = m_rate;
    m_stats.channels = m_ch;
    if (!ok) MLOGE("audio", "audio decoder open failed: audio will be silent for this stream");
    return ok;
}

void AudioOutput::Handle(Item& it)
{
    if (it.kind == Item::Format) {
        m_codec = it.codec;
        m_rate = it.rate > 0 ? it.rate : 44100;
        m_ch = it.ch > 0 ? it.ch : 2;
        m_cfg = it.data;
        m_haveFormat = true;
        FlushVoice(true);
        m_sched.Reset();
        m_sched.ResetJitter(); // new stream: start from the default jitter target again
        OpenDecoder();
        return;
    }
    if (it.kind == Item::Flush) {
        FlushVoice(true);
        m_sched.Reset();
        if (m_dec.IsOpen()) m_dec.Flush();
        return;
    }
    {
        std::lock_guard<std::mutex> lk(m_statsMu);
        m_stats.packets++;
    }
    if (!m_haveFormat) return;
    if (!m_dec.IsOpen() || m_dec.OutRate() != m_deviceRate) {
        if (!OpenDecoder()) return;
    }
    m_sched.NoteArrival(it.ptsNs, it.arrivalNs);
    const bool enabled = m_enabled.load();
    if (!enabled && m_wasEnabled) FlushVoice(true);
    m_wasEnabled = enabled;
    int r = m_dec.Decode(it.data.data(), it.data.size(), it.ptsNs, [&](const float* pcm, int frames, int64_t pts) {
        if (enabled) SubmitChunk(pcm, frames, pts);
    });
    if (r < 0) {
        std::lock_guard<std::mutex> lk(m_statsMu);
        m_stats.decodeErrors++;
    }
}

void AudioOutput::ThreadMain()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    MLOGI("audio", "audio thread started");
    while (m_running.load()) {
        if (!EnsureEngine()) {
            Item drop;
            m_queue.Pop(drop, std::chrono::milliseconds(500)); // no engine: discard, retry periodically
            continue;
        }
        if (m_volumeDirty.exchange(false) && m_voice) m_voice->SetVolume(DbToGain(m_volumeDb.load()));
        Item it;
        if (m_queue.Pop(it, std::chrono::milliseconds(20))) Handle(it);
        const int64_t now = NowNs();
        {
            std::lock_guard<std::mutex> lk(m_statsMu);
            m_stats.bufferMs = m_voice ? (double)QueuedFrames() * 1000.0 / m_deviceRate : 0;
            m_stats.targetMs = NsToMs(m_sched.TargetDelayNs());
            m_stats.jitterMs = NsToMs(m_sched.JitterTargetNs());
            m_stats.arrivalLagMs = NsToMs(m_sched.ArrivalLagNs());
            m_stats.active = m_playing;
        }
        if (now - m_lastLogNs >= kLogIntervalNs && m_haveFormat) {
            m_lastLogNs = now;
            AudioOutStats s = Stats();
            MLOGI("audio",
                  "%s %d Hz %d ch -> %d Hz: buffer %.1f ms, target %.1f ms (jitter %.0f, arrival lag %.1f, video %.1f, "
                  "offset %d), slew %d ppm, sync error %.1f ms, packets %llu, underruns %llu, hard resyncs %llu, "
                  "dropped %llu, errors %llu, %s",
                  s.codec.c_str(), s.rate, s.channels, s.outputRate, s.bufferMs, s.targetMs, s.jitterMs, s.arrivalLagMs,
                  NsToMs(m_sched.VideoLatencyNs()), m_offsetMs.load(), s.slewPpm, s.syncErrorMs,
                  (unsigned long long)s.packets, (unsigned long long)s.underruns, (unsigned long long)s.resyncs,
                  (unsigned long long)s.dropped, (unsigned long long)s.decodeErrors,
                  m_enabled.load() ? "enabled" : "DISABLED");
        }
    }
    m_dec.Close();
    ReleaseEngine();
    MLOGI("audio", "audio thread stopped");
}

}} // namespace XboxCast::Media
