// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "ViewTransform.h"

#include <algorithm>
#include <cmath>

// Note: (std::min)/(std::max) in parentheses everywhere: the app's pch may pull in <windows.h> without NOMINMAX.

namespace XboxCast { namespace Media {

const char* ViewModeName(ViewMode m)
{
    switch (m) {
    case ViewMode::Fit: return "Fit";
    case ViewMode::Fill: return "Fill";
    case ViewMode::Zoom: return "Zoom";
    case ViewMode::Auto: return "Auto";
    }
    return "?";
}

bool NearlyEqual(const RectF& a, const RectF& b, float eps)
{
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps && std::fabs(a.w - b.w) <= eps &&
           std::fabs(a.h - b.h) <= eps;
}

float FillLoss(float contentW, float contentH, float viewW, float viewH)
{
    if (contentW <= 0 || contentH <= 0 || viewW <= 0 || viewH <= 0) return 0;
    float ca = contentW / contentH, va = viewW / viewH;
    float r = ca < va ? ca / va : va / ca;
    return 1.0f - r;
}

static float Clamp(float v, float lo, float hi) { return (std::max)(lo, (std::min)(hi, v)); }

// Pan for one axis: content of length len (pixels at this scale) shown in a view of length view.
static float ClampPanAxis(float pan, float len, float view)
{
    if (len <= view) return 0.5f;          // content fits: centered
    float half = (view * 0.5f) / len;      // half the visible fraction
    return Clamp(pan, half, 1.0f - half);
}

void ClampPan(const ViewParams& p, float scale, float* panX, float* panY)
{
    float cw = p.content.w * p.frameW * scale;
    float ch = p.content.h * p.frameH * scale;
    *panX = ClampPanAxis(*panX, cw, (float)p.viewW);
    *panY = ClampPanAxis(*panY, ch, (float)p.viewH);
}

Layout ComputeLayout(const ViewParams& p)
{
    Layout l;
    RectF c = p.content;
    if (c.Empty()) c = RectF::Unit();
    // keep C inside the frame
    c.x = Clamp(c.x, 0, 1);
    c.y = Clamp(c.y, 0, 1);
    c.w = Clamp(c.w, 0.01f, 1 - c.x);
    c.h = Clamp(c.h, 0.01f, 1 - c.y);
    l.content = c;
    if (p.frameW <= 0 || p.frameH <= 0 || p.viewW <= 0 || p.viewH <= 0) {
        l.placement = RectF::Make(0, 0, (float)p.viewW, (float)p.viewH);
        l.scale = 0;
        return l;
    }
    float cw = c.w * p.frameW, ch = c.h * p.frameH; // content size in frame pixels
    float fit = (std::min)(p.viewW / cw, p.viewH / ch);
    float fill = (std::max)(p.viewW / cw, p.viewH / ch);

    ViewMode mode = p.mode;
    float scale = fit;
    if (mode == ViewMode::Auto) {
        float loss = FillLoss(cw, ch, (float)p.viewW, (float)p.viewH);
        mode = loss <= p.autoFillMaxLoss ? ViewMode::Fill : ViewMode::Fit;
    }
    switch (mode) {
    case ViewMode::Fit: scale = fit; break;
    case ViewMode::Fill: scale = fill; break;
    case ViewMode::Zoom: scale = fit * Clamp(p.zoom, 1.0f, 3.0f); break;
    default: break;
    }
    l.resolvedMode = mode;
    l.scale = scale;

    float panX = p.panX, panY = p.panY;
    if (mode == ViewMode::Fit) { panX = 0.5f; panY = 0.5f; }
    ClampPan(p, scale, &panX, &panY);

    float fw = p.frameW * scale, fh = p.frameH * scale;
    // the content point (c.x + panX*c.w) lands on the view center
    l.placement.w = fw;
    l.placement.h = fh;
    l.placement.x = p.viewW * 0.5f - (c.x + panX * c.w) * fw;
    l.placement.y = p.viewH * 0.5f - (c.y + panY * c.h) * fh;
    return l;
}

DrawRects ComputeDrawRects(const Layout& l, int viewW, int viewH)
{
    DrawRects d;
    const RectF& F = l.placement;
    if (F.Empty()) return d;
    // content rect in view pixels
    float x0 = F.x + l.content.x * F.w, y0 = F.y + l.content.y * F.h;
    float x1 = x0 + l.content.w * F.w, y1 = y0 + l.content.h * F.h;
    // clip to view
    float cx0 = (std::max)(x0, 0.0f), cy0 = (std::max)(y0, 0.0f);
    float cx1 = (std::min)(x1, (float)viewW), cy1 = (std::min)(y1, (float)viewH);
    if (cx1 <= cx0 || cy1 <= cy0) return d;
    d.dst = RectF::Make(cx0, cy0, cx1 - cx0, cy1 - cy0);
    // back to frame UV
    d.src.x = (cx0 - F.x) / F.w;
    d.src.y = (cy0 - F.y) / F.h;
    d.src.w = (cx1 - cx0) / F.w;
    d.src.h = (cy1 - cy0) / F.h;
    return d;
}

static float Lerp(float a, float b, float t) { return a + (b - a) * t; }
static RectF LerpRect(const RectF& a, const RectF& b, float t)
{
    return RectF::Make(Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.w, b.w, t), Lerp(a.h, b.h, t));
}

Layout LerpLayout(const Layout& a, const Layout& b, float t)
{
    Layout r;
    r.placement = LerpRect(a.placement, b.placement, t);
    r.content = LerpRect(a.content, b.content, t);
    r.resolvedMode = t < 1.0f ? a.resolvedMode : b.resolvedMode;
    r.scale = Lerp(a.scale, b.scale, t);
    return r;
}

float EaseOut(float t)
{
    t = Clamp(t, 0, 1);
    float u = 1 - t;
    return 1 - u * u * u;
}

void LayoutAnimator::SetTarget(const Layout& target, int64_t nowNs, bool snap)
{
    if (!m_hasTarget || snap) {
        m_from = target;
        m_to = target;
        m_startNs = nowNs - m_durationNs;
        m_hasTarget = true;
        return;
    }
    if (NearlyEqual(target.placement, m_to.placement, 0.25f) && NearlyEqual(target.content, m_to.content, 1e-4f))
        return; // same target: don't restart the curve
    m_from = Current(nowNs);
    m_to = target;
    m_startNs = nowNs;
}

Layout LayoutAnimator::Current(int64_t nowNs) const
{
    if (!m_hasTarget) return Layout();
    if (m_durationNs <= 0) return m_to;
    float t = (float)(nowNs - m_startNs) / (float)m_durationNs;
    if (t >= 1) return m_to;
    return LerpLayout(m_from, m_to, EaseOut(t));
}

bool LayoutAnimator::Animating(int64_t nowNs) const
{
    return m_hasTarget && (nowNs - m_startNs) < m_durationNs;
}

}} // namespace XboxCast::Media
