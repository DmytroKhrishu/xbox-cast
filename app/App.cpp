#include "pch.h"

#include "App.h"
#include <winrt/XboxCast.h>

#include "core/ReceiverHost.h"
#include "diag/Diag.h"
#include "settings/Settings.h"

using namespace winrt;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::Foundation;
using namespace Windows::UI::Core;
using namespace Windows::UI::ViewManagement;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Navigation;
// The app's own C++ namespace ::XboxCast (Diag, Settings, ReceiverHost) and the projection winrt::XboxCast share a name:
// never `using namespace XboxCast;` next to `using namespace winrt;` (ambiguous). Qualify as ::XboxCast::X instead.
using ::XboxCast::Diag;
using ::XboxCast::ReceiverHost;
using ::XboxCast::ReceiverState;

namespace winrt::XboxCast::implementation
{
App::App()
{
    XC_LOGI("app", "process start");
    // Window / XAML rules (docs/contract.md §3). Pointer mode and the focus visual kind are Application properties, so
    // they are set before any window exists. Xbox defaults FocusVisualKind to Reveal; force HighVisibility (the plain
    // kind) and let ui turn system focus visuals off per control (UseSystemFocusVisuals="False") and draw its own.
    RequiresPointerMode(ApplicationRequiresPointerMode::WhenRequested);
    FocusVisualKind(Windows::UI::Xaml::FocusVisualKind::HighVisibility);

    Suspending({ this, &App::OnSuspending });
    Resuming({ this, &App::OnResuming });
    UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
    {
        XC_LOGE("app", "unhandled exception 0x%08x %ls", static_cast<uint32_t>(e.Exception().value), e.Message().c_str());
    });
    EnteredBackground([](IInspectable const&, EnteredBackgroundEventArgs const&) { XC_LOGI("app", "entered background"); });
    LeavingBackground([](IInspectable const&, LeavingBackgroundEventArgs const&) { XC_LOGI("app", "leaving background"); });
}

void App::ApplyWindowRules()
{
    if (m_windowRulesApplied) return;
    m_windowRulesApplied = true;
    // Layout in 1920x1080 effective pixels instead of the Xbox's 200% scale (960x540).
    bool scalingOff = ApplicationViewScaling::TrySetDisableLayoutScaling(true);
    // Draw edge to edge: no Xbox overscan margins; the UI keeps its own 5% TV-safe margins.
    bool bounds = ApplicationView::GetForCurrentView().SetDesiredBoundsMode(ApplicationViewBoundsMode::UseCoreWindow);
    auto b = Window::Current().Bounds();
    XC_LOGI("app", "window rules: layoutScalingOff=%d useCoreWindow=%d bounds=%.0fx%.0f", scalingOff ? 1 : 0, bounds ? 1 : 0,
            b.Width, b.Height);
}

void App::OnLaunched(LaunchActivatedEventArgs const& e)
{
    XC_LOGI("app", "launched kind=%d previous=%d prelaunch=%d", static_cast<int>(e.Kind()),
            static_cast<int>(e.PreviousExecutionState()), e.PrelaunchActivated() ? 1 : 0);
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
            rootFrame.Navigate(xaml_typename<winrt::XboxCast::MainPage>(), box_value(e.Arguments()));
        }
        Window::Current().Activate();
        StartServices();
    }
}

void App::StartServices()
{
    if (m_servicesStarted) return;
    m_servicesStarted = true;

    // Keep the screen awake while a sender is connected. DisplayRequest is created lazily on the first session, never
    // before Window::Activate (that freezes the splash on Xbox; contract §3).
    if (!m_stateSubscribed)
    {
        m_stateSubscribed = true;
        auto dispatcher = Window::Current().Dispatcher();
        ReceiverHost::Instance().StateChanged([this, dispatcher](ReceiverState s)
        {
            bool active = s == ReceiverState::Connecting || s == ReceiverState::Mirroring;
            dispatcher.RunAsync(CoreDispatcherPriority::Normal, [this, active]() { SetDisplayActive(active); });
        });
    }

    Diag::StartServer();
    // Socket setup and mDNS registration block: keep them off the UI thread.
    Windows::System::Threading::ThreadPool::RunAsync([](auto&&)
    {
        bool ok = ReceiverHost::Instance().Start();
        XC_LOGI("app", "receiver start: %s", ok ? "running" : "not running");
    });
}

void App::StopServices()
{
    if (!m_servicesStarted) return;
    m_servicesStarted = false;
    ReceiverHost::Instance().Stop();
    Diag::StopServer();
    SetDisplayActive(false);
}

void App::SetDisplayActive(bool active)
{
    try
    {
        if (active && !m_displayActive)
        {
            if (!m_displayRequest) m_displayRequest = Windows::System::Display::DisplayRequest();
            m_displayRequest.RequestActive();
            m_displayActive = true;
            XC_LOGI("app", "display request active");
        }
        else if (!active && m_displayActive)
        {
            m_displayRequest.RequestRelease();
            m_displayActive = false;
            XC_LOGI("app", "display request released");
        }
    }
    catch (hresult_error const& ex)
    {
        XC_LOGE("app", "DisplayRequest failed 0x%08x %ls", static_cast<uint32_t>(ex.code()), ex.message().c_str());
    }
}

void App::OnSuspending([[maybe_unused]] IInspectable const& sender, SuspendingEventArgs const& e)
{
    auto deferral = e.SuspendingOperation().GetDeferral();
    XC_LOGI("app", "suspending: stopping the receiver and the dev server");
    StopServices();
    XC_LOGI("app", "suspended");
    deferral.Complete();
}

void App::OnResuming(IInspectable const&, IInspectable const&)
{
    XC_LOGI("app", "resuming: restarting the receiver and the dev server");
    StartServices();
}

void App::OnNavigationFailed(IInspectable const&, NavigationFailedEventArgs const& e)
{
    XC_LOGE("app", "navigation to %ls failed", e.SourcePageType().Name.c_str());
    throw hresult_error(E_FAIL, hstring(L"Failed to load Page ") + e.SourcePageType().Name);
}
}
