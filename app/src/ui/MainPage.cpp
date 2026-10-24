#include "pch.h"
#include "MainPage.h"
#if __has_include("MainPage.g.cpp")
#include "MainPage.g.cpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Documents.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Animation.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

#include "Anim.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace winrt::Windows::UI::Xaml::Input;
namespace Hosting = winrt::Windows::UI::Xaml::Hosting;
using xcui::Key;
using xcui::Logf;
using xcui::SessionState;
namespace anim = xcui::anim;

namespace winrt::XboxCast::implementation {

namespace {

constexpr double kZoomMin = 1.0;
constexpr double kZoomMax = 3.0;
constexpr int kDelayMin = -300;
constexpr int kDelayMax = 300;
constexpr int kOverlayHideMs = 4000;
constexpr int kHoldShowMs = 200;
constexpr int kHoldFireMs = 1000;

wchar_t const* const kModes[] = {L"Fit", L"Fill", L"Zoom", L"Auto"};

TimeSpan Ms(int ms) {
    return TimeSpan{std::chrono::milliseconds(ms)};
}

SolidColorBrush MakeBrush(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return SolidColorBrush(ColorHelper::FromArgb(a, r, g, b));
}

bool IsRestartKey(std::wstring const& k) {
    return k == L"deviceName" || k == L"resolution" || k == L"maxFps" || k == L"allowH265" || k == L"pinMode" ||
           k == L"mdnsMode";
}

int ModeIndex(std::wstring const& m) {
    for (int i = 0; i < 4; i++) {
        if (m == kModes[i]) return i;
    }
    return 3;
}

std::wstring Trim(std::wstring s) {
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && iswspace(s[i])) i++;
    return s.substr(i);
}

std::wstring DelayLabel(int ms) {
    wchar_t buf[32];
    if (ms > 0) swprintf(buf, 32, L"+%d ms", ms);
    else swprintf(buf, 32, L"%d ms", ms);
    return buf;
}

std::wstring ZoomLabel(double z) {
    wchar_t buf[32];
    if (std::fabs(z - std::round(z)) < 0.001) swprintf(buf, 32, L"%.1fx", z);
    else swprintf(buf, 32, L"%.2fx", z);
    return buf;
}

}  // namespace

// ============================================================================================ setup

template <typename F>
void MainPage::Ui(F&& f) {
    if (!m_dispatcher || m_dispatcher.HasThreadAccess()) {
        f();
        return;
    }
    m_dispatcher.RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal, std::forward<F>(f));
}


MainPage::MainPage() {}

void MainPage::InitializeComponent() {
    MainPageT::InitializeComponent();
    m_dispatcher = Dispatcher();

    BuildRows();
    BuildBackdrop();
    LoadSettings();
    RefreshAll();

    PreviewKeyDown([this](IInspectable const&, KeyRoutedEventArgs const& e) { HandlePreviewKeyDown(e); });
    PreviewKeyUp([this](IInspectable const&, KeyRoutedEventArgs const& e) { HandlePreviewKeyUp(e); });
    Loaded([this](IInspectable const&, RoutedEventArgs const&) { HandleLoaded(); });

    // B is handled as a key; never let the system treat it as "back".
    Windows::UI::Core::SystemNavigationManager::GetForCurrentView().BackRequested(
        [](IInspectable const&, Windows::UI::Core::BackRequestedEventArgs const& e) { e.Handled(true); });

    m_overlayTimer.Interval(Ms(kOverlayHideMs));
    m_overlayTimer.Tick([this](IInspectable const&, IInspectable const&) { HideOverlay(); });
    m_toastTimer.Tick([this](IInspectable const&, IInspectable const&) {
        m_toastTimer.Stop();
        anim::Fade(Toast(), 0, anim::kCalm);
        anim::To(ToastShift(), L"Y", -24, anim::kCalm);
    });
    m_holdTimer.Interval(Ms(16));
    m_holdTimer.Tick([this](IInspectable const&, IInspectable const&) { HoldTick(); });
    m_padTimer.Interval(Ms(16));
    m_padTimer.Tick([this](IInspectable const&, IInspectable const&) { PadTick(); });
    m_statsTimer.Interval(Ms(1000));
    m_statsTimer.Tick([this](IInspectable const&, IInspectable const&) { RefreshStats(); });
    m_settleTimer.Interval(Ms(anim::kSlow + 50));
    m_settleTimer.Tick([this](IInspectable const&, IInspectable const&) {
        m_settleTimer.Stop();
        if (m_state == SessionState::Mirroring) {
            StartBackdrop(false);
            m_spinner.Stop();
        }
    });
    m_holdCmdTimer.Tick([this](IInspectable const&, IInspectable const&) {
        m_holdCmdTimer.Stop();
        Key k = m_holdCmdKey;
        m_holdCmdKey = Key::None;
        if (k != Key::None) ReleaseKey(k);
    });
    m_hintTimer.Interval(Ms(3000));
    m_hintTimer.Tick([this](IInspectable const&, IInspectable const&) {
        m_hintTimer.Stop();
        anim::Fade(ConnectingHint(), 1, anim::kSlow);
    });

    // Overlay focus order (single row, left to right).
    m_ovItems = {ModeFit(), ModeFill(), ModeZoom(), ModeAuto(), ZoomOutButton(), ZoomInButton(),
                 DelayDownButton(), DelayUpButton(), StatsToggleButton(), DisconnectButton()};
    m_cats = {CatReceiver(), CatPicture(), CatAdvanced(), CatAbout()};

    NameBox().KeyDown([this](IInspectable const&, KeyRoutedEventArgs const& e) {
        if (e.Key() == Windows::System::VirtualKey::Enter) {
            e.Handled(true);
            CommitName();
        }
    });
    // The Xbox keyboard writes the text back as it closes; commit after that has been dispatched.
    Windows::UI::ViewManagement::InputPane::GetForCurrentView().Hiding(
        [weak = get_weak()](IInspectable const&, IInspectable const&) {
            auto self = weak.get();
            if (!self || !self->m_editingName) return;
            self->m_dispatcher.RunAsync(Windows::UI::Core::CoreDispatcherPriority::Low, [weak] {
                if (auto s = weak.get()) s->CommitName();
            });
        });

    Windows::Networking::Connectivity::NetworkInformation::NetworkStatusChanged([weak = get_weak()](IInspectable const&) {
        if (auto self = weak.get()) self->Ui([self] { self->UpdateNetwork(); });
    });

    auto v = Windows::ApplicationModel::Package::Current().Id().Version();
    wchar_t ver[48];
    swprintf(ver, 48, L"%u.%u.%u", v.Major, v.Minor, v.Build);
    VersionText().Text(ver);
    AboutVersion().Text(std::wstring(L"Version ") + ver);
    std::wstring core = xcui::Bridge::Get().CoreVersion();
    AboutCore().Text(core);
    AboutCore().Visibility(core.empty() ? Visibility::Collapsed : Visibility::Visible);

    WireBridge();
    Logf('I', "page init version=%s", winrt::to_string(ver).c_str());
}

