#include "pch.h"
#include "MainPage.h"
#include "MainPage.g.cpp"

using namespace winrt;
using namespace Windows::ApplicationModel;

namespace winrt::XboxCast::implementation
{
    // Placeholder (platform bootstrap, contract §3). ui owns this file from here on.
    void MainPage::InitializeComponent()
    {
        MainPageT::InitializeComponent();
        auto v = Package::Current().Id().Version();
        VersionText().Text(L"version " + to_hstring(v.Major) + L"." + to_hstring(v.Minor) + L"." + to_hstring(v.Build));
    }
}
