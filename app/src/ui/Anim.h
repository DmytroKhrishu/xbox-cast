#pragma once
// Small storyboard helpers for the calm motion language: fades and slides 250-350 ms, cubic ease-out.
// Only Opacity and RenderTransform properties are animated (independent animations, run on the compositor).

#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Media.Animation.h>

namespace xcui::anim {

constexpr int kFast = 150;
constexpr int kNormal = 250;
constexpr int kCalm = 300;
constexpr int kSlow = 350;

// Fades el to `to`. Fading in makes it Visible first; fading to 0 collapses it at the end (unless a newer fade
// on the same element started meanwhile).
void Fade(winrt::Windows::UI::Xaml::UIElement const& el, double to, int ms);

// Animates a double property (e.g. a TranslateTransform's "X"/"Y" or "Opacity") from its current value to `to`.
void To(winrt::Windows::UI::Xaml::DependencyObject const& target, wchar_t const* property, double to, int ms);

// Endless linear loop without reverse (spinner rotation).
winrt::Windows::UI::Xaml::Media::Animation::Storyboard Spin(
    winrt::Windows::UI::Xaml::DependencyObject const& target, wchar_t const* property, int ms);

}  // namespace xcui::anim
