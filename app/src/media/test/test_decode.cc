// xbox-cast media harness: end-to-end decode through the same FFmpeg calls the app uses (software path).
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

extern "C" {
#include <libavutil/pixdesc.h>
}

#include "FfAudioDecoder.h"
#include "FfVideoDecoder.h"
#include "H264Sps.h"
#include "MediaClock.h"
#include "fixtures.h"
#include "xtest.h"

using namespace XboxCast::Media;

// Make the stream look like the iPhone's: every SPS loses its VUI bitstream_restriction.
static std::vector<uint8_t> IosLike(const std::vector<uint8_t>& au, int* stripped)
{
    std::vector<uint8_t> out;
    static const uint8_t sc[4] = {0, 0, 0, 1};
    H264::ReorderFix::ForEachNal(au.data(), au.size(), [&](const uint8_t* nal, size_t len) {
        out.insert(out.end(), sc, sc + 4);
        if (len > 1 && (nal[0] & 0x1f) == 7) {
            H264::SpsInfo info = H264::ParseSps(nal, len);
            std::vector<uint8_t> s = H264::StripRestriction(nal, len, info);
            if (!s.empty()) {
                (*stripped)++;
                out.insert(out.end(), s.begin(), s.end());
                return;
            }
        }
        out.insert(out.end(), nal, nal + len);
    });
    return out;
}

struct RunResult {
    int frames = 0;
    int maxHeld = 0;
    double avgInOutMs = 0, maxInOutMs = 0;
    double decodeMsAvg = 0;
    std::vector<int> sizesW, sizesH;
    std::string pixfmt;
};

static RunResult RunVideo(const std::vector<xt::Packet>& pkts, xc_video_codec codec, bool reorderFix, bool lowDelay,
                          int flushAt = -1, int strict = 0)
{
    RunResult r;
    FfVideoDecoder dec;
    VideoDecoderOptions o;
    o.codec = codec;
    o.reorderFix = reorderFix;
    o.lowDelayFlag = lowDelay;
    o.strictCompliance = strict;
    o.swThreads = 1;
    CHECK(dec.Open(o));
    double sum = 0, dsum = 0;
    int idx = 0;
    for (auto& p : pkts) {
        if (idx++ == flushAt) dec.Flush();
        int64_t t0 = NowNs();
        dec.Decode(p.data.data(), p.data.size(), p.ptsUs * 1000, p.key, [&](AVFrame* f, const DecodeTiming& t) {
            r.frames++;
            double ms = NsToMs(t.outNs - t.inNs);
            sum += ms;
            r.maxInOutMs = (std::max)(r.maxInOutMs, ms);
            r.maxHeld = (std::max)(r.maxHeld, t.packetsHeld);
            if (r.sizesW.empty() || r.sizesW.back() != f->width || r.sizesH.back() != f->height) {
                r.sizesW.push_back(f->width);
                r.sizesH.push_back(f->height);
            }
            r.pixfmt = av_get_pix_fmt_name((AVPixelFormat)f->format);
        });
        dsum += NsToMs(NowNs() - t0);
    }
    if (r.frames) r.avgInOutMs = sum / r.frames;
    r.decodeMsAvg = pkts.empty() ? 0 : dsum / pkts.size();
    return r;
}

TEST(sps_rewrite_roundtrip)
{
    xt::Stream s = xt::ReadStream(xt::Fixture("land.h264"), AVMEDIA_TYPE_VIDEO);
    int stripped = 0;
    std::vector<uint8_t> ios = IosLike(s.packets[0].data, &stripped);
    CHECK(stripped == 1);
    bool sawSps = false;
    H264::ReorderFix::ForEachNal(ios.data(), ios.size(), [&](const uint8_t* nal, size_t len) {
        if ((nal[0] & 0x1f) != 7) return;
        sawSps = true;
        H264::SpsInfo a = H264::ParseSps(nal, len);
        INFO("iOS-like SPS: %s", a.text.c_str());
        CHECK(a.ok && a.hasVui && !a.hasRestriction);
        std::vector<uint8_t> w = H264::RewriteSps(nal, len, a, 0);
        H264::SpsInfo b = H264::ParseSps(w.data(), w.size());
        INFO("rewritten:    %s", b.text.c_str());
        CHECK(b.ok && b.hasRestriction && b.reorderFrames == 0 && b.dpbFrames >= 1);
        CHECK(b.refFrames == a.refFrames);
        // no VUI at all (the case the screen-mirror-xbox port saw)
        std::vector<uint8_t> nv(nal, nal + len);
    });
    CHECK(sawSps);
    H264::ReorderFix fix;
    std::vector<uint8_t> out;
    CHECK(fix.Process(ios.data(), ios.size(), out));
    CHECK(fix.SpsRewritten() == 1);
    // second keyframe with the same SPS hits the cache
    CHECK(fix.Process(ios.data(), ios.size(), out));
    CHECK(fix.SpsRewritten() == 1);
    // P-slice access unit: untouched
    CHECK(!fix.Process(s.packets[1].data.data(), s.packets[1].data.size(), out));
}

