// xbox-cast media: view transform math (portable, no platform headers).
//
// Model: the decoded frame (visible size frameW x frameH, square pixels) is placed in the view (viewW x viewH pixels)
// by an axis-aligned "placement" rect F: frame UV (u,v) in [0,1] lands on view pixel (F.x + u*F.w, F.y + v*F.h).
// Only the part of the frame inside the content rect C (normalized frame coords, e.g. the auto-crop result) is drawn,
// clipped to the view. Everything else is black.
//
// Animating between two layouts = interpolating F and C linearly (an affine map), then clipping. That keeps the
// magnification consistent during the transition (no "rubber" effect from interpolating src and dst separately).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace XboxCast { namespace Media {

enum class ViewMode { Fit, Fill, Zoom, Auto };

const char* ViewModeName(ViewMode m);

struct RectF {
    float x = 0, y = 0, w = 0, h = 0;
    float Right() const { return x + w; }
    float Bottom() const { return y + h; }
    bool Empty() const { return w <= 0 || h <= 0; }
    static RectF Unit() { RectF r; r.w = 1; r.h = 1; return r; }
    static RectF Make(float x, float y, float w, float h) { RectF r; r.x = x; r.y = y; r.w = w; r.h = h; return r; }
};

bool NearlyEqual(const RectF& a, const RectF& b, float eps);

struct ViewParams {
    int frameW = 0, frameH = 0;     // visible decoded size
    int viewW = 0, viewH = 0;       // render target size in pixels
    ViewMode mode = ViewMode::Auto;
    float zoom = 1.0f;              // Zoom mode: 1..3, relative to Fit
    float panX = 0.5f, panY = 0.5f; // point of the content (normalized in C) to keep at the view center
    RectF content = RectF::Unit();  // C: crop rect in normalized frame coords (Unit when auto-crop is off)
    float autoFillMaxLoss = 0.08f;  // Auto: Fill when it hides at most this fraction of the content, else Fit
};

struct Layout {
    RectF placement;               // F, view pixels
    RectF content = RectF::Unit(); // C, normalized frame coords
    ViewMode resolvedMode = ViewMode::Fit; // Auto resolves to Fit or Fill
    float scale = 0;               // view pixels per frame pixel
};

// What to draw: a quad at dst (view pixels) sampling src (normalized frame UV). Empty when nothing is visible.
struct DrawRects {
    RectF dst;
    RectF src;
};

// Fraction of the content that Fill hides for a content aspect vs view aspect (0 = same aspect).
float FillLoss(float contentW, float contentH, float viewW, float viewH);

// Clamp a pan center so the content never leaves the view interior in a dimension where it is larger than the view
// (and is centered where it is smaller). Returns the clamped pan for the given layout scale.
void ClampPan(const ViewParams& p, float scale, float* panX, float* panY);

Layout ComputeLayout(const ViewParams& p);
DrawRects ComputeDrawRects(const Layout& l, int viewW, int viewH);

Layout LerpLayout(const Layout& a, const Layout& b, float t);

// Ease-out cubic.
float EaseOut(float t);

// Animates between layouts. Retargeting mid-flight starts from the current interpolated layout (never pops).
class LayoutAnimator {
public:
    explicit LayoutAnimator(int64_t durationNs = 300000000) : m_durationNs(durationNs) {}

    // Animate from the current position to target. If snap, jump (used for a new stream/rotation where the old
    // frame geometry is meaningless).
    void SetTarget(const Layout& target, int64_t nowNs, bool snap);
    Layout Current(int64_t nowNs) const;
    bool Animating(int64_t nowNs) const;
    bool HasTarget() const { return m_hasTarget; }
    const Layout& Target() const { return m_to; }
    void Reset() { m_hasTarget = false; }

private:
    int64_t m_durationNs;
    bool m_hasTarget = false;
    Layout m_from, m_to;
    int64_t m_startNs = 0;
};

}} // namespace XboxCast::Media
