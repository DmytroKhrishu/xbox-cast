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
    int64_t d = lag + (int64_t)m_cfg.jitterTargetMs * 1000000;
    if (m_videoLatencyNs > 0) d = (std::max)(d, m_videoLatencyNs);
    d += m_offsetNs;
    const int64_t minD = lag + (int64_t)m_cfg.minBufferMs * 1000000;
    return (std::max)(d, minD);
}

AudioPlan AudioScheduler::Plan(int64_t ptsNs, int frames, int64_t writeCursorNs, bool starved)
{
    AudioPlan p;
    const int64_t desired = ptsNs + TargetDelayNs();
    const int64_t err = desired - writeCursorNs; // > 0: chunk is early (gap needed), < 0: chunk is late
    m_lastErrNs = err;
    const int64_t tol = (int64_t)m_cfg.toleranceMs * 1000000;
    if (!starved && err >= -tol && err <= tol) return p;

    p.resync = true;
    p.fadeIn = true;
    if (err > 0) {
        const int64_t maxSil = (int64_t)m_cfg.maxSilenceMs * 1000000;
        p.silenceFrames = (int)NsToFrames((std::min)(err, maxSil));
    } else if (err < 0) {
        int64_t skip = NsToFrames(-err);
        if (skip >= frames) {
            p.drop = true;
            p.skipFrames = frames;
        } else {
            p.skipFrames = (int)skip;
        }
    }
    return p;
}

}} // namespace XboxCast::Media
