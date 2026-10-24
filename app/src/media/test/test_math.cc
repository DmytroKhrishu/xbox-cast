// xbox-cast media harness: view transform, color matrix, audio math, scheduler, queues.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <cstdlib>
#include <thread>

#include "AudioScheduler.h"
#include "ColorMatrix.h"
#include "MediaStats.h"
#include "Queues.h"
#include "ViewTransform.h"
#include "xtest.h"

using namespace XboxCast::Media;

static ViewParams VP(int fw, int fh, ViewMode m, RectF c = RectF::Unit())
{
    ViewParams p;
    p.frameW = fw;
    p.frameH = fh;
    p.viewW = 1920;
    p.viewH = 1080;
    p.mode = m;
    p.content = c;
    return p;
}

static void CheckRect(const RectF& r, float x, float y, float w, float h, float eps)
{
    CHECK_NEAR(r.x, x, eps);
    CHECK_NEAR(r.y, y, eps);
    CHECK_NEAR(r.w, w, eps);
    CHECK_NEAR(r.h, h, eps);
}

TEST(transform_landscape_phone_fit_fill_auto)
{
    // iPhone landscape mirror frame 1920x886 on a 1920x1080 view
    Layout fit = ComputeLayout(VP(1920, 886, ViewMode::Fit));
    CheckRect(fit.placement, 0, 97, 1920, 886, 0.01f);
    DrawRects d = ComputeDrawRects(fit, 1920, 1080);
    CheckRect(d.dst, 0, 97, 1920, 886, 0.01f);
    CheckRect(d.src, 0, 0, 1, 1, 1e-5f);

    Layout fill = ComputeLayout(VP(1920, 886, ViewMode::Fill));
    const float s = 1080.0f / 886.0f;
    CHECK_NEAR(fill.scale, s, 1e-4);
    d = ComputeDrawRects(fill, 1920, 1080);
    CheckRect(d.dst, 0, 0, 1920, 1080, 0.01f);
    float visible = 1920.0f / (1920.0f * s);
    CheckRect(d.src, (1 - visible) / 2, 0, visible, 1, 1e-4f);

    // 2.167 vs 1.778: Fill would hide 18% -> Auto = Fit
    CHECK_NEAR(FillLoss(1920, 886, 1920, 1080), 0.1795, 1e-3);
    Layout aut = ComputeLayout(VP(1920, 886, ViewMode::Auto));
    CHECK(aut.resolvedMode == ViewMode::Fit);
}

TEST(transform_portrait_phone)
{
    // portrait phone arrives ~496x1080 (width follows the phone)
    Layout fit = ComputeLayout(VP(496, 1080, ViewMode::Fit));
    CheckRect(fit.placement, 712, 0, 496, 1080, 0.01f);
    Layout aut = ComputeLayout(VP(496, 1080, ViewMode::Auto));
    CHECK(aut.resolvedMode == ViewMode::Fit);
    Layout fill = ComputeLayout(VP(496, 1080, ViewMode::Fill));
    DrawRects d = ComputeDrawRects(fill, 1920, 1080);
    CheckRect(d.dst, 0, 0, 1920, 1080, 0.01f);
    CHECK_NEAR(d.src.w, 1.0, 1e-4);
    CHECK_NEAR(d.src.h, (1080.0 / (1920.0 / 496.0)) / 1080.0, 1e-4); // only a band of the phone screen
}

