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
        void SetDisplayActive(bool active);   // UI thread

        bool m_windowRulesApplied = false;
        bool m_servicesStarted = false;
        bool m_stateSubscribed = false;
        Windows::System::Display::DisplayRequest m_displayRequest{ nullptr };
        bool m_displayActive = false;
    };
}
