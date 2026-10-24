// xbox-cast media: statistics (portable). MediaPipeline::Stats() returns a MediaStats snapshot; ToJson() is what
// the Diag stats provider puts under "media" in GET /status.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "ViewTransform.h"

namespace XboxCast { namespace Media {

struct MediaStats {
    // video
    bool        videoActive = false;
    std::string codec;            // "h264" | "hevc" | ""
    std::string decoder;          // "d3d11va" | "software" | ""
    std::string pixelFormat;      // "nv12" | "p010" | "yuv420p" ...
    std::string colorMatrix;      // "BT.709 limited" ...
    int         codedWidth = 0, codedHeight = 0;
    int         viewWidth = 0, viewHeight = 0;   // swap chain size
    double      inputFps = 0;     // frames arriving from castcore
    double      decodeFps = 0;
    double      renderFps = 0;    // presents of a new video frame per second
    double      decodeMsAvg = 0, decodeMsMax = 0;
    double      decodeInOutMsAvg = 0, decodeInOutMsMax = 0; // packet into avcodec -> its picture out (last second)
    int         decodeHeldMax = 0;  // most packets a picture waited behind since stream start (0 = 1-in-1-out)
    int         spsRewritten = 0;   // H.264 SPS rewritten to max_num_reorder_frames=0 (see H264Sps.h)
    double      latencyMsAvg = 0, latencyMsMax = 0;   // present time - pts (xc_now_ns clock), last second
    double      presentMsAvg = 0;                     // Present() call duration
    int         decodeQueue = 0;  // packets waiting for the decoder
    int         renderQueue = 0;  // decoded frame waiting for the renderer (0/1)
    uint64_t    framesIn = 0, framesDecoded = 0, framesPresented = 0;
    uint64_t    framesDroppedQueue = 0;   // dropped before decode (backlog overflow / waiting for keyframe)
    uint64_t    framesDroppedRender = 0;  // decoded but replaced before present (render slower than decode)
    uint64_t    decodeErrors = 0;
    std::string viewMode;         // requested mode
    std::string resolvedMode;     // Auto -> Fit/Fill
    float       zoom = 1.0f;
    bool        autoCrop = true;
    RectF       crop = RectF::Unit();  // detected content rect (normalized)
    // audio
    bool        audioActive = false;
    bool        audioEnabled = true;
    std::string audioCodec;
    int         audioRate = 0, audioChannels = 0, outputRate = 0;
    double      audioBufferMs = 0;    // queued in XAudio2 + pending
    double      audioTargetMs = 0;    // target delay pts -> heard
    double      audioArrivalLagMs = 0;
    double      audioSyncErrorMs = 0; // last measured error
    int         audioOffsetMs = 0;
    float       volumeDb = 0;
    uint64_t    audioPackets = 0, audioDecodeErrors = 0, audioUnderruns = 0, audioResyncs = 0, audioDropped = 0;
};

std::string ToJson(const MediaStats& s);

// Accumulates samples over ~1 s windows; Snapshot reports the last completed window.
class RateWindow {
public:
    void Add(int64_t nowNs, double value);  // value used for avg/max (e.g. ms)
    void Tick(int64_t nowNs);               // closes the window if >= 1 s old
    double Rate() const { return m_rate; }
    double Avg() const { return m_avg; }
    double Max() const { return m_max; }
    void Reset();

private:
    void Roll(int64_t nowNs);
    int64_t m_start = 0;
    uint64_t m_n = 0;
    double m_sum = 0, m_curMax = 0;
    double m_rate = 0, m_avg = 0, m_max = 0;
};

// Thread-safe wrapper (producer threads Add, Stats() reads).
class SharedRateWindow {
public:
    void Add(int64_t nowNs, double value)
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_w.Add(nowNs, value);
    }
    void Read(int64_t nowNs, double* rate, double* avg, double* max)
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_w.Tick(nowNs);
        if (rate) *rate = m_w.Rate();
        if (avg) *avg = m_w.Avg();
        if (max) *max = m_w.Max();
    }
    void Reset()
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_w.Reset();
    }

private:
    std::mutex m_mu;
    RateWindow m_w;
};

}} // namespace XboxCast::Media
