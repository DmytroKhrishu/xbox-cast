#include "pch.h"
#include "Bridge.h"

#include <cstdarg>
#include <cstdio>
#include <memory>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.Threading.h>

#include "core/ReceiverHost.h"
#include "diag/Diag.h"
#include "settings/Settings.h"

// Media lands separately (contract §3). Until app/src/media/MediaPipeline.h exists the UI runs without video: every
// media call below only logs. Only the methods the contract lists are used, so the header can land without UI edits.
#if __has_include("media/MediaPipeline.h")
#include "media/MediaPipeline.h"
#define XC_UI_HAVE_MEDIA 1
#else
#define XC_UI_HAVE_MEDIA 0
#endif

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Data::Json;

namespace xcui {

namespace {
#if XC_UI_HAVE_MEDIA
std::unique_ptr<::XboxCast::MediaPipeline> g_pipeline;
#endif

::XboxCast::Diag::Level ToLevel(char level) {
    switch (level) {
    case 'D': return ::XboxCast::Diag::Level::Debug;
    case 'W': return ::XboxCast::Diag::Level::Warn;
    case 'E': return ::XboxCast::Diag::Level::Error;
    default: return ::XboxCast::Diag::Level::Info;
    }
}

SessionState Map(::XboxCast::ReceiverState s) {
    switch (s) {
    case ::XboxCast::ReceiverState::Connecting: return SessionState::Connecting;
    case ::XboxCast::ReceiverState::Mirroring: return SessionState::Mirroring;
    default: return SessionState::Idle;   // Stopped and Idle both show the idle screen
    }
}

// Settings has typed accessors per key but its generic Get is private; ToJson() is the public generic read
// (14 keys, cheap) and Set() the public generic write with validation.
IJsonValue Lookup(wchar_t const* key) {
    try {
        auto o = ::XboxCast::Settings::Instance().ToJson();
        if (o.HasKey(key)) return o.GetNamedValue(key);
    } catch (...) {
    }
    return nullptr;
}

void Store(wchar_t const* key, IJsonValue const& v, std::string const& shown) {
    std::wstring err;
    bool ok = false;
    try {
        ok = ::XboxCast::Settings::Instance().Set(key, v, &err);
    } catch (hresult_error const& e) {
        err = e.message().c_str();
    }
    if (ok) Logf('I', "setting %s=%s", Narrow(key).c_str(), shown.c_str());
    else Logf('W', "setting %s=%s rejected: %s", Narrow(key).c_str(), shown.c_str(), Narrow(err).c_str());
}

void AppendRows(std::vector<StatRow>& rows, JsonObject const& o) {
    for (auto const& kv : o) {
        StatRow r;
        r.label = kv.Key().c_str();
        auto v = kv.Value();
        switch (v.ValueType()) {
        case JsonValueType::Number: {
            wchar_t buf[64];
            double n = v.GetNumber();
            if (n == static_cast<double>(static_cast<long long>(n))) swprintf(buf, 64, L"%lld", static_cast<long long>(n));
            else swprintf(buf, 64, L"%.1f", n);
            r.value = buf;
            break;
        }
        case JsonValueType::String: r.value = v.GetString().c_str(); break;
        case JsonValueType::Boolean: r.value = v.GetBoolean() ? L"yes" : L"no"; break;
        default: continue;
        }
        rows.push_back(r);
    }
}
}  // namespace

std::string Narrow(std::wstring const& w) {
    return winrt::to_string(w);
}

void Logf(char level, char const* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Bridge::Get().Log(level, buf);
}

Bridge& Bridge::Get() {
    static Bridge instance;
    return instance;
}

void Bridge::Log(char level, std::string const& message) {
    ::XboxCast::Diag::LogRaw(ToLevel(level), "ui", message.c_str());
}

// ---------------------------------------------------------------- settings (platform Settings, contract §3)
std::wstring Bridge::GetString(wchar_t const* key, wchar_t const* def) {
    auto v = Lookup(key);
    if (v && v.ValueType() == JsonValueType::String) return v.GetString().c_str();
    if (v && v.ValueType() == JsonValueType::Number) return std::to_wstring(static_cast<int>(v.GetNumber()));
    return def;
}

int Bridge::GetInt(wchar_t const* key, int def) {
    auto v = Lookup(key);
    if (!v) return def;
    if (v.ValueType() == JsonValueType::Number) return static_cast<int>(v.GetNumber());
    if (v.ValueType() == JsonValueType::Boolean) return v.GetBoolean() ? 1 : 0;
    return def;
}

bool Bridge::GetBool(wchar_t const* key, bool def) {
    auto v = Lookup(key);
    if (!v) return def;
    if (v.ValueType() == JsonValueType::Boolean) return v.GetBoolean();
    if (v.ValueType() == JsonValueType::Number) return v.GetNumber() != 0;
    return def;
}

double Bridge::GetDouble(wchar_t const* key, double def) {
    auto v = Lookup(key);
    if (v && v.ValueType() == JsonValueType::Number) return v.GetNumber();
    return def;
}

void Bridge::SetString(wchar_t const* key, std::wstring const& value) {
    Store(key, JsonValue::CreateStringValue(value), Narrow(value));
}

void Bridge::SetInt(wchar_t const* key, int value) {
    Store(key, JsonValue::CreateNumberValue(value), std::to_string(value));
}

void Bridge::SetBool(wchar_t const* key, bool value) {
    Store(key, JsonValue::CreateBooleanValue(value), value ? "true" : "false");
}

void Bridge::SetDouble(wchar_t const* key, double value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", value);
    Store(key, JsonValue::CreateNumberValue(value), buf);
}

// ---------------------------------------------------------------- lifecycle / receiver
void Bridge::Start() {
    if (m_started) return;
    m_started = true;
    using namespace ::XboxCast;
    auto& host = ReceiverHost::Instance();
    // All of these fire on non-UI threads; the callbacks (MainPage) marshal to the dispatcher.
    host.StateChanged([this](ReceiverState s) {
        if (onState) onState(Map(s));
    });
    host.Connected([this](ClientInfo const& c) {
        std::wstring name = c.name.c_str();
        if (name.empty()) name = c.model.c_str();
        if (onConnected) onConnected(name);
    });
    host.Disconnected([this](hstring const& reason) {
        if (onDisconnected) onDisconnected(reason.c_str());
    });
    host.Pin([this](hstring const& pin) {
        if (onPin) onPin(pin.c_str());
    });
    host.Stats([this](hstring const& json) {
        if (onStats) onStats(json.c_str());
    });
    Diag::Command([this](hstring const& json) {
        if (onCommand) onCommand(json.c_str());
    });
    Settings::Instance().Changed([this](hstring const& key) {
        if (onSettingChanged) onSettingChanged(key.c_str());
    });
    Logf('I', "bridge start media=%d castcore=%d state=%s", XC_UI_HAVE_MEDIA, host.HasCastcore() ? 1 : 0,
         ReceiverStateName(host.State()));
}

SessionState Bridge::State() const {
    return Map(::XboxCast::ReceiverHost::Instance().State());
}

void Bridge::Disconnect() {
    Log('I', "disconnect requested");
    // Disconnect takes the lifecycle lock (may block behind a running Start): keep it off the UI thread.
    Windows::System::Threading::ThreadPool::RunAsync([](auto&&) { ::XboxCast::ReceiverHost::Instance().Disconnect(); });
}

void Bridge::RequestRestart() {
    // ReceiverHost restarts itself when one of its keys changes (Settings::Changed, debounced 300 ms).
    Logf('I', "receiver setting changed, ReceiverHost restarts the receiver (state=%s)",
         ::XboxCast::ReceiverStateName(::XboxCast::ReceiverHost::Instance().State()));
}

std::wstring Bridge::ReceiverProblem() {
    auto& host = ::XboxCast::ReceiverHost::Instance();
    if (host.State() != ::XboxCast::ReceiverState::Stopped) return {};
    // Stopped with no error = starting up or between Stop and Start of a restart: not a problem yet.
    std::string e = host.LastError();
    return std::wstring(e.begin(), e.end());
}

void Bridge::OnSessionEnded() {
}

std::wstring Bridge::CoreVersion() {
    return winrt::to_hstring(::XboxCast::ReceiverHost::Instance().CastcoreVersion()).c_str();
}

// ---------------------------------------------------------------- media (contract §3 MediaPipeline)
#if XC_UI_HAVE_MEDIA
namespace {
::XboxCast::ViewMode ToMode(std::wstring const& m) {
    if (m == L"Fit") return ::XboxCast::ViewMode::Fit;
    if (m == L"Fill") return ::XboxCast::ViewMode::Fill;
    if (m == L"Zoom") return ::XboxCast::ViewMode::Zoom;
    return ::XboxCast::ViewMode::Auto;
}
}  // namespace

void Bridge::AttachVideo(winrt::Windows::UI::Xaml::Controls::SwapChainPanel const& panel) {
    if (!g_pipeline) g_pipeline = std::make_unique<::XboxCast::MediaPipeline>();
    g_pipeline->Attach(panel);
    ::XboxCast::ReceiverHost::Instance().SetSinks(g_pipeline.get(), g_pipeline.get());
    Log('I', "video attached, sinks set");
}
void Bridge::SetViewMode(std::wstring const& mode) {
    if (g_pipeline) g_pipeline->SetViewMode(ToMode(mode));
}
void Bridge::SetZoom(float zoom) {
    if (g_pipeline) g_pipeline->SetZoom(zoom);
}
void Bridge::Pan(float dx, float dy) {
    if (g_pipeline) g_pipeline->Pan(dx, dy);
}
void Bridge::ResetPan() {
    if (g_pipeline) g_pipeline->ResetPan();
}
void Bridge::SetAutoCrop(bool on) {
    if (g_pipeline) g_pipeline->SetAutoCrop(on);
}
void Bridge::SetAudioEnabled(bool on) {
    if (g_pipeline) g_pipeline->SetAudioEnabled(on);
}
void Bridge::SetAudioOffsetMs(int ms) {
    if (g_pipeline) g_pipeline->SetAudioOffsetMs(ms);
}
void Bridge::StopVideo() {
    if (g_pipeline) g_pipeline->Stop();
}
#else
void Bridge::AttachVideo(winrt::Windows::UI::Xaml::Controls::SwapChainPanel const&) {
    Log('I', "video attach skipped (no media pipeline in this build)");
}
void Bridge::SetViewMode(std::wstring const& mode) {
    Logf('D', "media viewMode=%s (no media in this build)", Narrow(mode).c_str());
}
void Bridge::SetZoom(float) {}
void Bridge::Pan(float, float) {}
void Bridge::ResetPan() {}
void Bridge::SetAutoCrop(bool) {}
void Bridge::SetAudioEnabled(bool) {}
void Bridge::SetAudioOffsetMs(int) {}
void Bridge::StopVideo() {}
#endif

std::vector<StatRow> Bridge::StatsRows(std::wstring const& coreJson) {
    // castcore on_stats JSON + the "media" /status section (whatever media registers), flattened one level.
    std::vector<StatRow> rows;
    JsonObject o{nullptr};
    if (!coreJson.empty() && JsonObject::TryParse(coreJson, o)) AppendRows(rows, o);
    try {
        auto status = ::XboxCast::Diag::BuildStatus();
        if (status.HasKey(L"media")) {
            auto m = status.GetNamedValue(L"media");
            if (m.ValueType() == JsonValueType::Object) AppendRows(rows, m.GetObject());
        }
    } catch (...) {
    }
    return rows;
}

}  // namespace xcui