void MainPage::WireBridge() {
    auto& b = xcui::Bridge::Get();
    auto weak = get_weak();
    b.onState = [weak](SessionState s) {
        if (auto self = weak.get()) self->Ui([self, s] { self->SetState(s); });
    };
    b.onConnected = [weak](std::wstring const& client) {
        if (auto self = weak.get()) self->Ui([self, client] { self->OnConnected(client); });
    };
    b.onDisconnected = [weak](std::wstring const& reason) {
        if (auto self = weak.get()) self->Ui([self, reason] { self->OnDisconnected(reason); });
    };
    b.onPin = [weak](std::wstring const& pin) {
        if (auto self = weak.get()) self->Ui([self, pin] { self->OnPin(pin); });
    };
    b.onStats = [weak](std::wstring const& json) {
        if (auto self = weak.get()) self->Ui([self, json] { self->m_lastCoreStats = json; });
    };
    b.onCommand = [weak](std::wstring const& json) {
        if (auto self = weak.get()) self->Ui([self, json] { self->OnCommand(json); });
    };
    b.onSettingChanged = [weak](std::wstring const& key) {
        if (auto self = weak.get())
            self->Ui([self, key] {
                self->LoadSettings();
                self->RefreshAll();
                self->PushMedia(false);
                if (IsRestartKey(key)) xcui::Bridge::Get().RequestRestart();
            });
    };
}

void MainPage::HandleLoaded() {
    auto& b = xcui::Bridge::Get();
    b.AttachVideo(VideoPanel());
    b.Start();
    PushMedia(true);
    UpdateNetwork();
    SetState(b.State());
    StartBackdrop(m_state == SessionState::Idle);
    Focus(FocusState::Programmatic);
}

void MainPage::BuildBackdrop() {
    // Three soft radial blobs drifting slowly (47-67 s loops) + a vignette, as Composition visuals: the animations
    // run on the compositor thread and all stop while mirroring (StartBackdrop(false)).
    using namespace Windows::UI::Composition;
    m_spinner = anim::Spin(SpinnerRotate(), L"Angle", 900);
    if (!Windows::Foundation::Metadata::ApiInformation::IsTypePresent(L"Windows.UI.Composition.CompositionRadialGradientBrush")) {
        Logf('W', "backdrop: radial gradient brush not available, plain background");
        return;
    }
    auto compositor = Hosting::ElementCompositionPreview::GetElementVisual(Backdrop()).Compositor();
    auto root = compositor.CreateContainerVisual();
    root.RelativeSizeAdjustment({1.0f, 1.0f});

    struct Blob { float size, left, top; uint8_t r, g, b, a; float dx, dy; int tx, ty; };
    Blob const blobs[] = {
        {1500, -420, -620, 52, 76, 190, 107, 160, 110, 47000, 61000},
        {1300, 980, 180, 104, 58, 168, 87, -140, -90, 53000, 43000},
        {1100, 260, 520, 24, 104, 128, 66, 120, -120, 67000, 57000},
    };
    auto ease = compositor.CreateCubicBezierEasingFunction({0.37f, 0.0f}, {0.63f, 1.0f});
    auto drift = [&](float from, float to, int ms) {
        auto a = compositor.CreateScalarKeyFrameAnimation();
        a.InsertKeyFrame(0.0f, from);
        a.InsertKeyFrame(0.5f, to, ease);
        a.InsertKeyFrame(1.0f, from, ease);
        a.Duration(Ms(ms));
        a.IterationBehavior(AnimationIterationBehavior::Forever);
        return a;
    };
    for (auto const& bl : blobs) {
        auto brush = compositor.CreateRadialGradientBrush();
        brush.ColorStops().Append(compositor.CreateColorGradientStop(0.0f, ColorHelper::FromArgb(bl.a, bl.r, bl.g, bl.b)));
        brush.ColorStops().Append(compositor.CreateColorGradientStop(1.0f, ColorHelper::FromArgb(0, bl.r, bl.g, bl.b)));
        auto v = compositor.CreateSpriteVisual();
        v.Size({bl.size, bl.size});
        v.Offset({bl.left, bl.top, 0.0f});
        v.Brush(brush);
        root.Children().InsertAtTop(v);
        BlobAnim ba;
        ba.visual = v;
        ba.ax = drift(bl.left - bl.dx, bl.left + bl.dx, bl.tx);
        ba.ay = drift(bl.top - bl.dy, bl.top + bl.dy, bl.ty);
        m_blobs.push_back(ba);
    }
    auto vb = compositor.CreateRadialGradientBrush();
    vb.EllipseCenter({0.5f, 0.45f});
    vb.EllipseRadius({0.72f, 0.72f});
    vb.ColorStops().Append(compositor.CreateColorGradientStop(0.45f, ColorHelper::FromArgb(0, 7, 7, 11)));
    vb.ColorStops().Append(compositor.CreateColorGradientStop(1.0f, ColorHelper::FromArgb(191, 7, 7, 11)));
    m_vignette = compositor.CreateSpriteVisual();
    m_vignette.RelativeSizeAdjustment({1.0f, 1.0f});
    m_vignette.Brush(vb);
    root.Children().InsertAtTop(m_vignette);
    Hosting::ElementCompositionPreview::SetElementChildVisual(Backdrop(), root);
}

void MainPage::StartBackdrop(bool run) {
    if (run == m_backdropRunning) return;
    m_backdropRunning = run;
    for (auto& b : m_blobs) {
        if (run) {
            b.visual.StartAnimation(L"Offset.X", b.ax);
            b.visual.StartAnimation(L"Offset.Y", b.ay);
        } else {
            b.visual.StopAnimation(L"Offset.X");
            b.visual.StopAnimation(L"Offset.Y");
        }
    }
    Logf('D', "backdrop %s", run ? "on" : "off");
}

