#pragma once
#include "App.xaml.g.h"

namespace winrt::XboxCast::implementation
{
    struct App : AppT<App>
    {
        App();
        void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs const&);

    private:
        void OnSuspending(IInspectable const&, Windows::ApplicationModel::SuspendingEventArgs const&);
        void OnResuming(IInspectable const&, IInspectable const&);
        void OnNavigationFailed(IInspectable const&, Windows::UI::Xaml::Navigation::NavigationFailedEventArgs const&);
        void ApplyWindowRules();
        void StartServices();
        void StopServices();

        bool m_windowRulesApplied = false;
    };
}
