#pragma once
// XboxCast.MainPage: idle screen, connecting, mirroring (video + overlay), settings panel, PIN, stats, toasts.
// Spec: docs/design/png/*.png, docs/controller-ux.md, docs/ui.md.

#include "MainPage.g.h"

#include <chrono>
#include <string>
#include <vector>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.Display.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Animation.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

#include "Bridge.h"
#include "Input.h"

namespace winrt::XboxCast::implementation {

struct MainPage : MainPageT<MainPage> {
    MainPage();
    void InitializeComponent();   // two-phase init: named elements are usable from here on

 private:
    using Clock = std::chrono::steady_clock;
    enum class Layer { Idle, Mirroring, Overlay, Settings, Pin };
    enum class RowKind { Edit, Choice, Switch, Delay };

    struct Choice {
        std::wstring value;
        std::wstring label;
    };
    struct Row {
        Windows::UI::Xaml::Controls::Button btn{nullptr};
        Windows::UI::Xaml::Controls::TextBlock val{nullptr};
        Windows::UI::Xaml::Controls::Border track{nullptr};
        Windows::UI::Xaml::Shapes::Ellipse knob{nullptr};
        Windows::UI::Xaml::Media::TranslateTransform knobShift{nullptr};
        std::vector<Windows::UI::Xaml::Controls::FontIcon> chev;
        RowKind kind = RowKind::Choice;
        wchar_t const* key = L"";
        std::wstring def;        // default per contract §3 (string form)
        std::vector<Choice> choices;
        bool intValue = false;   // choice/switch stored as int
        bool restart = false;    // "Applies on next connection"
        wchar_t const* help = L"";
    };

    // setup
    void OnLoaded();
    void BuildRows();
    void BuildBackdrop();
    void WireBridge();
    template <typename F> void Ui(F&& f);

    // input
    void OnPreviewKeyDown(Windows::UI::Xaml::Input::KeyRoutedEventArgs const& e);
    void OnPreviewKeyUp(Windows::UI::Xaml::Input::KeyRoutedEventArgs const& e);
    void PressKey(xcui::Key k, bool repeat);
    void ReleaseKey(xcui::Key k);
    void BTap();
    void OnCommand(std::wstring const& json);
    void HoldTick();
    void PadTick();
    Layer Top() const;
    char const* LayerName() const;

    // session / layers
    void SetState(xcui::SessionState s);
    void OnConnected(std::wstring const& client);
    void OnDisconnected(std::wstring const& reason);
    void OnPin(std::wstring const& pin);
    void ShowOverlay();
    void HideOverlay();
    void OverlayKey(xcui::Key k);
    void MirrorKey(xcui::Key k);
    void FocusOverlay(int index);
    void ActivateOverlayItem();
    void ShowToast(std::wstring const& title, std::wstring const& detail, int ms);
    void UpdateStatsVisibility();
    void RefreshStats();
    void SetHoldProgress(double p);
    void StartBackdrop(bool run);

    // settings panel
    void OpenSettings();
    void CloseSettings();
    void SettingsKey(xcui::Key k);
    void FocusCategory(int index);
    void FocusRow(int index);
    std::vector<Row>& Rows(int category);
    void ActivateRow(Row& r, int dir);   // dir 0 = A, -1 = Left, +1 = Right
    void BeginEditName();
    void CommitName();
    void UpdateRow(Row& r, bool focused);
    void UpdateHelp();

    // model
    void LoadSettings();
    void RefreshAll();
    void PushMedia(bool force);
    void SetViewMode(std::wstring const& mode, bool toast);
    void CycleViewMode(int dir);
    void ZoomStep(double d);
    void SetZoomLive(double z);
    void DelayStep(int d);
    void SetAudioOn(bool on, bool toast);
    void SetShowStats(bool on);
    void ResetView();
    void Disconnect();
    void UpdateNetwork();

    // state
    Windows::UI::Core::CoreDispatcher m_dispatcher{nullptr};
    xcui::SessionState m_state = xcui::SessionState::Idle;
    bool m_overlay = false;
    bool m_settings = false;
    bool m_pin = false;
    bool m_editingName = false;
    std::wstring m_client;
    std::wstring m_lastCoreStats;

    // settings mirror
    std::wstring m_name, m_viewMode;
    double m_zoom = 1.0;
    int m_delay = 0;
    bool m_audioOn = true, m_autoCrop = true, m_showStats = false;
    // last values pushed to media
    std::wstring m_pushedMode;
    double m_pushedZoom = -1;
    int m_pushedDelay = 1 << 30;
    int m_pushedAudio = -1, m_pushedCrop = -1;

    // overlay
    std::vector<Windows::UI::Xaml::Controls::Button> m_ovItems;
    int m_ovFocus = 0;

    // settings
    std::vector<Windows::UI::Xaml::Controls::Button> m_cats;
    std::vector<Row> m_rows[4];
    int m_cat = 0;
    int m_row = -1;   // -1 = focus is on the category list

    // hold B
    bool m_bDown = false;
    bool m_holdFired = false;
    bool m_ringShown = false;
    Clock::time_point m_bDownAt;

    // analog
    Clock::time_point m_lastPad;
    bool m_zoomDirty = false;

    // timers and storyboards
    Windows::UI::Xaml::DispatcherTimer m_overlayTimer;
    Windows::UI::Xaml::DispatcherTimer m_toastTimer;
    Windows::UI::Xaml::DispatcherTimer m_holdTimer;
    Windows::UI::Xaml::DispatcherTimer m_padTimer;
    Windows::UI::Xaml::DispatcherTimer m_statsTimer;
    Windows::UI::Xaml::DispatcherTimer m_hintTimer;
    Windows::UI::Xaml::DispatcherTimer m_settleTimer;    // stops idle animations once the video has faded in
    Windows::UI::Xaml::DispatcherTimer m_holdCmdTimer;   // releases a /cmd {"hold":...} key
    xcui::Key m_holdCmdKey = xcui::Key::None;
    struct BlobAnim {
        Windows::UI::Composition::SpriteVisual visual{nullptr};
        Windows::UI::Composition::ScalarKeyFrameAnimation ax{nullptr};
        Windows::UI::Composition::ScalarKeyFrameAnimation ay{nullptr};
    };
    std::vector<BlobAnim> m_blobs;
    Windows::UI::Composition::SpriteVisual m_vignette{nullptr};
    Windows::UI::Xaml::Media::Animation::Storyboard m_spinner{nullptr};
    bool m_backdropRunning = false;

    Windows::System::Display::DisplayRequest m_displayRequest{nullptr};
    bool m_displayActive = false;
};

}  // namespace winrt::XboxCast::implementation

namespace winrt::XboxCast::factory_implementation {
struct MainPage : MainPageT<MainPage, implementation::MainPage> {};
}  // namespace winrt::XboxCast::factory_implementation