void MainPage::BuildRows() {
    auto setup = [](Row& r, Button const& btn, TextBlock const& val, RowKind kind, wchar_t const* key, bool restart,
                    wchar_t const* help) {
        r.btn = btn;
        r.val = val;
        r.kind = kind;
        r.key = key;
        r.restart = restart;
        r.help = help;
        // Collect the chevrons (FontIcons) inside the row's value StackPanel.
        if (auto grid = btn.Content().try_as<Grid>()) {
            for (auto const& child : grid.Children()) {
                if (auto sp = child.try_as<StackPanel>()) {
                    for (auto const& c : sp.Children()) {
                        if (auto icon = c.try_as<FontIcon>()) r.chev.push_back(icon);
                    }
                }
            }
        }
    };
    auto sw = [](Row& r, Border const& track, Shapes::Ellipse const& knob) {
        r.track = track;
        r.knob = knob;
        r.knobShift = TranslateTransform();
        knob.RenderTransform(r.knobShift);
    };

    auto& rc = m_rows[0];
    rc.resize(5);
    setup(rc[0], RowName(), ValName(), RowKind::Edit, L"deviceName", true,
          L"The name your iPhone shows in Screen Mirroring.");
    setup(rc[1], RowResolution(), ValResolution(), RowKind::Choice, L"resolution", true,
          L"The largest picture your iPhone sends. Higher looks sharper on a 4K TV and uses more bandwidth.");
    rc[1].choices = {{L"1080p", L"1080p"}, {L"1440p", L"1440p"}, {L"2160p", L"4K"}};
    setup(rc[2], RowFps(), ValFps(), RowKind::Choice, L"maxFps", true,
          L"60 fps is smoother. 30 fps needs less bandwidth on a weak Wi-Fi.");
    rc[2].choices = {{L"30", L"30 fps"}, {L"60", L"60 fps"}};
    rc[2].intValue = true;
    setup(rc[3], RowH265(), ValH265(), RowKind::Switch, L"allowH265", true,
          L"Lets newer iPhones send H.265, which looks better at the same bandwidth. Turn off if the picture stutters.");
    sw(rc[3], SwH265(), KnobH265());
    setup(rc[4], RowPin(), ValPin(), RowKind::Switch, L"pinMode", true,
          L"Shows a code on the TV that the iPhone must enter before it can mirror.");
    sw(rc[4], SwPin(), KnobPin());
    rc[4].intValue = true;

    auto& rp = m_rows[1];
    rp.resize(5);
    setup(rp[0], RowViewMode(), ValViewMode(), RowKind::Choice, L"viewMode", false,
          L"Fit shows the whole picture. Fill covers the screen. Zoom lets you zoom and pan. Auto removes black bars, "
          L"then fills the screen if little is cut off.");
    rp[0].choices = {{L"Fit", L"Fit"}, {L"Fill", L"Fill"}, {L"Zoom", L"Zoom"}, {L"Auto", L"Auto"}};
    setup(rp[1], RowAutoCrop(), ValAutoCrop(), RowKind::Switch, L"autoCrop", false,
          L"Finds black bars that apps add around the picture and crops them away.");
    sw(rp[1], SwAutoCrop(), KnobAutoCrop());
    setup(rp[2], RowAudio(), ValAudio(), RowKind::Switch, L"audioEnabled", false,
          L"Plays the iPhone's sound on the TV. X turns it on or off while mirroring.");
    sw(rp[2], SwAudio(), KnobAudio());
    setup(rp[3], RowDelay(), ValDelay(), RowKind::Delay, L"audioOffsetMs", false,
          L"Shifts the sound against the picture. Use it if voices run ahead of or behind lips on your TV. A resets it.");
    setup(rp[4], RowStats(), ValStats(), RowKind::Switch, L"showStats", false,
          L"Shows latency, frame rate and bitrate while mirroring. The View button does the same.");
    sw(rp[4], SwStats(), KnobStats());

    rc[0].def = L"Xbox";
    rc[1].def = L"1080p";
    rc[2].def = L"60";
    rc[3].def = L"1";
    rc[4].def = L"0";
    rp[0].def = L"Auto";
    rp[1].def = L"1";
    rp[2].def = L"1";
    rp[3].def = L"0";
    rp[4].def = L"0";

    auto& ra = m_rows[2];
    ra.resize(1);
    setup(ra[0], RowMdns(), ValMdns(), RowKind::Choice, L"mdnsMode", true,
          L"How this screen announces itself on the network. Built-in is the default; System is experimental.");
    ra[0].choices = {{L"builtin", L"Built-in"}, {L"os", L"System"}};
    ra[0].def = L"builtin";

    TranslateTransform statsKnob;
    StatsSwitchKnob().RenderTransform(statsKnob);
}

// ============================================================================================ model

void MainPage::LoadSettings() {
    auto& b = xcui::Bridge::Get();
    m_name = b.GetString(L"deviceName", L"Xbox");
    m_viewMode = b.GetString(L"viewMode", L"Auto");
    m_viewMode = kModes[ModeIndex(m_viewMode)];
    m_zoom = std::clamp(b.GetDouble(L"zoom", 1.0), kZoomMin, kZoomMax);
    m_delay = std::clamp(b.GetInt(L"audioOffsetMs", 0), kDelayMin, kDelayMax);
    m_audioOn = b.GetBool(L"audioEnabled", true);
    m_autoCrop = b.GetBool(L"autoCrop", true);
    m_showStats = b.GetBool(L"showStats", false);
}

void MainPage::RefreshAll() {
    IdleNameText().Text(m_name);
    IdleHowName().Text(m_name);

    int mi = ModeIndex(m_viewMode);
    Shapes::Ellipse dots[] = {ModeFitDot(), ModeFillDot(), ModeZoomDot(), ModeAutoDot()};
    Button pills[] = {ModeFit(), ModeFill(), ModeZoom(), ModeAuto()};
    for (int i = 0; i < 4; i++) {
        dots[i].Visibility(i == mi ? Visibility::Visible : Visibility::Collapsed);
        pills[i].Background(i == mi ? MakeBrush(0x38, 0x8E, 0xA8, 0xFF) : MakeBrush(0, 0, 0, 0));
    }
    ZoomText().Text(ZoomLabel(m_zoom));
    ZoomOutButton().Opacity(m_zoom <= kZoomMin + 0.001 ? 0.35 : 1.0);
    ZoomInButton().Opacity(m_zoom >= kZoomMax - 0.001 ? 0.35 : 1.0);
    DelayText().Text(DelayLabel(m_delay));
    DelayDownButton().Opacity(m_delay <= kDelayMin ? 0.35 : 1.0);
    DelayUpButton().Opacity(m_delay >= kDelayMax ? 0.35 : 1.0);

    bool statsFocused = m_overlay && m_ovFocus == 8;
    StatsSwitchTrack().Background(m_showStats ? (statsFocused ? MakeBrush(255, 0x4D, 0x66, 0xD6) : MakeBrush(255, 0x8E, 0xA8, 0xFF))
                                              : (statsFocused ? MakeBrush(0x40, 0x0B, 0x0B, 0x10) : MakeBrush(0x42, 255, 255, 255)));
    if (auto t = StatsSwitchKnob().RenderTransform().try_as<TranslateTransform>()) {
        anim::To(t, L"X", m_showStats ? 26 : 0, 200);
    }

    for (int c = 0; c < 3; c++) {
        for (size_t i = 0; i < m_rows[c].size(); i++) {
            UpdateRow(m_rows[c][i], m_settings && c == m_cat && static_cast<int>(i) == m_row);
        }
    }
    UpdateStatsVisibility();
}