TEST(transform_auto_crop_killer_cases)
{
    // 16:9 video inside the landscape phone frame: 1576x886 at x=172 of 1920x886
    RectF c = RectF::Make(172.0f / 1920, 0, 1576.0f / 1920, 1);
    Layout a = ComputeLayout(VP(1920, 886, ViewMode::Auto, c));
    CHECK(a.resolvedMode == ViewMode::Fill);
    DrawRects d = ComputeDrawRects(a, 1920, 1080);
    CheckRect(d.dst, 0, 0, 1920, 1080, 0.5f);
    CHECK(d.src.x >= c.x - 1e-4f);
    CHECK(d.src.Right() <= c.Right() + 1e-4f);

    // 16:9 video inside a PORTRAIT phone: 496x280 band at y=400 of 496x1080 -> fills the TV
    RectF pc = RectF::Make(0, 400.0f / 1080, 1, 280.0f / 1080);
    Layout p = ComputeLayout(VP(496, 1080, ViewMode::Auto, pc));
    CHECK(p.resolvedMode == ViewMode::Fill);
    d = ComputeDrawRects(p, 1920, 1080);
    CheckRect(d.dst, 0, 0, 1920, 1080, 0.5f);
    CHECK(d.src.y >= pc.y - 1e-4f);
    CHECK(d.src.Bottom() <= pc.Bottom() + 1e-4f);
    INFO("portrait-phone 16:9 video: scale %.3f (was %.3f in Fit without crop)", p.scale, 1.0f);

    // 2.39:1 film letterboxed in 16:9: cropping bars then Fill would hide 25% -> Fit (bars come back, no loss)
    RectF sc = RectF::Make(0, 138.0f / 1080, 1, 804.0f / 1080);
    Layout s = ComputeLayout(VP(1920, 1080, ViewMode::Auto, sc));
    CHECK(s.resolvedMode == ViewMode::Fit);
    d = ComputeDrawRects(s, 1920, 1080);
    CheckRect(d.dst, 0, 138, 1920, 804, 0.5f);

    // 4:3 inside 16:9 pillarbox -> loss 25% -> Fit
    RectF fc = RectF::Make(240.0f / 1920, 0, 1440.0f / 1920, 1);
    CHECK(ComputeLayout(VP(1920, 1080, ViewMode::Auto, fc)).resolvedMode == ViewMode::Fit);
    // 1.85:1 inside 16:9 -> loss 4% -> Fill
    RectF wc = RectF::Make(0, 21.0f / 1080, 1, 1038.0f / 1080);
    CHECK(ComputeLayout(VP(1920, 1080, ViewMode::Auto, wc)).resolvedMode == ViewMode::Fill);
}

TEST(transform_zoom_and_pan)
{
    ViewParams p = VP(1920, 1080, ViewMode::Zoom);
    p.zoom = 2.0f;
    Layout l = ComputeLayout(p);
    CHECK_NEAR(l.scale, 2.0, 1e-5);
    DrawRects d = ComputeDrawRects(l, 1920, 1080);
    CheckRect(d.src, 0.25f, 0.25f, 0.5f, 0.5f, 1e-5f);
    // pan to the top-left corner: clamped so no black shows
    p.panX = 0.0f;
    p.panY = 0.0f;
    d = ComputeDrawRects(ComputeLayout(p), 1920, 1080);
    CheckRect(d.src, 0, 0, 0.5f, 0.5f, 1e-5f);
    CheckRect(d.dst, 0, 0, 1920, 1080, 0.01f);
    // zoom clamps to 1..3
    p.zoom = 9.0f;
    CHECK_NEAR(ComputeLayout(p).scale, 3.0, 1e-5);
    p.zoom = 0.2f;
    CHECK_NEAR(ComputeLayout(p).scale, 1.0, 1e-5);
    // zoom on a portrait phone: x stays centered while the content is narrower than the view
    ViewParams q = VP(496, 1080, ViewMode::Zoom);
    q.zoom = 1.5f;
    q.panX = 0.0f;
    Layout ql = ComputeLayout(q);
    CHECK_NEAR(ql.placement.x + ql.placement.w / 2, 960, 0.01);
}

TEST(transform_animation_never_pops)
{
    LayoutAnimator an(300000000);
    Layout fit = ComputeLayout(VP(1920, 886, ViewMode::Fit));
    Layout fill = ComputeLayout(VP(1920, 886, ViewMode::Fill));
    an.SetTarget(fit, 0, true);
    CheckRect(an.Current(0).placement, 0, 97, 1920, 886, 0.01f);
    an.SetTarget(fill, 1000, false);
    CheckRect(an.Current(1000).placement, 0, 97, 1920, 886, 0.01f); // starts where it was
    Layout mid = an.Current(1000 + 150000000);
    CHECK(mid.placement.w > fit.placement.w && mid.placement.w < fill.placement.w);
    CHECK(mid.placement.w > (fit.placement.w + fill.placement.w) / 2); // ease-out: past the midpoint at t=0.5
    // retarget mid-flight: continuous
    Layout before = an.Current(1000 + 200000000);
    an.SetTarget(fit, 1000 + 200000000, false);
    Layout after = an.Current(1000 + 200000000);
    CHECK(NearlyEqual(before.placement, after.placement, 0.01f));
    CHECK(an.Animating(1000 + 300000000));
    CheckRect(an.Current(1000 + 200000000 + 300000000).placement, 0, 97, 1920, 886, 0.01f);
    // max per-frame step at 60 Hz stays small (no visible jump)
    an.SetTarget(fill, 0, true);
    an.SetTarget(fit, 0, false);
    float maxStep = 0;
    Layout prev = an.Current(0);
    for (int64_t t = 16666667; t <= 300000000; t += 16666667) {
        Layout c = an.Current(t);
        maxStep = (std::max)(maxStep, std::fabs(c.placement.w - prev.placement.w));
        prev = c;
    }
    INFO("largest 60 Hz step of a Fill->Fit transition: %.1f px of %.1f", maxStep, fill.placement.w - fit.placement.w);
    CHECK(maxStep < 0.2f * (fill.placement.w - fit.placement.w));
}

