#pragma once
// Logical controller keys. Pad (VirtualKey) and dev-server /cmd ("key" names) both map here, then go through
// MainPage::OnKey, so agents drive exactly the code path a controller does (docs/controller-ux.md).

#include <cwchar>
#include <string>
#include <winrt/Windows.System.h>

namespace xcui {

enum class Key { None, A, B, X, Y, LB, RB, LT, RT, Up, Down, Left, Right, Menu, View };

inline wchar_t const* KeyName(Key k) {
    switch (k) {
    case Key::A: return L"A";
    case Key::B: return L"B";
    case Key::X: return L"X";
    case Key::Y: return L"Y";
    case Key::LB: return L"LB";
    case Key::RB: return L"RB";
    case Key::LT: return L"LT";
    case Key::RT: return L"RT";
    case Key::Up: return L"Up";
    case Key::Down: return L"Down";
    case Key::Left: return L"Left";
    case Key::Right: return L"Right";
    case Key::Menu: return L"Menu";
    case Key::View: return L"View";
    default: return L"None";
    }
}

inline Key KeyFromName(std::wstring const& n) {
    static Key const all[] = {Key::A, Key::B, Key::X, Key::Y, Key::LB, Key::RB, Key::LT, Key::RT,
                              Key::Up, Key::Down, Key::Left, Key::Right, Key::Menu, Key::View};
    for (Key k : all) {
        if (_wcsicmp(n.c_str(), KeyName(k)) == 0) return k;
    }
    return Key::None;
}

// Analog inputs (triggers, right stick) are NOT mapped: MainPage polls Windows.Gaming.Input while mirroring.
// Keyboard keys are mapped too so the app is usable on a desktop dev build.
inline Key KeyFromVirtualKey(winrt::Windows::System::VirtualKey vk) {
    using winrt::Windows::System::VirtualKey;
    switch (vk) {
    case VirtualKey::GamepadA:
    case VirtualKey::Enter:
    case VirtualKey::Space: return Key::A;
    case VirtualKey::GamepadB:
    case VirtualKey::Escape:
    case VirtualKey::Back: return Key::B;
    case VirtualKey::GamepadX: return Key::X;
    case VirtualKey::GamepadY: return Key::Y;
    case VirtualKey::GamepadLeftShoulder:
    case VirtualKey::PageUp: return Key::LB;
    case VirtualKey::GamepadRightShoulder:
    case VirtualKey::PageDown: return Key::RB;
    case VirtualKey::GamepadDPadUp:
    case VirtualKey::GamepadLeftThumbstickUp:
    case VirtualKey::Up: return Key::Up;
    case VirtualKey::GamepadDPadDown:
    case VirtualKey::GamepadLeftThumbstickDown:
    case VirtualKey::Down: return Key::Down;
    case VirtualKey::GamepadDPadLeft:
    case VirtualKey::GamepadLeftThumbstickLeft:
    case VirtualKey::Left: return Key::Left;
    case VirtualKey::GamepadDPadRight:
    case VirtualKey::GamepadLeftThumbstickRight:
    case VirtualKey::Right: return Key::Right;
    case VirtualKey::GamepadMenu:
    case VirtualKey::F10: return Key::Menu;
    case VirtualKey::GamepadView:
    case VirtualKey::F9: return Key::View;
    default: return Key::None;
    }
}

// Pad keys we consume without mapping, so XAML never moves focus on its own.
inline bool IsSwallowedVirtualKey(winrt::Windows::System::VirtualKey vk) {
    using winrt::Windows::System::VirtualKey;
    switch (vk) {
    case VirtualKey::GamepadLeftTrigger:
    case VirtualKey::GamepadRightTrigger:
    case VirtualKey::GamepadRightThumbstickUp:
    case VirtualKey::GamepadRightThumbstickDown:
    case VirtualKey::GamepadRightThumbstickLeft:
    case VirtualKey::GamepadRightThumbstickRight:
    case VirtualKey::GamepadLeftThumbstickButton:
    case VirtualKey::GamepadRightThumbstickButton:
    case VirtualKey::Tab: return true;
    default: return false;
    }
}

inline bool IsDirection(Key k) {
    return k == Key::Up || k == Key::Down || k == Key::Left || k == Key::Right;
}

}  // namespace xcui
