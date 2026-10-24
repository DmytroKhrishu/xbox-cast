// xbox-cast media harness: audio scheduler replay of the first console session (iPhone 16 Pro, AAC-ELD 44.1 kHz,
// 480 samples per packet, 48 kHz output). Runs the same packet trace through the scheduler as it was before the
// adaptive/slew change (LegacyScheduler, copied verbatim) and through the current AudioScheduler.
//
// Trace model, from the console log (docs/media.md "Audio"):
//   * pts_ns is the ARRIVAL time: UxPlay never sets client_time_received, so raop_ntp_convert_remote_time() returns 0
//     and castcore stamps pts = xc_now_ns() (the log shows arrival lag 0.0-0.1 ms). A Wi-Fi stall therefore delivers a
//     burst of packets that all carry about the same pts;
//   * stalls (UxPlay holds everything behind a lost packet until the resend): every ~1.5 s, 20-120 ms during the
//     first 45 s, then every ~6 s, 20-70 ms; plus 0-6 ms ordinary jitter;
//   * sender clock +100 ppm against the Xbox crystal (packets come 100 ppm faster than we play them), then -100 ppm
//     after the pause;
//   * a 20 s pause (iPhone stops sending audio) at 60 s;
//   * renderer video latency alternating 2.7 / 10.3 ms every 20 s.
// Audible events counted: underruns (output ran dry mid-stream), hard corrections (silence inserted / audio skipped
// or dropped while playing). Lip sync: (time a sample is heard) - (time the video frame sent at the same instant is
// shown: send + 6 ms network + renderer latency), i.e. how far audio trails the picture.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "AudioScheduler.h"
#include "FfAudioDecoder.h"
#include "xtest.h"

using namespace XboxCast::Media;

namespace {

// The scheduler before this change: fixed 50 ms jitter target, any |error| > 20 ms -> silence / skip / drop.
class LegacyScheduler {
public:
    void SetVideoLatencyNs(int64_t ns)
    {
        if (ns <= 0) { m_video = 0; return; }
        if (m_video == 0 || std::llabs(ns - m_video) > 8000000) m_video = ns;
    }
    void NoteArrival(int64_t pts, int64_t arrival) { m_arrival.Push(arrival, arrival - pts); }
    int64_t TargetDelayNs() const
    {
        const int64_t lag = m_arrival.Empty() ? 0 : m_arrival.Max();
        int64_t d = lag + 50000000;
        if (m_video > 0) d = (std::max)(d, m_video);
        return (std::max)(d, lag + 15000000);
    }
    AudioPlan Plan(int64_t pts, int frames, int64_t cursor, bool starved)
    {
        AudioPlan p;
        const int64_t err = pts + TargetDelayNs() - cursor;
        if (!starved && std::llabs(err) <= 20000000) return p;
        p.resync = true;
        p.fadeIn = true;
        if (err > 0) {
            p.silenceFrames = (int)((std::min)(err, (int64_t)250000000) * 48000 / 1000000000LL);
        } else if (err < 0) {
            int64_t skip = -err * 48000 / 1000000000LL;
            p.drop = skip >= frames;
            p.skipFrames = p.drop ? frames : (int)skip;
        }
        return p;
    }

private:
    WindowedMax m_arrival;
    int64_t m_video = 0;
};

struct Rng { // deterministic on every platform
    uint64_t s;
    double U() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return (double)(s >> 11) / 9007199254740992.0; }
};

struct SimResult {
    int underruns = 0, underrunsLate = 0, hard = 0, hardLate = 0, dropped = 0;
    double avMean = 0, avP1 = 0, avP99 = 0; // ms, after warm-up (45 s)
    double finalTargetMs = 0;
    int maxSlewPpm = 0;
};

const int64_t kMs = 1000000;

struct Packet { int64_t send, arrival; };

std::vector<Packet> MakeTrace()
{
    Rng r{42};
    std::vector<Packet> out;
    const double pktSenderNs = 480.0 * 1e9 / 44100.0;
    double t = 1e9; // send time on our clock
    int64_t nextStall = (int64_t)(1.5e9), stallEnd = 0;
    int64_t lastArr = 0;
    const int64_t pauseFrom = 60000 * kMs, pauseTo = 80000 * kMs, end = 200000 * kMs;
    while ((int64_t)t < end) {
        const int64_t send = (int64_t)t;
        const double ppm = send < pauseTo ? 100.0 : -100.0;
        t += pktSenderNs / (1.0 + ppm * 1e-6); // sender clock fast -> packets closer together on ours
        if (send >= pauseFrom && send < pauseTo) continue;
        const bool early = send < 45000 * kMs;
        if (send >= nextStall) {
            const double len = early ? 20 + r.U() * 100 : 20 + r.U() * 50;
            stallEnd = send + (int64_t)(len * kMs);
            nextStall = send + (int64_t)((early ? 0.7 + r.U() * 1.6 : 3 + r.U() * 6) * 1e9);
        }
        int64_t arr = send + 3 * kMs + (int64_t)(r.U() * 6 * kMs);
        if (send < stallEnd) arr = (std::max)(arr, stallEnd + 2 * kMs); // held until the resend
        arr = (std::max)(arr, lastArr);                                  // UxPlay delivers in order
        lastArr = arr;
        out.push_back(Packet{send, arr});
    }
    return out;
}

