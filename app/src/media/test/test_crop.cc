// xbox-cast media harness: CropDetector on synthetic and on real encoded/decoded frames.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdlib>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include "CropDetector.h"
#include "FfVideoDecoder.h"
#include "fixtures.h"
#include "xtest.h"

using namespace XboxCast::Media;

static const int W = CropDetector::kSampleW, H = CropDetector::kSampleH;

// letterboxed synthetic sample: black (16 + noise) outside [x0,x1)x[y0,y1), textured content inside
static std::vector<uint8_t> Synth(int x0, int y0, int x1, int y1, int contentBase, int contentVar, unsigned seed)
{
    std::srand(seed);
    std::vector<uint8_t> img((size_t)W * H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            bool in = x >= x0 && x < x1 && y >= y0 && y < y1;
            int v = in ? contentBase + (std::rand() % (2 * contentVar + 1)) - contentVar : 16 + (std::rand() % 3) - 1;
            img[(size_t)y * W + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    return img;
}

TEST(crop_synthetic_analyze)
{
    CropDetector d;
    auto img = Synth(23, 0, 233, H, 120, 60, 1);
    CropSample s = d.Analyze(img.data(), W, H, W, false);
    CHECK(s.valid);
    CHECK(s.left == 23 && s.right == 232 && s.top == 0 && s.bottom == H - 1);
    auto black = Synth(0, 0, 0, 0, 0, 0, 2);
    CHECK(!d.Analyze(black.data(), W, H, W, false).valid);
    // dark textured content (mean ~30, grain) is NOT a bar
    auto dark = Synth(0, 20, W, H - 20, 30, 12, 3);
    s = d.Analyze(dark.data(), W, H, W, false);
    CHECK(s.valid && s.top == 20 && s.bottom == H - 21);
}

TEST(crop_hysteresis_dark_scene_and_expand)
{
    CropDetector d;
    auto box = Synth(0, 18, W, H - 18, 120, 60, 10); // 2.39-ish letterbox
    for (int i = 0; i < 2; i++) d.Push(box.data(), W, H, W, false);
    RectF r = d.Content();
    CHECK_NEAR(r.y, (18 + 0.5) / H, 1e-3);
    CHECK_NEAR(r.h, (H - 36 - 1.0) / H, 1e-3);
    // dark scene: the top/bottom thirds of the picture go black for 4 samples (2 s). Must not crop further.
    for (int i = 0; i < 4; i++) {
        auto darker = Synth(0, 50, W, H - 50, 120, 60, 20 + i);
        CHECK(!d.Push(darker.data(), W, H, W, false));
    }
    CHECK(NearlyEqual(d.Content(), r, 1e-6f));
    // content goes full-frame (bars gone): expands after 2 samples (~1 s)
    auto full = Synth(0, 0, W, H, 120, 60, 40);
    d.Push(full.data(), W, H, W, false);
    CHECK(NearlyEqual(d.Content(), r, 1e-6f));
    d.Push(full.data(), W, H, W, false);
    CHECK(NearlyEqual(d.Content(), RectF::Unit(), 1e-6f));
    // off-center dark area (dark-mode UI on the left) is not cropped: symmetric rule
    CropDetector e;
    auto lopsided = Synth(60, 0, W, H, 120, 60, 50);
    for (int i = 0; i < 3; i++) e.Push(lopsided.data(), W, H, W, false);
    CHECK(NearlyEqual(e.Content(), RectF::Unit(), 1e-6f));
    // tiny logo on black: rejected
    CropDetector f;
    auto logo = Synth(120, 64, 136, 80, 200, 20, 60);
    for (int i = 0; i < 3; i++) f.Push(logo.data(), W, H, W, false);
    CHECK(NearlyEqual(f.Content(), RectF::Unit(), 1e-6f));
}

// Decodes a fixture in software and feeds a crop sample every `every` frames, like the renderer does every ~500 ms.
static RectF RunCropFixture(const char* name, int every, int* changes, bool logEach = false)
{
    xt::Stream s = xt::ReadStream(xt::Fixture(name), AVMEDIA_TYPE_VIDEO);
    FfVideoDecoder dec;
    VideoDecoderOptions o;
    o.codec = XC_VIDEO_H264;
    CHECK(dec.Open(o));
    CropDetector det;
    std::vector<uint8_t> small((size_t)W * H);
    int n = 0;
    *changes = 0;
    for (auto& p : s.packets) {
        dec.Decode(p.data.data(), p.data.size(), p.ptsUs * 1000, p.key, [&](AVFrame* f, const DecodeTiming&) {
            if (n++ % every) return;
            DownscaleLuma(f->data[0], f->width, f->height, f->linesize[0], small.data(), W, H);
            bool full = f->color_range == AVCOL_RANGE_JPEG;
            if (det.Push(small.data(), W, H, W, full)) {
                (*changes)++;
                RectF c = det.Content();
                if (logEach) INFO("frame %d: content [%.4f %.4f %.4f %.4f]", n - 1, c.x, c.y, c.w, c.h);
            }
        });
    }
    RectF c = det.Content();
    INFO("%s: %d frames, %d samples, %d rect changes, content [x %.4f y %.4f w %.4f h %.4f]", name, n, det.Samples(),
         *changes, c.x, c.y, c.w, c.h);
    return c;
}

TEST(crop_real_pillarbox_in_landscape_phone)
{
    int ch;
    RectF c = RunCropFixture("crop_pillar.h264", 15, &ch);
    CHECK_NEAR(c.x, 172.0 / 1920, 0.006);
    CHECK_NEAR(c.w, 1576.0 / 1920, 0.012);
    CHECK_NEAR(c.y, 0, 1e-6);
    CHECK(ch == 1);
}

TEST(crop_real_16x9_video_in_portrait_phone)
{
    int ch;
    RectF c = RunCropFixture("crop_portrait_video.h264", 15, &ch);
    CHECK_NEAR(c.y, 400.0 / 1080, 0.008);
    CHECK_NEAR(c.h, 280.0 / 1080, 0.016);
    CHECK_NEAR(c.x, 0, 1e-6);
    CHECK(ch == 1);
}

TEST(crop_real_scope_film_with_grain)
{
    int ch;
    RectF c = RunCropFixture("crop_scope.h264", 15, &ch);
    CHECK_NEAR(c.y, 138.0 / 1080, 0.008);
    CHECK_NEAR(c.h, 804.0 / 1080, 0.016);
    CHECK(ch == 1);
}

TEST(crop_real_dark_scene_keeps_content)
{
    int ch;
    RectF c = RunCropFixture("crop_dark.h264", 15, &ch);
    // never crop into the (dark) picture; finding the real bars is the goal
    CHECK(c.y <= 138.0 / 1080 + 0.008);
    CHECK(c.Bottom() >= (138.0 + 804.0) / 1080 - 0.008);
    CHECK_NEAR(c.y, 138.0 / 1080, 0.008);
    CHECK(ch <= 1);
}

TEST(crop_real_full_frame_stays_full)
{
    int ch;
    RectF c = RunCropFixture("crop_full.h264", 15, &ch);
    CHECK(NearlyEqual(c, RectF::Unit(), 1e-6f));
    CHECK(ch == 0);
}

TEST(crop_real_fade_through_black)
{
    int ch;
    RectF c = RunCropFixture("crop_fade.h264", 15, &ch, true);
    CHECK_NEAR(c.x, 172.0 / 1920, 0.006);
    CHECK_NEAR(c.w, 1576.0 / 1920, 0.012);
    CHECK(ch == 1); // the fade never produced a bogus intermediate crop
}
