// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "CropDetector.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace XboxCast { namespace Media {

bool CropDetector::IsBarLine(const uint8_t* p, int count, int step, int black) const
{
    if (count <= 0) return true;
    int64_t sum = 0, sum2 = 0;
    int bright = 0;
    const int brightLevel = black + m_cfg.brightDelta;
    for (int i = 0; i < count; i++) {
        int v = p[(size_t)i * step];
        sum += v;
        sum2 += (int64_t)v * v;
        if (v > brightLevel) bright++;
    }
    double mean = (double)sum / count;
    double var = (double)sum2 / count - mean * mean;
    double sd = var > 0 ? std::sqrt(var) : 0.0;
    if (mean > black + m_cfg.meanMargin) return false;
    if (sd > m_cfg.flatStd) return false;
    if (bright > m_cfg.maxBrightFrac * count) return false;
    return true;
}

CropSample CropDetector::Analyze(const uint8_t* luma, int w, int h, int stride, bool fullRange) const
{
    CropSample s;
    if (!luma || w <= 0 || h <= 0) return s;
    const int black = fullRange ? 0 : 16;
    int top = 0;
    while (top < h && IsBarLine(luma + (size_t)top * stride, w, 1, black)) top++;
    if (top == h) return s; // all black: no information
    int bottom = h - 1;
    while (bottom > top && IsBarLine(luma + (size_t)bottom * stride, w, 1, black)) bottom--;
    int left = 0;
    while (left < w && IsBarLine(luma + left, h, stride, black)) left++;
    int right = w - 1;
    while (right > left && IsBarLine(luma + right, h, stride, black)) right--;
    if (left >= w) return s;
    s.valid = true;
    s.top = top;
    s.bottom = bottom;
    s.left = left;
    s.right = right;
    return s;
}

RectF CropDetector::ToRect(const CropSample& s, int w, int h) const
{
    // bar sizes in sample pixels
    int barL = s.left, barR = w - 1 - s.right, barT = s.top, barB = h - 1 - s.bottom;
    if (m_cfg.symmetric) {
        barL = barR = (std::min)(barL, barR);
        barT = barB = (std::min)(barT, barB);
    }
    float x0 = 0, y0 = 0, x1 = 1, y1 = 1;
    const float in = m_cfg.inset;
    if ((float)barL / w >= m_cfg.minBar) x0 = (barL + in) / w;
    if ((float)barR / w >= m_cfg.minBar) x1 = (w - barR - in) / w;
    if ((float)barT / h >= m_cfg.minBar) y0 = (barT + in) / h;
    if ((float)barB / h >= m_cfg.minBar) y1 = (h - barB - in) / h;
    return RectF::Make(x0, y0, x1 - x0, y1 - y0);
}

static bool SameExtents(const CropSample& a, const CropSample& b, int tolX, int tolY)
{
    return a.valid == b.valid && std::abs(a.top - b.top) <= tolY && std::abs(a.bottom - b.bottom) <= tolY &&
           std::abs(a.left - b.left) <= tolX && std::abs(a.right - b.right) <= tolX;
}

bool CropDetector::Push(const uint8_t* luma, int w, int h, int stride, bool fullRange)
{
    m_samples++;
    if (w != m_w || h != m_h) {
        m_window.clear();
        m_candidateCount = 0;
        m_w = w;
        m_h = h;
    }
    CropSample s = Analyze(luma, w, h, stride, fullRange);
    if (!s.valid) return false;

    // reject implausibly small content (logo on black, mostly-black UI)
    if ((s.right - s.left + 1) < m_cfg.minContent * w || (s.bottom - s.top + 1) < m_cfg.minContent * h) {
        m_candidateCount = 0;
        return false;
    }

    const int tolX = (std::max)(1, w / 128), tolY = (std::max)(1, h / 72);
    if (m_candidateCount > 0 && SameExtents(s, m_candidate, tolX, tolY)) {
        m_candidateCount++;
    } else {
        m_candidate = s;
        m_candidateCount = 1;
    }
    if (m_candidateCount < m_cfg.confirmSamples) return false;

    m_window.push_back(s);
    while ((int)m_window.size() > m_cfg.windowSamples) m_window.pop_front();

    // union of the accepted samples in the window
    CropSample u = m_window.front();
    for (const CropSample& x : m_window) {
        u.top = (std::min)(u.top, x.top);
        u.left = (std::min)(u.left, x.left);
        u.bottom = (std::max)(u.bottom, x.bottom);
        u.right = (std::max)(u.right, x.right);
    }
    RectF r = ToRect(u, w, h);
    if (NearlyEqual(r, m_content, m_cfg.minChange)) return false;
    m_content = r;
    return true;
}

void CropDetector::Reset()
{
    m_content = RectF::Unit();
    m_window.clear();
    m_candidateCount = 0;
    m_samples = 0;
    m_w = m_h = 0;
}

void DownscaleLuma(const uint8_t* src, int sw, int sh, int sstride, uint8_t* dst, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((int64_t)y * sh / dh), y1 = (int)((int64_t)(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((int64_t)x * sw / dw), x1 = (int)((int64_t)(x + 1) * sw / dw);
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t sum = 0;
            for (int yy = y0; yy < y1; yy++) {
                const uint8_t* row = src + (size_t)yy * sstride;
                for (int xx = x0; xx < x1; xx++) sum += row[xx];
            }
            dst[(size_t)y * dw + x] = (uint8_t)((sum + ((x1 - x0) * (y1 - y0)) / 2) / ((x1 - x0) * (y1 - y0)));
        }
    }
}

void DownscaleLuma16(const uint16_t* src, int sw, int sh, int sstrideElems, int bits, bool msbAligned, uint8_t* dst,
                     int dw, int dh)
{
    const int shift = msbAligned ? 8 : (bits - 8);
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((int64_t)y * sh / dh), y1 = (int)((int64_t)(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((int64_t)x * sw / dw), x1 = (int)((int64_t)(x + 1) * sw / dw);
            if (x1 <= x0) x1 = x0 + 1;
            uint64_t sum = 0;
            for (int yy = y0; yy < y1; yy++) {
                const uint16_t* row = src + (size_t)yy * sstrideElems;
                for (int xx = x0; xx < x1; xx++) sum += (uint32_t)(row[xx] >> shift);
            }
            int n = (x1 - x0) * (y1 - y0);
            dst[(size_t)y * dw + x] = (uint8_t)((std::min)(255u, (uint32_t)((sum + n / 2) / n)));
        }
    }
}

}} // namespace XboxCast::Media
