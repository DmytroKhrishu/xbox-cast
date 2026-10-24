#include "pch.h"
#include "Anim.h"

#include <chrono>
#include <functional>
#include <string>
#include <unordered_map>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.Media.h>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Media::Animation;

namespace xcui::anim {
namespace {

struct Running {
    uint32_t gen = 0;
    Storyboard sb{nullptr};
};

// Keyed by the target's ABI pointer + property; all targets live as long as the page.
std::unordered_map<std::wstring, Running>& Table() {
    static std::unordered_map<std::wstring, Running> t;
    return t;
}

std::wstring Slot(DependencyObject const& target, wchar_t const* property) {
    wchar_t buf[40];
    swprintf(buf, 40, L"%p:", get_abi(target));
    return std::wstring(buf) + property;
}

Duration Ms(int ms) {
    return DurationHelper::FromTimeSpan(TimeSpan{std::chrono::milliseconds(ms)});
}

EasingFunctionBase EaseOut() {
    CubicEase e;
    e.EasingMode(EasingMode::EaseOut);
    return e;
}

// Starts (or replaces) the animation in a slot. Replacing keeps the current animated value as the new base, so a
// fade-out interrupted by a fade-in continues from where it is instead of popping.
uint32_t Run(DependencyObject const& target, wchar_t const* property, double to, int ms, double current,
             std::function<void(DependencyObject const&, double)> const& setBase, Storyboard& out) {
    auto& slot = Table()[Slot(target, property)];
    if (slot.sb) {
        slot.sb.Stop();
        setBase(target, current);
    }
    DoubleAnimation a;
    a.To(to);
    a.Duration(Ms(ms));
    a.EasingFunction(EaseOut());
    Storyboard::SetTarget(a, target);
    Storyboard::SetTargetProperty(a, property);
    Storyboard sb;
    sb.Children().Append(a);
    slot.sb = sb;
    slot.gen++;
    out = sb;
    return slot.gen;
}

}  // namespace

void Fade(UIElement const& el, double to, int ms) {
    double current = el.Opacity();
    if (to > 0 && el.Visibility() != Visibility::Visible) {
        current = 0;
        el.Opacity(0);
        el.Visibility(Visibility::Visible);
    }
    Storyboard sb{nullptr};
    uint32_t gen = Run(el, L"Opacity", to, ms, current,
                       [](DependencyObject const& t, double v) { t.as<UIElement>().Opacity(v); }, sb);
    std::wstring key = Slot(el, L"Opacity");
    if (to <= 0) {
        weak_ref<UIElement> weak = make_weak(el);
        sb.Completed([weak, key, gen](IInspectable const&, IInspectable const&) {
            auto it = Table().find(key);
            if (it == Table().end() || it->second.gen != gen) return;
            if (auto e = weak.get()) e.Visibility(Visibility::Collapsed);
        });
    }
    sb.Begin();
}

void To(DependencyObject const& target, wchar_t const* property, double to, int ms) {
    // Current value: read through the property path only for the few properties we animate.
    double current = 0;
    std::wstring p(property);
    if (auto tt = target.try_as<Media::TranslateTransform>()) current = p == L"X" ? tt.X() : tt.Y();
    else if (auto st = target.try_as<Media::ScaleTransform>()) current = p == L"ScaleX" ? st.ScaleX() : st.ScaleY();
    else if (auto ui = target.try_as<UIElement>()) current = ui.Opacity();
    Storyboard sb{nullptr};
    Run(target, property, to, ms, current,
        [p](DependencyObject const& t, double v) {
            if (auto tt = t.try_as<Media::TranslateTransform>()) { p == L"X" ? tt.X(v) : tt.Y(v); }
            else if (auto st = t.try_as<Media::ScaleTransform>()) { p == L"ScaleX" ? st.ScaleX(v) : st.ScaleY(v); }
            else if (auto ui = t.try_as<UIElement>()) ui.Opacity(v);
        },
        sb);
    sb.Begin();
}

Storyboard Spin(DependencyObject const& target, wchar_t const* property, int ms) {
    DoubleAnimation a;
    a.From(0.0);
    a.To(360.0);
    a.Duration(Ms(ms));
    Storyboard::SetTarget(a, target);
    Storyboard::SetTargetProperty(a, property);
    Storyboard sb;
    sb.Children().Append(a);
    sb.RepeatBehavior(RepeatBehaviorHelper::Forever());
    return sb;
}

}  // namespace xcui::anim
