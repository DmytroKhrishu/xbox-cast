// xbox-cast media: black-bar (letterbox/pillarbox) detector (portable, no platform headers).
//
// Input: a small luma image (the renderer GPU-downsamples the decoded Y plane to kSampleW x kSampleH and reads it
// back every ~500 ms; the Linux harness box-filters decoded frames the same way).
// Output: the normalized content rect inside the frame.
//
// Per sample: a row/column is "bar" when it is flat and near black: mean <= black + meanMargin, stddev <= flatStd,
// and almost no pixels brighter than black + brightDelta. Encoded bars are flat; dark film scenes have texture/grain.
// Extents = first/last non-bar row/column. An all-bar sample (fade to black) carries no information and is ignored.
//
// Stability / hysteresis:
//   * a new extent must be seen confirmSamples times in a row (within tolerance) before it is accepted;
//   * the reported rect is the UNION of the accepted extents of the last windowSamples samples, so
//     content expanding (bars shrinking) shows up after confirmSamples (~1 s), while bars growing needs the whole
//     window (~3 s) of consistent evidence: a dark scene cannot crop content seen moments ago;
//   * changes smaller than minChange are ignored, tiny bars (< minBar) are treated as no bar, and a content rect
//     smaller than minContent of the frame is rejected (mostly-black screen with a small logo).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <deque>

#include "ViewTransform.h"

namespace XboxCast { namespace Media {

struct CropConfig {
    int   meanMargin = 10;       // 8-bit levels above black for a bar row's mean
    int   brightDelta = 32;      // pixel counts as "bright" above black + this
    float maxBrightFrac = 0.01f; // bar rows have at most this fraction of bright pixels
    float flatStd = 3.5f;        // bar rows are flat (8-bit stddev)
    int   confirmSamples = 2;
    int   windowSamples = 6;
    float minChange = 0.01f;     // ignore rect changes below this (normalized)
    float minBar = 0.012f;       // bars thinner than this are not cropped (keeps the full frame for near-full content)
    float minContent = 0.15f;    // reject content smaller than this in either dimension
    float inset = 0.5f;          // extra samples cropped inward at a detected edge (hides the blended edge line)
    bool  symmetric = true;      // crop each axis by the smaller of its two bars: senders center letterboxed content,
                                 // an off-center dark area (dark-mode UI, candle-lit scene) is not a bar
};

struct CropSample {
    bool valid = false;  // false: all black / no information
    int top = 0, bottom = 0, left = 0, right = 0; // inclusive content extents in sample pixels
};

class CropDetector {
public:
    static const int kSampleW = 256;
    static const int kSampleH = 144;

    explicit CropDetector(const CropConfig& cfg = CropConfig()) : m_cfg(cfg) {}

    // Analyze one luma image (8-bit). fullRange: black = 0, else black = 16. Returns true if the reported content
    // rect changed.
    bool Push(const uint8_t* luma, int w, int h, int stride, bool fullRange);

    // Raw single-image analysis (no hysteresis), exposed for tests.
    CropSample Analyze(const uint8_t* luma, int w, int h, int stride, bool fullRange) const;

    RectF Content() const { return m_content; }
    int Samples() const { return m_samples; }
    void Reset();

private:
    bool IsBarLine(const uint8_t* p, int count, int step, int black) const;
    RectF ToRect(const CropSample& s, int w, int h) const;

    CropConfig m_cfg;
    RectF m_content = RectF::Unit();
    std::deque<CropSample> m_window;  // accepted samples
    CropSample m_candidate;
    int m_candidateCount = 0;
    int m_samples = 0;
    int m_w = 0, m_h = 0;
};

// Box-filter downscale of an 8-bit plane to dw x dh (what the GPU path approximates). For tests and the software
// decode path.
void DownscaleLuma(const uint8_t* src, int sw, int sh, int sstride, uint8_t* dst, int dw, int dh);
// Same for a 16-bit plane holding `bits`-bit samples (P010 / yuv420p10), output 8-bit.
void DownscaleLuma16(const uint16_t* src, int sw, int sh, int sstrideElems, int bits, bool msbAligned, uint8_t* dst,
                     int dw, int dh);

}} // namespace XboxCast::Media
