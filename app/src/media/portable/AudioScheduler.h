// xbox-cast media: audio timing (portable): dB->gain, fades, the A/V-sync jitter-buffer planner.
//
// Clock model. Every pts is on the xc_now_ns() clock. We want an audio sample with pts P to be heard at P + D.
//   D = max(arrivalLag + jitterTarget, videoLatency) + userOffset, never below arrivalLag + minBuffer.
//   * arrivalLag  = windowed max (2 s) of (arrival time - pts): how late packets reach us relative to their pts;
//   * jitterTarget = adaptive (absorbs Wi-Fi jitter + UxPlay's resend stalls): starts at 70 ms, +20 ms per real
//     underrun (cap 120), -5 ms per 10 s of clean playback (floor 50);
//   * videoLatency = smoothed (present time - pts) reported by the renderer, so audio lines up with the picture
//     (video is never delayed for audio: it is presented as soon as it is decoded);
//   * userOffset   = audioOffsetMs setting (lip-sync trim; negative = earlier, clamped by the min buffer).
// The output keeps a "write cursor": the time the next submitted sample will be heard (now + queued/rate).
// Plan() compares where a decoded chunk should start (pts + D) with the write cursor and says how many frames of
// silence to insert or frames to skip, but only when |error| stays above hardResyncMs for hardHoldMs of audio:
// Wi-Fi stalls deliver packets in bursts (UxPlay holds everything behind a lost packet until the resend), which
// look like large transient errors and must not each cost an audible skip. Everything smaller is corrected by
// slewing: a smoothed error drives a resample-ratio trim of up to +-maxSlewPpm (swr_set_compensation), which also
// absorbs the sender/Xbox clock drift. After a real underrun playback resumes after a short refill (fade in),
// never a full re-buffer.
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
    bool resync = false;   // a hard correction started (counted in stats)
    bool underrun = false; // output ran dry mid-stream (not a stream start / sender pause): target grew
    int slewPpm = 0;       // resample-ratio trim for the following audio (> 0: stretch, play later)
};

struct AudioSchedulerConfig {
    int sampleRate = 48000;
    int jitterTargetMs = 70;  // starting jitter target
    int jitterMinMs = 50;
    int jitterMaxMs = 120;    // audio is this much (+arrival lag) behind on-arrival video: keep lip sync tolerable
    int jitterGrowMs = 20;    // per underrun
    int jitterShrinkMs = 5;   // per jitterShrinkEveryMs of audio without an underrun
    int jitterShrinkEveryMs = 10000;
    int minBufferMs = 15;
    int slewDeadbandMs = 2;   // smoothed |error| below this: no slew
    int slewWindowMs = 1000;  // error smoothing time constant (averages out Wi-Fi jitter)
    int hardResyncMs = 40;    // |error| above this (was: 20 ms, at once) ...
    int hardHoldMs = 300;     // ... for this much audio -> hard correction
    int maxSlewPpm = 2000;    // +-0.2 % (3.5 cents, inaudible)
    int slewPpmPerMs = 250;   // slew gain on the smoothed error
    int underrunRefillPct = 50; // after an underrun, refill this share of the target before resuming
    int pauseMs = 1000;       // a pts gap above this is a sender pause / new stream, not an underrun
    int maxSilenceMs = 250;
};

class AudioScheduler {
public:
    explicit AudioScheduler(const AudioSchedulerConfig& cfg = AudioSchedulerConfig()) : m_cfg(cfg) { ResetJitter(); }

    void Configure(const AudioSchedulerConfig& cfg) { m_cfg = cfg; }
    // Stream state (flush). The learned jitter target survives: the network did not change.
    void Reset();
    // New stream / format: back to the starting jitter target.
    void ResetJitter() { m_jitterNs = (int64_t)m_cfg.jitterTargetMs * 1000000; }
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
    int64_t JitterTargetNs() const { return m_jitterNs; }
    int SlewPpm() const { return m_slewPpm; }

private:
    int64_t FramesToNs(int64_t f) const { return f * 1000000000LL / m_cfg.sampleRate; }
    int64_t NsToFrames(int64_t ns) const { return ns * m_cfg.sampleRate / 1000000000LL; }
    void HardCorrect(AudioPlan& p, int64_t err, int frames, int64_t maxSilenceNs) const;

    AudioSchedulerConfig m_cfg;
    WindowedMax m_arrival;
    int64_t m_offsetNs = 0;
    int64_t m_videoLatencyNs = 0;
    int64_t m_lastErrNs = 0;
    int64_t m_jitterNs = 0;
    bool m_anchored = false;    // a chunk was scheduled since Reset()
    bool m_correcting = false;  // hard correction in progress (a dropped chunk continues it)
    int64_t m_nextPts = 0;      // pts expected after the last chunk
    double m_errEmaNs = 0;
    int64_t m_beyondFrames = 0; // consecutive audio with |error| > hardResyncMs
    int64_t m_cleanFrames = 0;  // audio since the last underrun / shrink step
    int m_slewPpm = 0;
};

}} // namespace XboxCast::Media
