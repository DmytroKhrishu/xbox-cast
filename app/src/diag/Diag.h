#pragma once
// Diagnostics (docs/contract.md §4). Owner: platform. There is no debugger on the console: log richly.
//
//   Diag::Log(Diag::Level::Info, "media", "decoder opened %s %dx%d", codec, w, h);
//
// Lines go to an in-memory ring buffer (4000 lines, monotonic seq) and OutputDebugStringA. Format:
//   2026-01-01T12:00:00.123 I media decoder opened h264 1920x886 d3d11va
// Thread-safe, never throws, cheap enough for ~100 lines/s. Tags: app core mdns net video render audio ui settings diag.
//
// Dev HTTP server (TCP 7099, enabled by the devServer setting): GET /status, GET /log?since=<seq>, POST /cmd.
// Every /cmd body is forwarded verbatim (JSON text) through Diag::Command; viewMode/zoom/setting/disconnect/mdnsTest are
// also handled here. Subscribers run on a server worker thread: marshal to the Dispatcher before touching XAML.

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <cstdarg>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace XboxCast
{
    class Diag
    {
    public:
        enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

        static void Log(Level level, char const* tag, char const* fmt, ...);
        static void LogV(Level level, char const* tag, char const* fmt, va_list args);
        // Pre-formatted message (castcore on_log, already one line or several; split on '\n').
        static void LogRaw(Level level, char const* tag, char const* msg);

        struct Line { uint64_t seq; std::string text; };
        // Lines with seq > since (oldest first), at most `max`. *next = seq of the newest line (pass back as since).
        static std::vector<Line> Since(uint64_t since, size_t max, uint64_t* next);
        static uint64_t LastSeq();

        // Raised for every POST /cmd body (the raw JSON text). Any thread (server worker).
        static winrt::event_token Command(winrt::delegate<winrt::hstring> const& handler);
        static void Command(winrt::event_token const& token) noexcept;
        static void RaiseCommand(winrt::hstring const& json);

        // /status sections. A provider returns a JSON object that becomes /status[name] (e.g. "media"). Register once
        // (same name replaces); called on the server worker thread, must be quick and thread-safe. nullptr unregisters.
        using StatusProvider = std::function<winrt::Windows::Data::Json::JsonObject()>;
        static void SetStatusProvider(std::string const& name, StatusProvider provider);
        static winrt::Windows::Data::Json::JsonObject BuildStatus();

        // Dev HTTP server lifecycle (App calls these; Start is a no-op unless the devServer setting is on).
        static void StartServer();
        static void StopServer();
        static bool ServerRunning();
        static constexpr uint16_t kServerPort = 7099;
    };
}

#define XC_LOGD(tag, ...) ::XboxCast::Diag::Log(::XboxCast::Diag::Level::Debug, tag, __VA_ARGS__)
#define XC_LOGI(tag, ...) ::XboxCast::Diag::Log(::XboxCast::Diag::Level::Info, tag, __VA_ARGS__)
#define XC_LOGW(tag, ...) ::XboxCast::Diag::Log(::XboxCast::Diag::Level::Warn, tag, __VA_ARGS__)
#define XC_LOGE(tag, ...) ::XboxCast::Diag::Log(::XboxCast::Diag::Level::Error, tag, __VA_ARGS__)