TEST(color_matrix_bt709_limited)
{
    // limited-range 8-bit: Y=16 -> black, Y=235 -> white, BT.709 100% red (Y 63, Cb 102, Cr 240), 75% red (51,109,212)
    CscConstants c = ComputeCsc(YuvMatrix::Bt709, false, 8, SampleStorage::Unorm8);
    auto conv = [&](int y, int cb, int cr, float* rgb) {
        float s[3] = {y / 255.0f, cb / 255.0f, cr / 255.0f};
        for (int i = 0; i < 3; i++) {
            rgb[i] = 0;
            for (int j = 0; j < 3; j++) rgb[i] += c.m[i * 3 + j] * (s[j] - c.off[j]);
        }
    };
    float rgb[3];
    conv(16, 128, 128, rgb);
    CHECK_NEAR(rgb[0], 0, 1e-3); CHECK_NEAR(rgb[1], 0, 1e-3); CHECK_NEAR(rgb[2], 0, 1e-3);
    conv(235, 128, 128, rgb);
    CHECK_NEAR(rgb[0], 1, 1e-3); CHECK_NEAR(rgb[1], 1, 1e-3); CHECK_NEAR(rgb[2], 1, 1e-3);
    conv(63, 102, 240, rgb);
    CHECK_NEAR(rgb[0], 1.0, 0.01); CHECK_NEAR(rgb[1], 0, 0.01); CHECK_NEAR(rgb[2], 0, 0.01);
    conv(51, 109, 212, rgb);
    CHECK_NEAR(rgb[0], 0.75, 0.01); CHECK_NEAR(rgb[1], 0, 0.01); CHECK_NEAR(rgb[2], 0, 0.01);
    // P010 (10-bit MSB-aligned in R16_UNORM): Y=64<<6 black, 940<<6 white
    CscConstants p = ComputeCsc(YuvMatrix::Bt709, false, 10, SampleStorage::Unorm16Msb);
    float sw = (940 << 6) / 65535.0f, sc = (512 << 6) / 65535.0f;
    float r = p.m[0] * (sw - p.off[0]) + p.m[1] * (sc - p.off[1]) + p.m[2] * (sc - p.off[2]);
    CHECK_NEAR(r, 1.0, 2e-3);
    CHECK(MatrixFromAvColorspace(1, 1080) == YuvMatrix::Bt709);
    CHECK(MatrixFromAvColorspace(2, 480) == YuvMatrix::Bt601);
    CHECK(MatrixFromAvColorspace(2, 886) == YuvMatrix::Bt709);
}

