#pragma once

#include "MainPage.g.h"

namespace winrt::XboxCast::implementation
{
    // Placeholder (platform bootstrap, contract §3). ui owns this file from here on.
    struct MainPage : MainPageT<MainPage>
    {
        MainPage() = default;
        void InitializeComponent();
    };
}

namespace winrt::XboxCast::factory_implementation
{
    struct MainPage : MainPageT<MainPage, implementation::MainPage>
    {
    };
}