template <class S>
SimResult Run(S& s, const std::vector<Packet>& trace, bool slew)
{
    SimResult res;
    const double framesPerPkt = 480.0 * 48000.0 / 44100.0;
    double fracFrames = 0;
    int ppm = 0;
    int64_t queuedEnd = 0; // when the last queued sample has been heard
    bool playing = false;
    std::vector<double> av;
    for (const Packet& pk : trace) {
        const int64_t now = pk.arrival, pts = pk.arrival; // castcore: pts = xc_now_ns() at delivery
        const int64_t videoLat = ((now / (20000 * kMs)) % 2) ? 10300000 : 2700000;
        s.SetVideoLatencyNs(videoLat);
        s.NoteArrival(pts, now);
        fracFrames += framesPerPkt * (1.0 + ppm * 1e-6);
        const int frames = (int)fracFrames;
        fracFrames -= frames;
        const bool starved = queuedEnd <= now;
        const bool late = now > 45000 * kMs;
        if (starved && playing) {
            res.underruns++;
            if (late) res.underrunsLate++;
            playing = false;
        }
        const int64_t cursor = starved ? now : queuedEnd;
        AudioPlan p = s.Plan(pts, frames, cursor, starved);
        if (p.resync && !starved) {
            res.hard++;
            if (late) res.hardLate++;
        }
        if (slew) ppm = p.slewPpm;
        res.maxSlewPpm = (std::max)(res.maxSlewPpm, std::abs(p.slewPpm));
        if (p.drop) {
            res.dropped++;
            continue;
        }
        const int64_t start = cursor + (int64_t)p.silenceFrames * 1000000000LL / 48000;
        const int64_t heard0 = start - (int64_t)p.skipFrames * 1000000000LL / 48000;
        queuedEnd = start + (int64_t)(frames - p.skipFrames) * 1000000000LL / 48000;
        playing = true;
        // the video frame sent at the same instant arrives over its own stream (typical 6 ms) and is shown videoLat
        // later: A/V offset = audio heard - picture shown, > 0 = audio behind the picture
        if (late) av.push_back((heard0 - (pk.send + 6 * kMs + videoLat)) / 1e6);
    }
    if (!av.empty()) {
        std::vector<double> v = av;
        std::sort(v.begin(), v.end());
        double sum = 0;
        for (double x : v) sum += x;
        res.avMean = sum / v.size();
        res.avP1 = v[v.size() / 100];
        res.avP99 = v[v.size() * 99 / 100];
    }
    res.finalTargetMs = s.TargetDelayNs() / 1e6;
    return res;
}

void Print(const char* name, const SimResult& r)
{
    INFO("%-6s underruns %2d (after 45 s: %d), hard corrections %3d (after 45 s: %2d), chunks dropped %3d, "
         "max slew %4d ppm, final target %.0f ms, A/V after 45 s (audio behind video): mean %.0f, p1 %.0f, p99 %.0f ms",
         name, r.underruns, r.underrunsLate, r.hard, r.hardLate, r.dropped, r.maxSlewPpm, r.finalTargetMs, r.avMean,
         r.avP1, r.avP99);
}

} // namespace

TEST(audio_scheduler_console_replay)
{
    const std::vector<Packet> trace = MakeTrace();
    LegacyScheduler oldS;
    SimResult a = Run(oldS, trace, false);
    AudioScheduler newS;
    SimResult b = Run(newS, trace, true);
    INFO("trace: %zu packets over 200 s", trace.size());
    Print("before", a);
    Print("after", b);
    CHECK(b.hard <= 5);                    // no resync storm
    CHECK(b.underruns * 2 <= a.underruns); // adaptive target absorbs most stalls
    CHECK(b.underrunsLate <= 3);           // and settles
    CHECK(b.avP99 <= 150);                 // audio never far behind the picture
    CHECK(b.avP1 >= 0);                    // and never ahead of it
}

// The slew really changes the output length, in the direction the scheduler expects (> 0 = more output = later).
TEST(audio_decoder_compensation)
{
    for (int ppm : {0, 2000, -2000}) {
        FfAudioDecoder d;
        CHECK(d.Open(XC_AUDIO_PCM, 44100, 2, nullptr, 0, 48000, 2));
        std::vector<int16_t> pcm(441 * 2, 0);
        int64_t out = 0;
        for (int i = 0; i < 500; i++) { // 5 s
            d.SetCompensationPpm(ppm);
            out += d.Decode((const uint8_t*)pcm.data(), pcm.size() * 2, 0, [](const float*, int, int64_t) {});
        }
        const double got = (double)out / (48000.0 * 5) - 1.0;
        INFO("compensation %5d ppm -> output %+.0f ppm", ppm, got * 1e6);
        CHECK(std::fabs(got * 1e6 - ppm) < 300); // filter delay is a few dozen frames of the 240000
    }
}