TEST(audio_volume_db)
{
    CHECK_NEAR(DbToGain(-144), 0, 0);
    CHECK_NEAR(DbToGain(0), 1, 1e-6);
    CHECK_NEAR(DbToGain(-6.0206f), 0.5, 1e-4);
    CHECK_NEAR(DbToGain(-30), 0.031623, 1e-5);
    CHECK_NEAR(DbToGain(-50), 0.031623, 1e-5); // clamped to the AirPlay range
    CHECK_NEAR(DbToGain(3), 1, 1e-6);
    float buf[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    ApplyFadeIn(buf, 4, 2, 4);
    CHECK(buf[0] > 0 && buf[0] < buf[2] && buf[6] < 1.0f);
}

// Simulates the output device: 48 kHz, packets of 480 frames (10 ms at 48k after resampling 441 from 44.1k),
// arriving with Wi-Fi jitter. Checks the scheduler converges, then stays without corrections.
TEST(audio_scheduler_jitter_and_sync)
{
    AudioSchedulerConfig cfg;
    cfg.sampleRate = 48000;
    cfg.jitterTargetMs = 50;
    AudioScheduler sch(cfg);
    std::srand(7);
    const int frames = 480;
    const int64_t pktNs = 10000000;
    int64_t queuedEndNs = 0; // write cursor: when the last queued sample will be heard
    int resyncs = 0, resyncsLate = 0, underruns = 0;
    double maxErrMs = 0;
    for (int i = 0; i < 1000; i++) {
        int64_t pts = 1000000000LL + i * pktNs;
        int64_t jitter = (std::rand() % 30) * 1000000LL; // 0..29 ms
        int64_t now = pts + 5000000 + jitter;             // 5 ms network + jitter
        sch.NoteArrival(pts, now);
        bool starved = queuedEndNs <= now;
        if (starved && i > 0) underruns++;
        int64_t cursor = starved ? now : queuedEndNs;
        AudioPlan p = sch.Plan(pts, frames, cursor, starved);
        if (p.resync) {
            resyncs++;
            if (i > 100) resyncsLate++;
        }
        if (p.drop) continue;
        int64_t start = cursor + (int64_t)p.silenceFrames * 1000000000LL / 48000;
        queuedEndNs = start + (int64_t)(frames - p.skipFrames) * 1000000000LL / 48000;
        if (i > 100) maxErrMs = (std::max)(maxErrMs, std::fabs(sch.LastErrorNs() / 1e6));
    }
    INFO("target delay %.1f ms (arrival lag %.1f), resyncs %d (after warm-up %d), underruns %d, max |err| %.1f ms",
         sch.TargetDelayNs() / 1e6, sch.ArrivalLagNs() / 1e6, resyncs, resyncsLate, underruns, maxErrMs);
    CHECK(resyncsLate == 0);
    CHECK(underruns <= 2);
    CHECK_NEAR(sch.TargetDelayNs() / 1e6, 34 + 50, 1.5);
    // video latency larger than the audio target: audio follows video
    sch.SetVideoLatencyNs(120000000);
    CHECK_NEAR(sch.TargetDelayNs() / 1e6, 120, 0.01);
    // user trim
    sch.SetOffsetMs(-40);
    CHECK_NEAR(sch.TargetDelayNs() / 1e6, 80, 0.01);
    sch.SetOffsetMs(-300); // clamped by min buffer
    CHECK_NEAR(sch.TargetDelayNs() / 1e6, 34 + 15, 1.5);
    // small video wobble does not move the target
    sch.SetOffsetMs(0);
    sch.SetVideoLatencyNs(124000000);
    CHECK_NEAR(sch.VideoLatencyNs() / 1e6, 120, 0.01);
}

TEST(queues_latest_mailbox_and_bounded)
{
    LatestMailbox<int> mb;
    CHECK(!mb.Put(1));
    CHECK(mb.Put(2)); // replaced: one dropped frame
    int v = 0;
    CHECK(mb.Take(v) && v == 2);
    CHECK(!mb.Take(v));
    std::thread t([&] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); mb.Put(3); });
    CHECK(mb.Wait(std::chrono::milliseconds(1000)));
    t.join();
    CHECK(mb.Take(v) && v == 3);

    BoundedQueue<int> q(3);
    CHECK(q.TryPush(1) && q.TryPush(2) && q.TryPush(3));
    CHECK(!q.TryPush(4));
    CHECK(q.PushEvict(5) == 1);
    CHECK(q.Size() == 3);
    CHECK(q.TryPop(v) && v == 2);
    CHECK(q.RemoveIf([](int x) { return x == 3; }) == 1);
}

TEST(stats_json_shape)
{
    MediaStats s;
    s.codec = "h264";
    s.codedWidth = 1920;
    s.codedHeight = 886;
    s.crop = RectF::Make(0.1f, 0, 0.8f, 1);
    std::string j = ToJson(s);
    CHECK(j.find("{\"latencyMs\":") == 0);
    CHECK(j.find("\"size\":\"1920x886\"") != std::string::npos);
    CHECK(j.find("\"cropRect\":\"10% 0% 80% 100%\"") != std::string::npos);
    CHECK(j.find("\"video\":{") != std::string::npos);
    CHECK(j.find("\"codedSize\":[1920,886]") != std::string::npos);
    CHECK(j.find("\"crop\":[0.1000,0.0000,0.8000,1.0000]") != std::string::npos);
    CHECK(j.find("\"audio\":{") != std::string::npos);
    RateWindow w;
    w.Add(1, 2.0);
    for (int i = 1; i <= 60; i++) w.Add(1 + i * 16666667LL, 4.0);
    w.Tick(1 + 1100000000LL);
    CHECK_NEAR(w.Rate(), 60, 2);
    CHECK_NEAR(w.Max(), 4, 1e-9);
}