void MainPage::PushMedia(bool force) {
    auto& b = xcui::Bridge::Get();
    if (force || m_pushedMode != m_viewMode) {
        b.SetViewMode(m_viewMode);
        m_pushedMode = m_viewMode;
    }
    if (force || std::fabs(m_pushedZoom - m_zoom) > 0.0001) {
        b.SetZoom(static_cast<float>(m_zoom));
        m_pushedZoom = m_zoom;
    }
    if (force || m_pushedDelay != m_delay) {
        b.SetAudioOffsetMs(m_delay);
        m_pushedDelay = m_delay;
    }
    if (force || m_pushedAudio != static_cast<int>(m_audioOn)) {
        b.SetAudioEnabled(m_audioOn);
        m_pushedAudio = m_audioOn;
    }
    if (force || m_pushedCrop != static_cast<int>(m_autoCrop)) {
        b.SetAutoCrop(m_autoCrop);
        m_pushedCrop = m_autoCrop;
    }
}

void MainPage::SetViewMode(std::wstring const& mode, bool toast) {
    std::wstring m = kModes[ModeIndex(mode)];
    if (m != m_viewMode) xcui::Bridge::Get().SetString(L"viewMode", m);
    m_viewMode = m;
    RefreshAll();
    PushMedia(false);
    if (toast && !m_overlay) ShowToast(m, L"", 1600);
}

void MainPage::CycleViewMode(int dir) {
    int i = (ModeIndex(m_viewMode) + dir + 4) % 4;
    SetViewMode(kModes[i], true);
}

void MainPage::ZoomStep(double d) {
    if (m_viewMode != L"Zoom") SetViewMode(L"Zoom", true);
    double z = std::clamp(std::round((m_zoom + d) * 4.0) / 4.0, kZoomMin, kZoomMax);
    if (std::fabs(z - m_zoom) < 0.0001) return;
    m_zoom = z;
    xcui::Bridge::Get().SetDouble(L"zoom", m_zoom);
    RefreshAll();
    PushMedia(false);
}

void MainPage::SetZoomLive(double z) {
    z = std::clamp(z, kZoomMin, kZoomMax);
    if (std::fabs(z - m_zoom) < 0.0005) return;
    if (m_viewMode != L"Zoom") SetViewMode(L"Zoom", true);
    m_zoom = z;
    m_zoomDirty = true;
    ZoomText().Text(ZoomLabel(m_zoom));
    PushMedia(false);
}

void MainPage::DelayStep(int d) {
    int v = std::clamp(m_delay + d, kDelayMin, kDelayMax);
    if (v == m_delay) return;
    m_delay = v;
    xcui::Bridge::Get().SetInt(L"audioOffsetMs", m_delay);
    RefreshAll();
    PushMedia(false);
}

void MainPage::SetAudioOn(bool on, bool toast) {
    m_audioOn = on;
    xcui::Bridge::Get().SetBool(L"audioEnabled", on);
    RefreshAll();
    PushMedia(false);
    if (toast) ShowToast(on ? L"Sound on" : L"Sound off", L"", 1600);
}

void MainPage::SetShowStats(bool on) {
    m_showStats = on;
    xcui::Bridge::Get().SetBool(L"showStats", on);
    RefreshAll();
    if (m_state == SessionState::Idle && !m_settings) {
        ShowToast(on ? L"Stats on" : L"Stats off", on ? L"Shown while mirroring" : L"", 1600);
    }
}

void MainPage::ResetView() {
    xcui::Bridge::Get().ResetPan();
    if (std::fabs(m_zoom - 1.0) > 0.0001) {
        m_zoom = 1.0;
        xcui::Bridge::Get().SetDouble(L"zoom", 1.0);
    }
    RefreshAll();
    PushMedia(false);
    if (!m_overlay) ShowToast(L"View reset", L"", 1600);
}

void MainPage::Disconnect() {
    Logf('I', "disconnect state=%d", static_cast<int>(m_state));
    xcui::Bridge::Get().Disconnect();
}

void MainPage::UpdateNetwork() {
    using namespace Windows::Networking::Connectivity;
    std::wstring best;
    try {
        auto profile = NetworkInformation::GetInternetConnectionProfile();
        guid adapter{};
        bool haveAdapter = false;
        if (profile && profile.NetworkAdapter()) {
            adapter = profile.NetworkAdapter().NetworkAdapterId();
            haveAdapter = true;
        }
        for (auto const& h : NetworkInformation::GetHostNames()) {
            if (h.Type() != Windows::Networking::HostNameType::Ipv4) continue;
            auto info = h.IPInformation();
            if (!info) continue;
            std::wstring ip = h.CanonicalName().c_str();
            if (ip.rfind(L"169.254.", 0) == 0 || ip.rfind(L"127.", 0) == 0) continue;
            if (haveAdapter && info.NetworkAdapter() && info.NetworkAdapter().NetworkAdapterId() == adapter) {
                best = ip;
                break;
            }
            if (best.empty()) best = ip;
        }
    } catch (...) {
        Logf('W', "network lookup failed");
    }
    NetworkText().Text(best.empty() ? L"Not connected" : best);
    Logf('I', "network ip=%s", best.empty() ? "none" : winrt::to_string(best).c_str());
}

// ============================================================================================ session / layers

MainPage::Layer MainPage::Top() const {
    if (m_pin) return Layer::Pin;
    if (m_settings) return Layer::Settings;
    if (m_overlay) return Layer::Overlay;
    if (m_state != SessionState::Idle) return Layer::Mirroring;
    return Layer::Idle;
}

char const* MainPage::LayerName() const {
    switch (Top()) {
    case Layer::Pin: return "pin";
    case Layer::Settings: return "settings";
    case Layer::Overlay: return "overlay";
    case Layer::Mirroring: return m_state == SessionState::Connecting ? "connecting" : "mirroring";
    default: return "idle";
    }
}

void MainPage::SetState(SessionState s) {
    if (s == m_state) return;
    SessionState prev = m_state;
    m_state = s;
    Logf('I', "state %d -> %d", static_cast<int>(prev), static_cast<int>(s));

    switch (s) {
    case SessionState::Connecting:
        ConnectingTitle().Text(m_client.empty() ? std::wstring(L"Connecting") : L"Connecting to " + m_client);
        IdleLayer().Visibility(Visibility::Visible);
        anim::Fade(IdleLayer(), 1, anim::kCalm);
        anim::Fade(IdleContent(), 0, anim::kNormal);
        anim::Fade(IdleHints(), 0, anim::kNormal);
        anim::Fade(ConnectingContent(), 1, anim::kCalm);
        anim::Fade(ConnectingHint(), 0, 1);
        m_hintTimer.Start();
        m_spinner.Begin();
        StartBackdrop(true);
        break;
    case SessionState::Mirroring:
        if (!m_displayRequest) {
            try {
                m_displayRequest = Windows::System::Display::DisplayRequest();
            } catch (...) {
                Logf('W', "DisplayRequest unavailable");
            }
        }
        if (m_displayRequest && !m_displayActive) {
            try {
                m_displayRequest.RequestActive();
                m_displayActive = true;
            } catch (...) {
                Logf('W', "DisplayRequest.RequestActive failed");
            }
        }
        m_hintTimer.Stop();
        anim::Fade(IdleLayer(), 0, anim::kSlow);
        m_padTimer.Start();
        // Backdrop + spinner stop once the fade has finished (keeps the GPU idle behind the video).
        m_settleTimer.Start();
        break;
    case SessionState::Idle:
    default:
        m_hintTimer.Stop();
        m_padTimer.Stop();
        m_spinner.Stop();
        if (m_overlay) HideOverlay();
        if (m_pin) OnPin(L"");
        if (m_bDown) {
            m_bDown = false;
            m_holdTimer.Stop();
            SetHoldProgress(-1);
        }
        if (m_displayRequest && m_displayActive) {
            try {
                m_displayRequest.RequestRelease();
            } catch (...) {
            }
            m_displayActive = false;
        }
        xcui::Bridge::Get().StopVideo();
        StartBackdrop(true);
        IdleLayer().Visibility(Visibility::Visible);
        anim::Fade(IdleLayer(), 1, anim::kCalm);
        anim::Fade(ConnectingContent(), 0, anim::kNormal);
        anim::Fade(IdleContent(), 1, anim::kCalm);
        anim::Fade(IdleHints(), 1, anim::kCalm);
        m_client.clear();
        xcui::Bridge::Get().OnSessionEnded();
        break;
    }
    UpdateStatsVisibility();
    if (!m_settings && !m_overlay) Focus(FocusState::Programmatic);
}