// The core latency claim: with the iPhone's SPS, does each frame come out in the same Decode() call?
TEST(decode_h264_one_in_one_out)
{
    xt::Stream s = xt::ReadStream(xt::Fixture("land.h264"), AVMEDIA_TYPE_VIDEO);
    std::vector<xt::Packet> ios = s.packets;
    int stripped = 0;
    for (auto& p : ios) p.data = IosLike(p.data, &stripped);
    CHECK(stripped >= 1);

    RunResult plain = RunVideo(ios, XC_VIDEO_H264, false, false);
    RunResult lowd = RunVideo(ios, XC_VIDEO_H264, false, true);
    RunResult fixd = RunVideo(ios, XC_VIDEO_H264, true, false);
    RunResult both = RunVideo(ios, XC_VIDEO_H264, true, true);
    INFO("iOS-like SPS, %zu packets (1920x886 60 fps, sw decode, 1 thread):", ios.size());
    INFO("  no fixes        : %d frames, max held %d pkts, in->out avg %.2f ms max %.2f ms", plain.frames, plain.maxHeld,
         plain.avgInOutMs, plain.maxInOutMs);
    INFO("  LOW_DELAY only  : %d frames, max held %d pkts, in->out avg %.2f ms max %.2f ms", lowd.frames, lowd.maxHeld,
         lowd.avgInOutMs, lowd.maxInOutMs);
    INFO("  SPS rewrite only: %d frames, max held %d pkts, in->out avg %.2f ms max %.2f ms", fixd.frames, fixd.maxHeld,
         fixd.avgInOutMs, fixd.maxInOutMs);
    INFO("  both (app)      : %d frames, max held %d pkts, in->out avg %.2f ms max %.2f ms, decode %.2f ms/frame",
         both.frames, both.maxHeld, both.avgInOutMs, both.maxInOutMs, both.decodeMsAvg);
    CHECK(both.maxHeld == 0);
    CHECK(both.frames == (int)ios.size());
    CHECK(fixd.maxHeld == 0);
    CHECK(lowd.maxHeld == 0);
    // A strictly conforming decoder (FFmpeg with -strict 1) is what the platform decoders behave like: without the
    // SPS rewrite it must assume the full DPB of reordering. This reproduces the 130-700 ms of screen-mirror-xbox.
    RunResult strictNo = RunVideo(ios, XC_VIDEO_H264, false, false, -1, 1 /*FF_COMPLIANCE_STRICT*/);
    RunResult strictFix = RunVideo(ios, XC_VIDEO_H264, true, false, -1, 1);
    INFO("  strict decoder, no rewrite : %d frames out, max held %d pkts (%.0f ms at 60 fps), in->out max %.2f ms",
         strictNo.frames, strictNo.maxHeld, strictNo.maxHeld * 16.7, strictNo.maxInOutMs);
    INFO("  strict decoder, SPS rewrite: %d frames out, max held %d pkts", strictFix.frames, strictFix.maxHeld);
    CHECK(strictFix.maxHeld == 0);
    INFO("  -> FFmpeg's default h264 decoder already outputs 1-in-1-out for this stream (it guesses has_b_frames from");
    INFO("     the stream); the rewrite protects against strict decoders / different FFmpeg heuristics.");
}

