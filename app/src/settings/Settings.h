#pragma once
// Typed accessors over ApplicationData::Current().LocalSettings() (docs/contract.md §3 "Settings keys").
// Owner: platform. Safe from any thread. Every setter that changes a value raises Changed(key) on the calling thread,
// after the value is stored; handlers that touch XAML must marshal to their Dispatcher.

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <mutex>
#include <string>

namespace XboxCast
{
    class Settings
    {
    public:
        static Settings& Instance();

        // Keys (use these instead of string literals).
        static constexpr wchar_t const* kDeviceName = L"deviceName";
        static constexpr wchar_t const* kDeviceId = L"deviceId";
        static constexpr wchar_t const* kResolution = L"resolution";
        static constexpr wchar_t const* kMaxFps = L"maxFps";
        static constexpr wchar_t const* kAllowH265 = L"allowH265";
        static constexpr wchar_t const* kPinMode = L"pinMode";
        static constexpr wchar_t const* kViewMode = L"viewMode";
        static constexpr wchar_t const* kZoom = L"zoom";
        static constexpr wchar_t const* kAutoCrop = L"autoCrop";
        static constexpr wchar_t const* kAudioEnabled = L"audioEnabled";
        static constexpr wchar_t const* kAudioOffsetMs = L"audioOffsetMs";
        static constexpr wchar_t const* kShowStats = L"showStats";
        static constexpr wchar_t const* kDevServer = L"devServer";
        static constexpr wchar_t const* kMdnsMode = L"mdnsMode";

        winrt::hstring DeviceName();            void DeviceName(winrt::hstring const& v);       // "Xbox"
        winrt::hstring DeviceId();              /* 12 lowercase hex, generated + stored on first read */
        winrt::hstring Resolution();            void Resolution(winrt::hstring const& v);       // "1080p" | "1440p" | "2160p"
        int MaxFps();                           void MaxFps(int v);                             // 30 | 60
        bool AllowH265();                       void AllowH265(bool v);                         // true
        int PinMode();                          void PinMode(int v);                            // 0 none, 1 on-screen PIN
        winrt::hstring ViewMode();              void ViewMode(winrt::hstring const& v);         // "Fit" | "Fill" | "Zoom" | "Auto"
        double Zoom();                          void Zoom(double v);                            // 1.0, clamped 1..3
        bool AutoCrop();                        void AutoCrop(bool v);                          // true
        bool AudioEnabled();                    void AudioEnabled(bool v);                      // true
        int AudioOffsetMs();                    void AudioOffsetMs(int v);                      // 0, clamped -300..300
        bool ShowStats();                       void ShowStats(bool v);                         // false
        bool DevServer();                       void DevServer(bool v);                         // true (dev) / false (Store)
        winrt::hstring MdnsMode();              void MdnsMode(winrt::hstring const& v);         // "builtin" | "os"

        // Derived helpers.
        void ResolutionSize(int& width, int& height);   // 1080p -> 1920x1080, 1440p -> 2560x1440, 2160p -> 3840x2160
        bool DeviceIdBytes(unsigned char out[6]);

        // Generic access (diag /cmd {"setting":{...}}, /status). Set validates and coerces by key type; returns false for an
        // unknown key or invalid value (reason in `error`). Strings are accepted for every type ("true", "60", "1.5").
        bool Set(winrt::hstring const& key, winrt::Windows::Data::Json::IJsonValue const& value, std::wstring* error = nullptr);
        winrt::Windows::Data::Json::JsonObject ToJson();
        void ResetAll();    // removes every key (defaults apply again; deviceId is regenerated)

        // Raised with the key name after a value changed (only when it actually changed). Any thread.
        winrt::event_token Changed(winrt::delegate<winrt::hstring> const& handler);
        void Changed(winrt::event_token const& token) noexcept;

    private:
        Settings() = default;
        winrt::hstring GetString(wchar_t const* key, wchar_t const* def);
        int GetInt(wchar_t const* key, int def);
        bool GetBool(wchar_t const* key, bool def);
        double GetDouble(wchar_t const* key, double def);
        void Put(wchar_t const* key, winrt::Windows::Foundation::IInspectable const& value);

        std::recursive_mutex m_lock;
        winrt::event<winrt::delegate<winrt::hstring>> m_changed;
    };
}