void MainPage::OnConnected(std::wstring const& client) {
    m_client = client.empty() ? L"iPhone" : client;
    ClientNameText().Text(m_client);
    Logf('I', "connected client=%s", winrt::to_string(m_client).c_str());
    if (m_state == SessionState::Connecting) ConnectingTitle().Text(L"Connecting to " + m_client);
    // Pan always restarts centred for a new session.
    xcui::Bridge::Get().ResetPan();
}

void MainPage::OnDisconnected(std::wstring const& reason) {
    std::wstring who = m_client.empty() ? L"iPhone" : m_client;
    Logf('I', "disconnected reason=%s", winrt::to_string(reason).c_str());
    SetState(SessionState::Idle);
    ShowToast(L"Mirroring ended", who + L" disconnected", 4000);
}

void MainPage::OnPin(std::wstring const& pin) {
    auto digits = PinDigits();
    if (pin.empty()) {
        if (!m_pin) return;
        m_pin = false;
        anim::Fade(PinLayer(), 0, anim::kCalm);
        Logf('I', "pin hidden");
        if (!m_settings && !m_overlay) Focus(FocusState::Programmatic);
        return;
    }
    digits.Children().Clear();
    for (wchar_t ch : pin) {
        Border cell;
        cell.Width(168);
        cell.Height(212);
        cell.CornerRadius(CornerRadiusHelper::FromUniformRadius(32));
        cell.Background(MakeBrush(0x14, 255, 255, 255));
        cell.Margin(ThicknessHelper::FromLengths(14, 0, 14, 0));
        TextBlock t;
        t.Text(std::wstring(1, ch));
        t.FontSize(132);
        t.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        t.HorizontalAlignment(HorizontalAlignment::Center);
        t.VerticalAlignment(VerticalAlignment::Center);
        t.Foreground(MakeBrush(255, 0xF5, 0xF5, 0xF7));
        cell.Child(t);
        digits.Children().Append(cell);
    }
    if (m_overlay) HideOverlay();
    m_pin = true;
    anim::Fade(PinLayer(), 1, anim::kCalm);
    Logf('I', "pin shown digits=%d", static_cast<int>(pin.size()));
}

void MainPage::ShowOverlay() {
    if (m_state == SessionState::Idle || m_pin || m_settings) return;
    if (!m_overlay) {
        m_overlay = true;
        OverlayShift().Y(30);
        anim::Fade(OverlayLayer(), 1, anim::kNormal);
        anim::To(OverlayShift(), L"Y", 0, anim::kCalm);
        FocusOverlay(ModeIndex(m_viewMode));
        Logf('D', "overlay show");
    }
    m_overlayTimer.Stop();
    m_overlayTimer.Start();
}

void MainPage::HideOverlay() {
    m_overlayTimer.Stop();
    if (!m_overlay) return;
    m_overlay = false;
    anim::Fade(OverlayLayer(), 0, anim::kNormal);
    anim::To(OverlayShift(), L"Y", 30, anim::kNormal);
    RefreshAll();
    if (!m_settings && !m_pin) Focus(FocusState::Programmatic);
    Logf('D', "overlay hide");
}

void MainPage::FocusOverlay(int index) {
    m_ovFocus = std::clamp(index, 0, static_cast<int>(m_ovItems.size()) - 1);
    m_ovItems[m_ovFocus].Focus(FocusState::Keyboard);
    RefreshAll();
}

void MainPage::ActivateOverlayItem() {
    switch (m_ovFocus) {
    case 0: case 1: case 2: case 3: SetViewMode(kModes[m_ovFocus], false); break;
    case 4: ZoomStep(-0.25); break;
    case 5: ZoomStep(0.25); break;
    case 6: DelayStep(-20); break;
    case 7: DelayStep(20); break;
    case 8: SetShowStats(!m_showStats); break;
    case 9: Disconnect(); break;
    default: break;
    }
}

void MainPage::OverlayKey(Key k) {
    m_overlayTimer.Stop();
    m_overlayTimer.Start();
    switch (k) {
    case Key::Left: FocusOverlay(m_ovFocus - 1); break;
    case Key::Right: FocusOverlay(m_ovFocus + 1); break;
    case Key::A: ActivateOverlayItem(); break;
    case Key::X: SetAudioOn(!m_audioOn, true); break;
    case Key::Y: ResetView(); break;
    case Key::LB: CycleViewMode(-1); break;
    case Key::RB: CycleViewMode(1); break;
    case Key::LT: ZoomStep(-0.25); break;
    case Key::RT: ZoomStep(0.25); break;
    default: break;
    }
}

void MainPage::MirrorKey(Key k) {
    bool zoomed = m_viewMode == L"Zoom" && m_zoom > kZoomMin + 0.001;
    switch (k) {
    case Key::A: ShowOverlay(); break;
    case Key::X: SetAudioOn(!m_audioOn, true); break;
    case Key::Y: ResetView(); break;
    case Key::LB: CycleViewMode(-1); break;
    case Key::RB: CycleViewMode(1); break;
    case Key::LT: ZoomStep(-0.25); break;
    case Key::RT: ZoomStep(0.25); break;
    case Key::Up: case Key::Down: case Key::Left: case Key::Right:
        if (zoomed) {
            float dx = k == Key::Left ? -0.05f : k == Key::Right ? 0.05f : 0.f;
            float dy = k == Key::Up ? -0.05f : k == Key::Down ? 0.05f : 0.f;
            xcui::Bridge::Get().Pan(dx, dy);
        } else {
            ShowOverlay();
        }
        break;
    default: break;
    }
}