TEST(decode_h264_rotation_and_flush)
{
    xt::Stream s = xt::ReadStream(xt::Fixture("rotate.h264"), AVMEDIA_TYPE_VIDEO);
    // pts: monotonic across the concatenation; SPS as the iPhone sends it
    int stripped = 0;
    for (size_t i = 0; i < s.packets.size(); i++) {
        s.packets[i].ptsUs = (int64_t)i * 16667;
        s.packets[i].data = IosLike(s.packets[i].data, &stripped);
    }
    CHECK(stripped >= 3);
    RunResult r = RunVideo(s.packets, XC_VIDEO_H264, true, true);
    std::string seq;
    for (size_t i = 0; i < r.sizesW.size(); i++) seq += std::to_string(r.sizesW[i]) + "x" + std::to_string(r.sizesH[i]) + " ";
    INFO("rotation: %d frames of %zu packets, geometry sequence: %s", r.frames, s.packets.size(), seq.c_str());
    CHECK(r.frames == (int)s.packets.size());
    CHECK(r.sizesW.size() == 3);
    CHECK(r.maxHeld == 0);
    if (r.sizesW.size() == 3) {
        CHECK(r.sizesW[0] == 1920 && r.sizesH[0] == 886);
        CHECK(r.sizesW[1] == 496 && r.sizesH[1] == 1080);
        CHECK(r.sizesW[2] == 1920);
    }
    // OnFlush in the middle of a GOP, then the stream resumes at the next keyframe (what the pipeline does: drop
    // packets until a keyframe after a flush)
    std::vector<xt::Packet> cut;
    bool waiting = false;
    for (size_t i = 0; i < s.packets.size(); i++) {
        if (i == 20) waiting = true;
        if (waiting && !s.packets[i].key) continue;
        waiting = false;
        cut.push_back(s.packets[i]);
    }
    RunResult c = RunVideo(cut, XC_VIDEO_H264, true, true, 20);
    INFO("flush at packet 20 + resume at next IDR: %d frames out of %zu packets", c.frames, cut.size());
    CHECK(c.frames == (int)cut.size());
}

TEST(decode_hevc_main_and_main10)
{
    xt::Stream m = xt::ReadStream(xt::Fixture("main.hevc"), AVMEDIA_TYPE_VIDEO);
    RunResult a = RunVideo(m.packets, XC_VIDEO_H265, true, true);
    INFO("hevc main: %d/%zu frames, %s, max held %d", a.frames, m.packets.size(), a.pixfmt.c_str(), a.maxHeld);
    CHECK(a.frames == (int)m.packets.size());
    CHECK(a.pixfmt == "yuv420p");
    CHECK(a.maxHeld == 0);
    xt::Stream t = xt::ReadStream(xt::Fixture("main10.hevc"), AVMEDIA_TYPE_VIDEO);
    RunResult b = RunVideo(t.packets, XC_VIDEO_H265, true, true);
    INFO("hevc main10: %d/%zu frames, %s, max held %d", b.frames, t.packets.size(), b.pixfmt.c_str(), b.maxHeld);
    CHECK(b.frames == (int)t.packets.size());
    CHECK(b.pixfmt == "yuv420p10le");
}

// Audio: decode access units with the same FfAudioDecoder + swresample to 48 kHz float stereo.
static void RunAudio(const char* fixture, xc_audio_codec codec, bool passConfig, double expectHz)
{
    xt::Stream s = xt::ReadStream(xt::Fixture(fixture), AVMEDIA_TYPE_AUDIO);
    FfAudioDecoder dec;
    const uint8_t* cfg = passConfig && !s.extradata.empty() ? s.extradata.data() : nullptr;
    size_t cfgN = passConfig ? s.extradata.size() : 0;
    CHECK(dec.Open(codec, s.sampleRate, s.channels, cfg, cfgN, 48000, 2));
    long long frames = 0;
    int errors = 0;
    std::vector<float> all;
    int64_t firstPts = INT64_MIN, lastPts = 0;
    for (auto& p : s.packets) {
        int n = dec.Decode(p.data.data(), p.data.size(), p.ptsUs * 1000, [&](const float* pcm, int f, int64_t pts) {
            frames += f;
            all.insert(all.end(), pcm, pcm + (size_t)f * 2);
            if (firstPts == INT64_MIN) firstPts = pts;
            lastPts = pts;
        });
        if (n < 0) errors++;
    }
    // frequency by zero crossings of the left channel with hysteresis (codec noise near zero would double-count);
    // skip the first 50 ms: codec priming
    int zc = 0;
    size_t start = 2400;
    const size_t end = (std::min)(all.size() / 2, start + 38400); // 0.8 s window: excludes the encoder's end padding
    double peak = 0;
    for (size_t i = start; i < end; i++) peak = (std::max)(peak, (double)std::fabs(all[i * 2]));
    const float hyst = (float)(peak * 0.25);
    int state = 0;
    for (size_t i = start; i < end; i++) {
        float v = all[i * 2];
        int ns = v > hyst ? 1 : v < -hyst ? -1 : state;
        if (state != 0 && ns != state) zc++;
        state = ns;
    }
    double secs = end > start ? (double)(end - start) / 48000.0 : 0;
    double hz = secs > 0 ? zc / 2.0 / secs : 0;
    INFO("%s (%s, %d Hz %d ch, config %zu B): %zu packets -> %lld frames @48k (%.3f s), errors %d, tone %.1f Hz, peak %.2f",
         fixture, FfAudioDecoder::CodecName(codec), s.sampleRate, s.channels, s.extradata.size(), s.packets.size(), frames,
         frames / 48000.0, errors, hz, peak);
    CHECK(errors == 0);
    CHECK(frames > 0);
    if (expectHz > 0) CHECK_NEAR(hz, expectHz, expectHz * 0.02);
    (void)lastPts;
}

