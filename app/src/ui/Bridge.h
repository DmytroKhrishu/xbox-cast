#pragma once
// UI-side adapter: the ONLY file pair in app/src/ui that knows platform (Settings, ReceiverHost, Diag) and media
// (MediaPipeline) types. Everything else in the UI talks to this interface, so contract changes stay in Bridge.cpp.

#include <functional>
#include <string>
#include <vector>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.Controls.h>

namespace xcui {

enum class SessionState { Idle, Connecting, Mirroring };

struct StatRow {
    std::wstring label;
    std::wstring value;
    bool good = false;   // drawn in the "healthy" colour
};

class Bridge {
 public:
    static Bridge& Get();

    // Events. May be raised on ANY thread; MainPage marshals them to its dispatcher.
    std::function<void(std::wstring const& client)> onConnected;
    std::function<void(std::wstring const& reason)> onDisconnected;
    std::function<void(SessionState state)> onState;
    std::function<void(std::wstring const& pin)> onPin;          // empty = hide
    std::function<void(std::wstring const& json)> onStats;       // castcore on_stats, ~1 Hz
    std::function<void(std::wstring const& json)> onCommand;     // dev server POST /cmd body
    std::function<void(std::wstring const& key)> onSettingChanged;

    // Subscribes to platform events. Call once on the UI thread after the callbacks are set.
    void Start();

    // Settings (keys and defaults: contract §3).
    std::wstring GetString(wchar_t const* key, wchar_t const* def);
    int GetInt(wchar_t const* key, int def);
    bool GetBool(wchar_t const* key, bool def);
    double GetDouble(wchar_t const* key, double def);
    void SetString(wchar_t const* key, std::wstring const& value);
    void SetInt(wchar_t const* key, int value);
    void SetBool(wchar_t const* key, bool value);
    void SetDouble(wchar_t const* key, double value);

    // Media (MediaPipeline owned here on behalf of MainPage, contract §3 wiring).
    void AttachVideo(winrt::Windows::UI::Xaml::Controls::SwapChainPanel const& panel);
    void SetViewMode(std::wstring const& mode);   // Fit | Fill | Zoom | Auto
    void SetZoom(float zoom);                     // 1.0 .. 3.0
    void Pan(float dx, float dy);                 // normalized; +x looks right, +y looks down
    void ResetPan();                              // centre the view (new session, Y)
    void SetAutoCrop(bool on);
    void SetAudioEnabled(bool on);
    void SetAudioOffsetMs(int ms);
    void StopVideo();
    std::vector<StatRow> StatsRows(std::wstring const& coreJson);

    // Receiver
    SessionState State() const;
    void Disconnect();
    void RequestRestart();     // receiver settings changed: restart now if idle, else when the session ends
    void OnSessionEnded();     // runs a pending restart
    std::wstring CoreVersion();

    // Logging with tag "ui". level: 'D' 'I' 'W' 'E'.
    void Log(char level, std::string const& message);

 private:
    bool m_restartPending = false;
    bool m_started = false;
};

// printf-style helper over Bridge::Log.
void Logf(char level, char const* fmt, ...);
std::string Narrow(std::wstring const& w);

}  // namespace xcui
