// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "AudioScheduler.h"

#include <algorithm>
#include <cmath>

namespace XboxCast { namespace Media {

float DbToGain(float db)
{
    if (db <= -100.0f) return 0.0f; // -144 = mute
    if (db > 0.0f) db = 0.0f;
    if (db < -30.0f) db = -30.0f;
    return std::pow(10.0f, db / 20.0f);
}

void ApplyFadeIn(float* pcm, int frames, int channels, int rampFrames)
{
    int n = (std::min)(frames, rampFrames);
    for (int i = 0; i < n; i++) {
        float g = (float)(i + 1) / (float)(n + 1);
        for (int c = 0; c < channels; c++) pcm[(size_t)i * channels + c] *= g;
    }
}

void ApplyFadeOut(float* pcm, int frames, int channels, int rampFrames)
{
    int n = (std::min)(frames, rampFrames);
    for (int i = 0; i < n; i++) {
        float g = (float)(n - i) / (float)(n + 1);
        size_t idx = (size_t)(frames - n + i) * channels;
        for (int c = 0; c < channels; c++) pcm[idx + c] *= g;
    }
}

void WindowedMax::Push(int64_t nowNs, int64_t value)
{
    while (!m_q.empty() && m_q.back().second <= value) m_q.pop_back();
    m_q.emplace_back(nowNs, value);
    while (!m_q.empty() && nowNs - m_q.front().first > m_windowNs) m_q.pop_front();
}

void AudioScheduler::Reset()
{
    m_arrival.Clear();
    m_videoLatencyNs = 0;
    m_lastErrNs = 0;
    m_anchored = false;
    m_correcting = false;
    m_nextPts = 0;
    m_errEmaNs = 0;
    m_beyondFrames = 0;
    m_cleanFrames = 0;
    m_slewPpm = 0;
}

void AudioScheduler::SetVideoLatencyNs(int64_t ns)
{
    // hysteresis: ignore wobble below 8 ms so the target doesn't creep and trigger corrections
    if (ns <= 0) { m_videoLatencyNs = 0; return; }
    if (m_videoLatencyNs == 0 || std::llabs(ns - m_videoLatencyNs) > 8000000) m_videoLatencyNs = ns;
}

void AudioScheduler::NoteArrival(int64_t ptsNs, int64_t arrivalNs) { m_arrival.Push(arrivalNs, arrivalNs - ptsNs); }

int64_t AudioScheduler::TargetDelayNs() const
{
    const int64_t lag = m_arrival.Empty() ? 0 : m_arrival.Max();
    int64_t d = lag + m_jitterNs;
    if (m_videoLatencyNs > 0) d = (std::max)(d, m_videoLatencyNs);
    d += m_offsetNs;
    const int64_t minD = lag + (int64_t)m_cfg.minBufferMs * 1000000;
    return (std::max)(d, minD);
}

// Silence for an early chunk, skip/drop for a late one.
void AudioScheduler::HardCorrect(AudioPlan& p, int64_t err, int frames, int64_t maxSilenceNs) const
{
    p.fadeIn = true;
    if (err > 0) {
        p.silenceFrames = (int)NsToFrames((std::min)(err, maxSilenceNs));
    } else if (err < 0) {
        const int64_t skip = NsToFrames(-err);
        p.drop = skip >= frames;
        p.skipFrames = p.drop ? frames : (int)skip;
    }
}

AudioPlan AudioScheduler::Plan(int64_t ptsNs, int frames, int64_t writeCursorNs, bool starved)
{
    AudioPlan p;
    const int64_t ms = 1000000;
    // A pts jump (sender paused audio, new stream) is a restart, not an underrun.
    const bool restart = !m_anchored || std::llabs(ptsNs - m_nextPts) > m_cfg.pauseMs * ms;
    m_nextPts = ptsNs + FramesToNs(frames);

    if (starved) {
        if (!restart) {
            // Ran dry mid-stream: the network needs more cushion. Resume after a short refill, not the full target
            // (the stall's backlog arrives right behind this chunk), and never skip here: the slew pulls sync back.
            p.underrun = true;
            m_jitterNs = (std::min)(m_jitterNs + m_cfg.jitterGrowMs * ms, m_cfg.jitterMaxMs * ms);
        }
        m_anchored = true;
        m_correcting = false;
        m_errEmaNs = 0;
        m_beyondFrames = 0;
        m_cleanFrames = 0;
        m_slewPpm = 0;
        const int64_t err = ptsNs + TargetDelayNs() - writeCursorNs;
        m_lastErrNs = err;
        int64_t maxSil = m_cfg.maxSilenceMs * ms;
        if (p.underrun) maxSil = (std::min)(maxSil, TargetDelayNs() * m_cfg.underrunRefillPct / 100);
        if (p.underrun && err < 0) p.fadeIn = true;
        else HardCorrect(p, err, frames, maxSil);
        return p;
    }

    m_cleanFrames += frames;
    if (m_cleanFrames >= NsToFrames(m_cfg.jitterShrinkEveryMs * ms)) {
        m_cleanFrames = 0;
        m_jitterNs = (std::max)(m_jitterNs - m_cfg.jitterShrinkMs * ms, m_cfg.jitterMinMs * ms);
    }

    const int64_t err = ptsNs + TargetDelayNs() - writeCursorNs; // > 0: chunk is early, < 0: chunk is late
    m_lastErrNs = err;

    // Hard correction only for a large error that persists: a burst after a stall is large but transient.
    if (!m_correcting) {
        m_beyondFrames = std::llabs(err) > m_cfg.hardResyncMs * ms ? m_beyondFrames + frames : 0;
        if (m_beyondFrames >= NsToFrames(m_cfg.hardHoldMs * ms)) {
            m_correcting = true;
            p.resync = true;
        }
    }
    if (m_correcting) {
        m_beyondFrames = 0;
        m_errEmaNs = 0;
        m_slewPpm = 0;
        HardCorrect(p, err, frames, m_cfg.maxSilenceMs * ms);
        if (!p.drop) m_correcting = false; // this chunk lands on the target
        return p;
    }

    // Everything else: slew. Smoothed error -> resample-ratio trim (also absorbs sender/Xbox clock drift).
    const double alpha = (std::min)(1.0, (double)frames / (double)NsToFrames(m_cfg.slewWindowMs * ms));
    m_errEmaNs += alpha * ((double)err - m_errEmaNs);
    const double e = m_errEmaNs / 1.0e6, dead = m_cfg.slewDeadbandMs;
    double ppm = std::fabs(e) <= dead ? 0.0 : (e > 0 ? e - dead : e + dead) * m_cfg.slewPpmPerMs;
    ppm = (std::max)(-(double)m_cfg.maxSlewPpm, (std::min)((double)m_cfg.maxSlewPpm, ppm));
    m_slewPpm = p.slewPpm = (int)std::lround(ppm);
    return p;
}

}} // namespace XboxCast::Media