TEST(decode_audio_aac_lc)
{
    RunAudio("aaclc.m4a", XC_AUDIO_AAC_LC, true, 440);
}

TEST(decode_audio_aac_lc_default_config)
{
    // sender passed no AudioSpecificConfig: the decoder builds 44.1k stereo LC itself
    RunAudio("aaclc.m4a", XC_AUDIO_AAC_LC, false, 440);
    std::vector<uint8_t> asc = FfAudioDecoder::DefaultAsc(2, 44100, 2, 1024);
    CHECK(asc.size() == 2 && asc[0] == 0x12 && asc[1] == 0x10);
}

TEST(decode_audio_alac_cookie)
{
    RunAudio("alac.m4a", XC_AUDIO_ALAC, true, 440);
}

TEST(decode_audio_aac_eld_conformance)
{
    // ER AAC-ELD from FFmpeg's FATE suite (no GPL-compatible ELD encoder exists for the harness to make one).
    // This file is 512-sample ELD mono; iOS mirroring sends 480-sample ELD stereo (spf 480).
    RunAudio("eld.mp4", XC_AUDIO_AAC_ELD, true, 0);
    xt::Stream s = xt::ReadStream(xt::Fixture("eld.mp4"), AVMEDIA_TYPE_AUDIO);
    auto hex = [](const std::vector<uint8_t>& v) {
        std::string a;
        for (uint8_t x : v) { char t[4]; std::snprintf(t, sizeof t, "%02x", x); a += t; }
        return a;
    };
    std::vector<uint8_t> ours = FfAudioDecoder::DefaultAsc(39, s.sampleRate, s.channels, 512);
    INFO("ELD ASC from file %s, DefaultAsc(39, %d, %d, 512) %s", hex(s.extradata).c_str(), s.sampleRate, s.channels,
         hex(ours).c_str());
    CHECK(hex(s.extradata) == hex(ours));
    // the iOS mirroring config (UxPlay hands GStreamer exactly this codec_data): 44.1 kHz stereo 480 -> f8e85000
    std::string ios = hex(FfAudioDecoder::DefaultAsc(39, 44100, 2, 480));
    INFO("iOS ELD ASC (44.1k stereo 480): %s", ios.c_str());
    CHECK(ios == "f8e85000");
    // our self-built config decodes the stream as well as the file's own
    FfAudioDecoder dec;
    CHECK(dec.Open(XC_AUDIO_AAC_ELD, s.sampleRate, s.channels, ours.data(), ours.size(), 48000, 2));
    int ok = 0, err = 0;
    for (auto& p : s.packets) {
        int n = dec.Decode(p.data.data(), p.data.size(), p.ptsUs * 1000, [](const float*, int, int64_t) {});
        if (n < 0) err++; else ok++;
    }
    INFO("ELD with our default ASC: %d ok, %d errors", ok, err);
    CHECK(err == 0);
    // and the 480 stereo config opens the decoder (frame_size 480)
    FfAudioDecoder d480;
    std::vector<uint8_t> c480 = FfAudioDecoder::DefaultAsc(39, 44100, 2, 480);
    CHECK(d480.Open(XC_AUDIO_AAC_ELD, 44100, 2, c480.data(), c480.size(), 48000, 2));
}

TEST(decode_audio_pcm)
{
    FfAudioDecoder dec;
    CHECK(dec.Open(XC_AUDIO_PCM, 44100, 2, nullptr, 0, 48000, 2));
    std::vector<int16_t> pcm(441 * 2);
    for (int i = 0; i < 441; i++) pcm[i * 2] = pcm[i * 2 + 1] = (int16_t)(16000 * std::sin(i * 2 * M_PI * 441 / 44100.0));
    long long out = 0;
    for (int k = 0; k < 100; k++)
        dec.Decode((const uint8_t*)pcm.data(), pcm.size() * 2, k * 10000000LL, [&](const float*, int f, int64_t) { out += f; });
    INFO("pcm: 100 x 441 frames @44.1k -> %lld frames @48k", out);
    CHECK(std::llabs(out - 48000) < 64);
}