void MainPage::ShowToast(std::wstring const& title, std::wstring const& detail, int ms) {
    ToastTitle().Text(title);
    ToastDetail().Text(detail);
    ToastDetail().Visibility(detail.empty() ? Visibility::Collapsed : Visibility::Visible);
    if (Toast().Visibility() != Visibility::Visible || Toast().Opacity() < 0.5) ToastShift().Y(-24);
    anim::Fade(Toast(), 1, anim::kCalm);
    anim::To(ToastShift(), L"Y", 0, anim::kCalm);
    m_toastTimer.Stop();
    m_toastTimer.Interval(Ms(ms));
    m_toastTimer.Start();
    Logf('D', "toast %s", winrt::to_string(title).c_str());
}

void MainPage::UpdateStatsVisibility() {
    bool show = m_showStats && m_state == SessionState::Mirroring;
    bool shown = StatsPanel().Visibility() == Visibility::Visible && StatsPanel().Opacity() > 0.01;
    if (show && !shown) {
        RefreshStats();
        anim::Fade(StatsPanel(), 1, anim::kCalm);
        m_statsTimer.Start();
    } else if (!show && shown) {
        anim::Fade(StatsPanel(), 0, anim::kNormal);
        m_statsTimer.Stop();
    }
}

void MainPage::RefreshStats() {
    auto rows = xcui::Bridge::Get().StatsRows(m_lastCoreStats);
    auto grid = StatsGrid();
    grid.Children().Clear();
    grid.RowDefinitions().Clear();
    if (rows.empty()) rows.push_back({L"Waiting for data", L"", false});
    int i = 0;
    for (auto const& r : rows) {
        grid.RowDefinitions().Append(RowDefinition());
        TextBlock l;
        l.Text(r.label);
        l.FontSize(27);
        l.FontWeight(Windows::UI::Text::FontWeights::Medium());
        l.Foreground(MakeBrush(0xA3, 0xEB, 0xEB, 0xF5));
        l.Margin(ThicknessHelper::FromLengths(0, 7, 24, 7));
        Grid::SetRow(l, i);
        TextBlock v;
        v.Text(r.value);
        v.FontSize(27);
        v.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        v.TextAlignment(TextAlignment::Right);
        v.Foreground(r.good ? MakeBrush(255, 0x7E, 0xE2, 0xA8) : MakeBrush(255, 0xF5, 0xF5, 0xF7));
        v.Margin(ThicknessHelper::FromLengths(0, 7, 0, 7));
        Grid::SetRow(v, i);
        Grid::SetColumn(v, 1);
        grid.Children().Append(l);
        grid.Children().Append(v);
        i++;
    }
}

void MainPage::SetHoldProgress(double p) {
    if (p < 0) {
        if (m_ringShown) anim::Fade(HoldRing(), 0, anim::kFast);
        m_ringShown = false;
        return;
    }
    if (!m_ringShown) {
        m_ringShown = true;
        anim::Fade(HoldRing(), 1, anim::kFast);
    }
    p = std::clamp(p, 0.0, 0.9999);
    // Arc on a 96x96 box, radius 39 (6 px inset + half the 6 px stroke), starting at 12 o'clock, clockwise.
    double const c = 48, r = 39, pi = 3.14159265358979;
    double a = p * 2 * pi;
    PathFigure fig;
    fig.StartPoint(Point(static_cast<float>(c), static_cast<float>(c - r)));
    fig.IsClosed(false);
    ArcSegment arc;
    arc.Point(Point(static_cast<float>(c + r * std::sin(a)), static_cast<float>(c - r * std::cos(a))));
    arc.Size(Size(static_cast<float>(r), static_cast<float>(r)));
    arc.IsLargeArc(p > 0.5);
    arc.SweepDirection(SweepDirection::Clockwise);
    fig.Segments().Append(arc);
    PathGeometry geo;
    geo.Figures().Append(fig);
    HoldArc().Data(geo);
}

// ============================================================================================ input

void MainPage::HandlePreviewKeyDown(KeyRoutedEventArgs const& e) {
    auto vk = e.Key();
    if (m_editingName) {
        using Windows::System::VirtualKey;
        if (vk == VirtualKey::GamepadB || vk == VirtualKey::Escape || vk == VirtualKey::GamepadDPadUp ||
            vk == VirtualKey::GamepadDPadDown || vk == VirtualKey::GamepadMenu) {
            e.Handled(true);
            if (!e.KeyStatus().WasKeyDown) CommitName();
        }
        return;   // everything else goes to the TextBox (A opens the keyboard)
    }
    Key k = xcui::KeyFromVirtualKey(vk);
    if (k == Key::None) {
        if (xcui::IsSwallowedVirtualKey(vk)) e.Handled(true);
        return;
    }
    e.Handled(true);
    PressKey(k, e.KeyStatus().WasKeyDown);
}

void MainPage::HandlePreviewKeyUp(KeyRoutedEventArgs const& e) {
    if (m_editingName) return;
    Key k = xcui::KeyFromVirtualKey(e.Key());
    if (k == Key::None) return;
    e.Handled(true);
    ReleaseKey(k);
}

void MainPage::PressKey(Key k, bool repeat) {
    if (!repeat || xcui::IsDirection(k)) {
        Logf('I', "key %s down%s layer=%s", winrt::to_string(xcui::KeyName(k)).c_str(), repeat ? " repeat" : "",
             LayerName());
    }
    if (k == Key::B) {
        if (repeat) return;
        if (m_state != SessionState::Idle) {
            // Fires on release (tap) or after 1 s (hold to disconnect).
            m_bDown = true;
            m_holdFired = false;
            m_bDownAt = Clock::now();
            m_holdTimer.Start();
            return;
        }
        BTap();
        return;
    }
    if (repeat && !xcui::IsDirection(k)) return;

    if (k == Key::View) {
        if (Top() != Layer::Pin) SetShowStats(!m_showStats);
        if (m_overlay) {
            m_overlayTimer.Stop();
            m_overlayTimer.Start();
        }
        return;
    }
    if (k == Key::Menu) {
        if (Top() == Layer::Pin) return;
        if (m_settings) CloseSettings();
        else OpenSettings();
        return;
    }
    switch (Top()) {
    case Layer::Settings: SettingsKey(k); break;
    case Layer::Overlay: OverlayKey(k); break;
    case Layer::Mirroring: MirrorKey(k); break;
    default: break;
    }
}

void MainPage::ReleaseKey(Key k) {
    if (k != Key::B || !m_bDown) return;
    m_bDown = false;
    m_holdTimer.Stop();
    SetHoldProgress(-1);
    if (!m_holdFired) BTap();
}

void MainPage::HoldTick() {
    if (!m_bDown) {
        m_holdTimer.Stop();
        return;
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - m_bDownAt).count();
    if (ms >= kHoldFireMs) {
        m_holdFired = true;
        m_holdTimer.Stop();
        SetHoldProgress(0.9999);
        SetHoldProgress(-1);
        Logf('I', "hold B -> disconnect");
        Disconnect();
        return;
    }
    if (ms >= kHoldShowMs) SetHoldProgress(static_cast<double>(ms - kHoldShowMs) / (kHoldFireMs - kHoldShowMs));
}

