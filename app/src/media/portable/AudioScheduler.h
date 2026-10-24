// xbox-cast media: audio timing (portable): dB->gain, fades, the A/V-sync jitter-buffer planner.
//
// Clock model. Every pts is on the xc_now_ns() clock. We want an audio sample with pts P to be heard at P + D.
//   D = max(arrivalLag + jitterTarget, videoLatency) + userOffset, never below arrivalLag + minBuffer.
//   * arrivalLag  = windowed max (2 s) of (arrival time - pts): how late packets reach us relative to their pts;
//   * jitterTarget = 40-60 ms for mirroring (absorbs Wi-Fi jitter + decode time);
//   * videoLatency = smoothed (present time - pts) reported by the renderer, so audio lines up with the picture
//     (video is never delayed for audio: it is presented as soon as it is decoded);
//   * userOffset   = audioOffsetMs setting (lip-sync trim; negative = earlier, clamped by the min buffer).
// The output keeps a "write cursor": the time the next submitted sample will be heard (now + queued/rate).
// Plan() compares where a decoded chunk should start (pts + D) with the write cursor and says how many frames of
// silence to insert or frames to skip. Within +-tolerance nothing is corrected (no audible fiddling).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <deque>

namespace XboxCast { namespace Media {

// AirPlay volume: -144 = mute, otherwise -30..0 dB.
float DbToGain(float db);

// Linear fade over the first rampFrames frames (interleaved float).
void ApplyFadeIn(float* pcm, int frames, int channels, int rampFrames);
void ApplyFadeOut(float* pcm, int frames, int channels, int rampFrames);

// Max of values seen in the last windowNs.
class WindowedMax {
public:
    explicit WindowedMax(int64_t windowNs = 2000000000LL) : m_windowNs(windowNs) {}
    void Push(int64_t nowNs, int64_t value);
    bool Empty() const { return m_q.empty(); }
    int64_t Max() const { return m_q.empty() ? 0 : m_q.front().second; }
    void Clear() { m_q.clear(); }

private:
    int64_t m_windowNs;
    std::deque<std::pair<int64_t, int64_t>> m_q; // monotonic deque (time, value), values decreasing
};

struct AudioPlan {
    int silenceFrames = 0; // insert this much silence before the chunk
    int skipFrames = 0;    // drop this many frames from the start of the chunk
    bool drop = false;     // whole chunk is too late
    bool fadeIn = false;   // ramp the (remaining) chunk in: start after silence/underrun/skip
    bool resync = false;   // a correction happened (counted in stats)
};

struct AudioSchedulerConfig {
    int sampleRate = 48000;
    int jitterTargetMs = 50;
    int minBufferMs = 15;
    int toleranceMs = 20;
    int maxSilenceMs = 250;
};

class AudioScheduler {
public:
    explicit AudioScheduler(const AudioSchedulerConfig& cfg = AudioSchedulerConfig()) : m_cfg(cfg) {}

    void Configure(const AudioSchedulerConfig& cfg) { m_cfg = cfg; }
    void Reset();
    void SetOffsetMs(int ms) { m_offsetNs = (int64_t)ms * 1000000; }
    // Smoothed video latency (present - pts); <= 0 means unknown/no video.
    void SetVideoLatencyNs(int64_t ns);
    void NoteArrival(int64_t ptsNs, int64_t arrivalNs);

    int64_t TargetDelayNs() const;
    // writeCursorNs: when the next submitted frame will be heard. starved: output had nothing queued.
    AudioPlan Plan(int64_t ptsNs, int frames, int64_t writeCursorNs, bool starved);

    int64_t ArrivalLagNs() const { return m_arrival.Max(); }
    int64_t VideoLatencyNs() const { return m_videoLatencyNs; }
    // last measured error (desired - write cursor) for stats, in ns
    int64_t LastErrorNs() const { return m_lastErrNs; }

private:
    int64_t FramesToNs(int64_t f) const { return f * 1000000000LL / m_cfg.sampleRate; }
    int64_t NsToFrames(int64_t ns) const { return ns * m_cfg.sampleRate / 1000000000LL; }

    AudioSchedulerConfig m_cfg;
    WindowedMax m_arrival;
    int64_t m_offsetNs = 0;
    int64_t m_videoLatencyNs = 0;
    int64_t m_lastErrNs = 0;
};

}} // namespace XboxCast::Media
