// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "MediaStats.h"

#include <cstdio>

namespace XboxCast { namespace Media {

void RateWindow::Roll(int64_t nowNs)
{
    double secs = (double)(nowNs - m_start) / 1e9;
    m_rate = secs > 0 ? (double)m_n / secs : 0;
    m_avg = m_n ? m_sum / (double)m_n : 0;
    m_max = m_curMax;
    m_start = nowNs;
    m_n = 0;
    m_sum = 0;
    m_curMax = 0;
}

void RateWindow::Tick(int64_t nowNs)
{
    if (m_start == 0) { m_start = nowNs; return; }
    int64_t age = nowNs - m_start;
    if (age >= 1000000000LL) {
        if (age > 3000000000LL) { // idle for a while: report zeros instead of a stale average
            m_rate = 0; m_avg = 0; m_max = 0;
            m_start = nowNs; m_n = 0; m_sum = 0; m_curMax = 0;
            return;
        }
        Roll(nowNs);
    }
}

void RateWindow::Add(int64_t nowNs, double value)
{
    Tick(nowNs);
    m_n++;
    m_sum += value;
    if (value > m_curMax) m_curMax = value;
}

void RateWindow::Reset()
{
    *this = RateWindow();
}

namespace {
void AppendEscaped(std::string& out, const std::string& s)
{
    out += '"';
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        default:
            if ((unsigned char)c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof(b), "\\u%04x", c);
                out += b;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

struct JsonWriter {
    std::string s;
    bool first = true;
    void Key(const char* k)
    {
        if (!first) s += ',';
        first = false;
        s += '"';
        s += k;
        s += "\":";
    }
    void Str(const char* k, const std::string& v) { Key(k); AppendEscaped(s, v); }
    void Bool(const char* k, bool v) { Key(k); s += v ? "true" : "false"; }
    void Int(const char* k, long long v) { Key(k); s += std::to_string(v); }
    void Num(const char* k, double v, int prec = 1)
    {
        Key(k);
        char b[48];
        std::snprintf(b, sizeof(b), "%.*f", prec, v);
        s += b;
    }
    void Raw(const char* k, const std::string& v) { Key(k); s += v; }
};
} // namespace

std::string ToJson(const MediaStats& m)
{
    JsonWriter v;
    v.s = "{";
    v.Bool("active", m.videoActive);
    v.Str("codec", m.codec);
    v.Str("decoder", m.decoder);
    v.Str("pixelFormat", m.pixelFormat);
    v.Str("colorMatrix", m.colorMatrix);
    v.Raw("codedSize", "[" + std::to_string(m.codedWidth) + "," + std::to_string(m.codedHeight) + "]");
    v.Raw("viewSize", "[" + std::to_string(m.viewWidth) + "," + std::to_string(m.viewHeight) + "]");
    v.Num("inputFps", m.inputFps);
    v.Num("decodeFps", m.decodeFps);
    v.Num("renderFps", m.renderFps);
    v.Num("decodeMsAvg", m.decodeMsAvg, 2);
    v.Num("decodeMsMax", m.decodeMsMax, 2);
    v.Num("decodeInOutMsAvg", m.decodeInOutMsAvg, 2);
    v.Num("decodeInOutMsMax", m.decodeInOutMsMax, 2);
    v.Int("decodeHeldMax", m.decodeHeldMax);
    v.Int("spsRewritten", m.spsRewritten);
    v.Num("latencyMsAvg", m.latencyMsAvg, 1);
    v.Num("latencyMsMax", m.latencyMsMax, 1);
    v.Num("presentMsAvg", m.presentMsAvg, 2);
    v.Int("decodeQueue", m.decodeQueue);
    v.Int("renderQueue", m.renderQueue);
    v.Int("framesIn", (long long)m.framesIn);
    v.Int("framesDecoded", (long long)m.framesDecoded);
    v.Int("framesPresented", (long long)m.framesPresented);
    v.Int("droppedQueue", (long long)m.framesDroppedQueue);
    v.Int("droppedRender", (long long)m.framesDroppedRender);
    v.Int("decodeErrors", (long long)m.decodeErrors);
    v.Str("viewMode", m.viewMode);
    v.Str("resolvedMode", m.resolvedMode);
    v.Num("zoom", m.zoom, 2);
    v.Bool("autoCrop", m.autoCrop);
    {
        char b[128];
        std::snprintf(b, sizeof(b), "[%.4f,%.4f,%.4f,%.4f]", m.crop.x, m.crop.y, m.crop.w, m.crop.h);
        v.Raw("crop", b);
    }
    JsonWriter a;
    a.s = "{";
    a.Bool("active", m.audioActive);
    a.Bool("enabled", m.audioEnabled);
    a.Str("codec", m.audioCodec);
    a.Int("rate", m.audioRate);
    a.Int("channels", m.audioChannels);
    a.Int("outputRate", m.outputRate);
    a.Num("bufferMs", m.audioBufferMs);
    a.Num("targetMs", m.audioTargetMs);
    a.Num("jitterMs", m.audioJitterMs);
    a.Int("slewPpm", m.audioSlewPpm);
    a.Num("arrivalLagMs", m.audioArrivalLagMs);
    a.Num("syncErrorMs", m.audioSyncErrorMs);
    a.Int("offsetMs", m.audioOffsetMs);
    a.Num("volumeDb", m.volumeDb);
    a.Int("packets", (long long)m.audioPackets);
    a.Int("decodeErrors", (long long)m.audioDecodeErrors);
    a.Int("underruns", (long long)m.audioUnderruns);
    a.Int("resyncs", (long long)m.audioResyncs);
    a.Int("dropped", (long long)m.audioDropped);
    a.s += "}";
    v.s += "}";
    // Flat summary first: the UI stats panel (ui/Bridge.cpp) shows only top-level scalars of the "media" section;
    // the nested "video"/"audio" objects carry the full detail for GET /status.
    JsonWriter t;
    t.s = "{";
    t.Num("latencyMs", m.latencyMsAvg, 1);
    t.Num("latencyMsMax", m.latencyMsMax, 1);
    t.Num("decodeFps", m.decodeFps, 1);
    t.Num("renderFps", m.renderFps, 1);
    t.Num("decodeMs", m.decodeMsAvg, 2);
    t.Num("decodeMsMax", m.decodeMsMax, 2);
    t.Num("decodeInOutMs", m.decodeInOutMsAvg, 2);
    t.Int("dropped", (long long)(m.framesDroppedQueue + m.framesDroppedRender));
    t.Str("codec", m.codec.empty() ? std::string("-") : m.codec + " " + m.decoder);
    t.Str("size", std::to_string(m.codedWidth) + "x" + std::to_string(m.codedHeight));
    {
        char b[96];
        std::snprintf(b, sizeof(b), "%.0f%% %.0f%% %.0f%% %.0f%%", m.crop.x * 100, m.crop.y * 100, m.crop.w * 100,
                      m.crop.h * 100);
        t.Str("cropRect", b);
    }
    t.Str("mode", m.viewMode == "Auto" ? "Auto (" + m.resolvedMode + ")" : m.viewMode);
    t.Num("audioBufferMs", m.audioBufferMs, 1);
    t.Int("underruns", (long long)m.audioUnderruns);
    t.Raw("video", v.s);
    t.Raw("audio", a.s);
    t.s += "}";
    return t.s;
}

}} // namespace XboxCast::Media