void MainPage::BTap() {
    switch (Top()) {
    case Layer::Pin: Disconnect(); break;
    case Layer::Settings:
        if (m_row >= 0) FocusCategory(m_cat);
        else CloseSettings();
        break;
    case Layer::Overlay: HideOverlay(); break;
    default: break;
    }
}

void MainPage::PadTick() {
    if (m_state != SessionState::Mirroring || m_settings || m_pin) return;
    auto now = Clock::now();
    double dt = std::chrono::duration<double>(now - m_lastPad).count();
    m_lastPad = now;
    if (dt <= 0 || dt > 0.1) dt = 0.016;

    double lt = 0, rt = 0, rx = 0, ry = 0;
    auto pads = Windows::Gaming::Input::Gamepad::Gamepads();
    for (auto const& pad : pads) {
        auto r = pad.GetCurrentReading();
        lt = (std::max)(lt, r.LeftTrigger);
        rt = (std::max)(rt, r.RightTrigger);
        if (std::fabs(r.RightThumbstickX) > std::fabs(rx)) rx = r.RightThumbstickX;
        if (std::fabs(r.RightThumbstickY) > std::fabs(ry)) ry = r.RightThumbstickY;
    }
    auto dead = [](double v, double d) { return std::fabs(v) < d ? 0.0 : (v - std::copysign(d, v)) / (1.0 - d); };
    double zoomRate = dead(rt, 0.08) - dead(lt, 0.08);
    if (zoomRate != 0) {
        SetZoomLive(m_zoom + zoomRate * 1.2 * dt);   // full pressure: 1.2x per second
        if (m_overlay) {
            m_overlayTimer.Stop();
            m_overlayTimer.Start();
        }
    } else if (m_zoomDirty) {
        m_zoomDirty = false;
        xcui::Bridge::Get().SetDouble(L"zoom", m_zoom);
        RefreshAll();
    }
    if (!m_overlay && m_viewMode == L"Zoom" && m_zoom > kZoomMin + 0.001) {
        double px = dead(rx, 0.15), py = dead(ry, 0.15);
        if (px != 0 || py != 0) {
            // Full deflection crosses the view in ~1.25 s at 1x; slower when zoomed further in.
            double s = 0.8 * dt / m_zoom;
            xcui::Bridge::Get().Pan(static_cast<float>(px * s), static_cast<float>(-py * s));
        }
    }
}

void MainPage::OnCommand(std::wstring const& json) {
    using namespace Windows::Data::Json;
    JsonObject o{nullptr};
    if (!JsonObject::TryParse(json, o)) {
        Logf('W', "cmd: bad json %s", winrt::to_string(json).c_str());
        return;
    }
    Logf('I', "cmd %s", winrt::to_string(json).c_str());
    if (o.HasKey(L"key")) {
        Key k = xcui::KeyFromName(o.GetNamedString(L"key", L"").c_str());
        if (k == Key::None) {
            Logf('W', "cmd: unknown key");
        } else {
            PressKey(k, false);
            ReleaseKey(k);
        }
    }
    if (o.HasKey(L"hold")) {
        Key k = xcui::KeyFromName(o.GetNamedString(L"hold", L"").c_str());
        int ms = std::clamp(static_cast<int>(o.GetNamedNumber(L"ms", 1200)), 50, 5000);
        if (k != Key::None) {
            if (m_holdCmdKey != Key::None) ReleaseKey(m_holdCmdKey);
            m_holdCmdKey = k;
            PressKey(k, false);
            m_holdCmdTimer.Interval(Ms(ms));
            m_holdCmdTimer.Start();
        }
    }
    if (o.HasKey(L"viewMode")) SetViewMode(o.GetNamedString(L"viewMode", L"Auto").c_str(), true);
    if (o.HasKey(L"zoom")) {
        if (m_viewMode != L"Zoom") SetViewMode(L"Zoom", false);
        m_zoom = std::clamp(o.GetNamedNumber(L"zoom", 1.0), kZoomMin, kZoomMax);
        xcui::Bridge::Get().SetDouble(L"zoom", m_zoom);
        RefreshAll();
        PushMedia(false);
    }
    if (o.HasKey(L"setting")) {
        auto& b = xcui::Bridge::Get();
        auto s = o.GetNamedObject(L"setting");
        for (auto const& kv : s) {
            std::wstring key = kv.Key().c_str();
            auto v = kv.Value();
            // Types per contract §3.
            if (key == L"maxFps" || key == L"pinMode" || key == L"audioOffsetMs") {
                b.SetInt(key.c_str(), v.ValueType() == JsonValueType::Boolean ? (v.GetBoolean() ? 1 : 0)
                                                                              : static_cast<int>(v.GetNumber()));
            } else if (key == L"allowH265" || key == L"autoCrop" || key == L"audioEnabled" || key == L"showStats" ||
                       key == L"devServer") {
                b.SetBool(key.c_str(), v.ValueType() == JsonValueType::Boolean ? v.GetBoolean() : v.GetNumber() != 0);
            } else if (key == L"zoom") {
                b.SetDouble(key.c_str(), v.GetNumber());
            } else if (v.ValueType() == JsonValueType::String) {
                b.SetString(key.c_str(), v.GetString().c_str());
            } else {
                Logf('W', "cmd: setting %s has an unexpected type", winrt::to_string(key).c_str());
            }
        }
    }
    if (o.HasKey(L"disconnect") && o.GetNamedBoolean(L"disconnect", false)) Disconnect();
}

// ============================================================================================ settings

std::vector<MainPage::Row>& MainPage::Rows(int category) {
    return m_rows[std::clamp(category, 0, 3)];
}

void MainPage::OpenSettings() {
    if (m_settings) return;
    if (m_overlay) HideOverlay();
    m_settings = true;
    SettingsShift().X(40);
    anim::Fade(SettingsLayer(), 1, anim::kCalm);
    anim::To(SettingsShift(), L"X", 0, anim::kCalm);
    FocusCategory(m_cat);
    Logf('I', "settings open");
}

void MainPage::CloseSettings() {
    if (!m_settings) return;
    if (m_editingName) CommitName();
    m_settings = false;
    m_row = -1;
    anim::Fade(SettingsLayer(), 0, anim::kNormal);
    anim::To(SettingsShift(), L"X", 40, anim::kNormal);
    RefreshAll();
    Focus(FocusState::Programmatic);
    Logf('I', "settings close");
}

