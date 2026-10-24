#include "pch.h"

#include "App.h"
#include <winrt/XboxCast.h>

using namespace winrt;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::Foundation;
using namespace Windows::UI::ViewManagement;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Navigation;
using namespace XboxCast;
using namespace XboxCast::implementation;

App::App()
{
    // Window / XAML rules (docs/contract.md §3). Pointer mode and focus visuals are Application properties, so they are set
    // here before any window exists. FocusVisualKind: Xbox switches the default to Reveal; force the plain kind and let ui
    // turn system focus visuals off per control (UseSystemFocusVisuals="False") and draw its own.
    RequiresPointerMode(ApplicationRequiresPointerMode::WhenRequested);
    FocusVisualKind(Windows::UI::Xaml::FocusVisualKind::HighVisibility);

    Suspending({ this, &App::OnSuspending });
    Resuming({ this, &App::OnResuming });
    UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
    {
        char msg[512];
        snprintf(msg, sizeof msg, "xbox-cast: unhandled exception 0x%08x %ls\n",
                 static_cast<uint32_t>(e.Exception().value), e.Message().c_str());
        OutputDebugStringA(msg);
    });
}

void App::ApplyWindowRules()
{
    if (m_windowRulesApplied) return;
    m_windowRulesApplied = true;
    // Layout in 1920x1080 effective pixels instead of the Xbox's 200% scale (960x540).
    bool scalingOff = ApplicationViewScaling::TrySetDisableLayoutScaling(true);
    // Draw edge to edge: no Xbox overscan margins; the UI keeps its own 5% TV-safe margins.
    bool bounds = ApplicationView::GetForCurrentView().SetDesiredBoundsMode(ApplicationViewBoundsMode::UseCoreWindow);
    char msg[160];
    snprintf(msg, sizeof msg, "xbox-cast: window rules: layoutScalingOff=%d useCoreWindow=%d\n", scalingOff ? 1 : 0, bounds ? 1 : 0);
    OutputDebugStringA(msg);
}

void App::OnLaunched(LaunchActivatedEventArgs const& e)
{
    ApplyWindowRules();

    Frame rootFrame{ nullptr };
    if (auto content = Window::Current().Content())
    {
        rootFrame = content.try_as<Frame>();
    }
    if (rootFrame == nullptr)
    {
        rootFrame = Frame();
        rootFrame.NavigationFailed({ this, &App::OnNavigationFailed });
        Window::Current().Content(rootFrame);
    }
    if (e.PrelaunchActivated() == false)
    {
        if (rootFrame.Content() == nullptr)
        {
            rootFrame.Navigate(xaml_typename<XboxCast::MainPage>(), box_value(e.Arguments()));
        }
        Window::Current().Activate();
        StartServices();
    }
}

void App::StartServices()
{
}

void App::StopServices()
{
}

void App::OnSuspending([[maybe_unused]] IInspectable const& sender, SuspendingEventArgs const& e)
{
    auto deferral = e.SuspendingOperation().GetDeferral();
    StopServices();
    deferral.Complete();
}

void App::OnResuming(IInspectable const&, IInspectable const&)
{
    StartServices();
}

void App::OnNavigationFailed(IInspectable const&, NavigationFailedEventArgs const& e)
{
    throw hresult_error(E_FAIL, hstring(L"Failed to load Page ") + e.SourcePageType().Name);
}