void MainPage::FocusCategory(int index) {
    m_cat = std::clamp(index, 0, 3);
    m_row = -1;
    StackPanel panes[] = {PaneReceiver(), PanePicture(), PaneAdvanced(), PaneAbout()};
    for (int i = 0; i < 4; i++) {
        panes[i].Visibility(i == m_cat ? Visibility::Visible : Visibility::Collapsed);
        m_cats[i].Background(MakeBrush(0, 0, 0, 0));
    }
    m_cats[m_cat].Focus(FocusState::Keyboard);
    RefreshAll();
    UpdateHelp();
}

void MainPage::FocusRow(int index) {
    auto& rows = Rows(m_cat);
    if (rows.empty()) return;
    m_row = std::clamp(index, 0, static_cast<int>(rows.size()) - 1);
    for (int i = 0; i < 4; i++) m_cats[i].Background(i == m_cat ? MakeBrush(0x14, 255, 255, 255) : MakeBrush(0, 0, 0, 0));
    rows[m_row].btn.Focus(FocusState::Keyboard);
    RefreshAll();
    UpdateHelp();
}

void MainPage::UpdateHelp() {
    bool restart = false;
    std::wstring help;
    if (m_row >= 0 && m_row < static_cast<int>(Rows(m_cat).size())) {
        auto const& r = Rows(m_cat)[m_row];
        help = r.help;
        restart = r.restart;
    } else {
        static wchar_t const* const catHelp[] = {
            L"How this screen appears to your iPhone and what it accepts.",
            L"How the picture fits the TV, and the sound.",
            L"Network options for troubleshooting.",
            L"Version and licenses.",
        };
        help = catHelp[m_cat];
    }
    HelpText().Text(help);
    RestartNote().Visibility(restart ? Visibility::Visible : Visibility::Collapsed);
}

void MainPage::SettingsKey(Key k) {
    if (m_row < 0) {
        switch (k) {
        case Key::Up: if (m_cat > 0) FocusCategory(m_cat - 1); break;
        case Key::Down: if (m_cat < 3) FocusCategory(m_cat + 1); break;
        case Key::Right: case Key::A: if (!Rows(m_cat).empty()) FocusRow(0); break;
        default: break;
        }
        return;
    }
    auto& r = Rows(m_cat)[m_row];
    switch (k) {
    case Key::Up: if (m_row > 0) FocusRow(m_row - 1); break;
    case Key::Down: FocusRow(m_row + 1); break;
    case Key::A: ActivateRow(r, 0); break;
    case Key::Left:
        if (r.kind == RowKind::Edit) FocusCategory(m_cat);
        else ActivateRow(r, -1);
        break;
    case Key::Right: if (r.kind != RowKind::Edit) ActivateRow(r, 1); break;
    default: break;
    }
}

void MainPage::ActivateRow(Row& r, int dir) {
    auto& b = xcui::Bridge::Get();
    switch (r.kind) {
    case RowKind::Edit:
        if (dir == 0) BeginEditName();
        return;
    case RowKind::Choice: {
        std::wstring cur = r.intValue ? std::to_wstring(b.GetInt(r.key, _wtoi(r.def.c_str())))
                                      : b.GetString(r.key, r.def.c_str());
        int n = static_cast<int>(r.choices.size());
        int i = 0;
        for (int j = 0; j < n; j++) {
            if (_wcsicmp(r.choices[j].value.c_str(), cur.c_str()) == 0) i = j;
        }
        int next = dir == 0 ? (i + 1) % n : std::clamp(i + dir, 0, n - 1);
        if (next == i) return;
        if (r.intValue) b.SetInt(r.key, _wtoi(r.choices[next].value.c_str()));
        else b.SetString(r.key, r.choices[next].value);
        break;
    }
    case RowKind::Switch: {
        bool def = r.def == L"1";
        bool cur = r.intValue ? b.GetInt(r.key, def ? 1 : 0) != 0 : b.GetBool(r.key, def);
        bool next = dir == 0 ? !cur : dir > 0;
        if (next == cur) return;
        if (r.intValue) b.SetInt(r.key, next ? 1 : 0);
        else b.SetBool(r.key, next);
        break;
    }
    case RowKind::Delay: {
        int cur = b.GetInt(r.key, 0);
        int next = dir == 0 ? 0 : std::clamp(cur + dir * 20, kDelayMin, kDelayMax);
        if (next == cur) return;
        b.SetInt(r.key, next);
        break;
    }
    }
    // Immediate feedback; the Changed round-trip refreshes again (idempotent).
    LoadSettings();
    RefreshAll();
    PushMedia(false);
}

void MainPage::BeginEditName() {
    m_editingName = true;
    auto box = NameBox();
    box.Text(m_name);
    box.Visibility(Visibility::Visible);
    RowName().Opacity(0);
    box.Focus(FocusState::Keyboard);
    box.SelectAll();
    try {
        Windows::UI::ViewManagement::InputPane::GetForCurrentView().TryShow();
    } catch (...) {
    }
    Logf('I', "name edit begin");
}

void MainPage::CommitName() {
    if (!m_editingName) return;
    m_editingName = false;
    std::wstring name = Trim(NameBox().Text().c_str());
    if (name.size() > 40) name.resize(40);
    NameBox().Visibility(Visibility::Collapsed);
    RowName().Opacity(1);
    if (!name.empty() && name != m_name) xcui::Bridge::Get().SetString(L"deviceName", name);
    LoadSettings();
    if (m_settings) FocusRow(0);
    Logf('I', "name edit end");
}

void MainPage::UpdateRow(Row& r, bool focused) {
    if (!r.btn) return;
    auto& b = xcui::Bridge::Get();
    std::wstring text;
    switch (r.kind) {
    case RowKind::Edit: text = b.GetString(r.key, r.def.c_str()); break;
    case RowKind::Choice: {
        std::wstring cur = r.intValue ? std::to_wstring(b.GetInt(r.key, _wtoi(r.def.c_str())))
                                      : b.GetString(r.key, r.def.c_str());
        text = cur;
        for (auto const& c : r.choices) {
            if (_wcsicmp(c.value.c_str(), cur.c_str()) == 0) text = c.label;
        }
        break;
    }
    case RowKind::Switch: {
        bool def = r.def == L"1";
        bool on = r.intValue ? b.GetInt(r.key, def ? 1 : 0) != 0 : b.GetBool(r.key, def);
        text = on ? L"On" : L"Off";
        if (r.track) {
            r.track.Background(on ? (focused ? MakeBrush(255, 0x4D, 0x66, 0xD6) : MakeBrush(255, 0x8E, 0xA8, 0xFF))
                                  : (focused ? MakeBrush(0x40, 0x0B, 0x0B, 0x10) : MakeBrush(0x42, 255, 255, 255)));
        }
        if (r.knobShift) anim::To(r.knobShift, L"X", on ? 26 : 0, 200);
        break;
    }
    case RowKind::Delay: text = DelayLabel(std::clamp(b.GetInt(r.key, 0), kDelayMin, kDelayMax)); break;
    }
    r.val.Text(text);
    for (auto& c : r.chev) c.Opacity(focused ? 1.0 : 0.0);
}

}  // namespace winrt::XboxCast::implementation
