#include "pch.h"
#include "settings/Settings.h"
#include "diag/Diag.h"

#include <algorithm>
#include <cwctype>
#include <random>

using namespace winrt;
using namespace Windows::Data::Json;
using namespace Windows::Foundation;
using namespace Windows::Storage;

namespace XboxCast
{
    namespace
    {
#ifdef XC_STORE_BUILD
        constexpr bool kDevServerDefault = false;
#else
        constexpr bool kDevServerDefault = true;
#endif
        enum class Kind { String, Int, Bool, Double };
        struct KeyInfo { wchar_t const* key; Kind kind; };
        constexpr KeyInfo kKeys[] = {
            { Settings::kDeviceName, Kind::String },  { Settings::kDeviceId, Kind::String },
            { Settings::kResolution, Kind::String },  { Settings::kMaxFps, Kind::Int },
            { Settings::kAllowH265, Kind::Bool },     { Settings::kPinMode, Kind::Int },
            { Settings::kViewMode, Kind::String },    { Settings::kZoom, Kind::Double },
            { Settings::kAutoCrop, Kind::Bool },      { Settings::kAudioEnabled, Kind::Bool },
            { Settings::kAudioOffsetMs, Kind::Int },  { Settings::kShowStats, Kind::Bool },
            { Settings::kDevServer, Kind::Bool },     { Settings::kMdnsMode, Kind::String },
        };

        IPropertySet Values()
        {
            return ApplicationData::Current().LocalSettings().Values();
        }

        bool IsHex12(hstring const& s)
        {
            if (s.size() != 12) return false;
            for (wchar_t c : s) if (!std::iswxdigit(c)) return false;
            return true;
        }

        // Case-insensitive pick from a fixed set; returns the canonical spelling or empty.
        hstring Pick(hstring const& v, std::initializer_list<wchar_t const*> allowed)
        {
            for (auto a : allowed)
                if (_wcsicmp(v.c_str(), a) == 0) return hstring(a);
            return {};
        }

        std::wstring Trim(std::wstring s)
        {
            auto ws = [](wchar_t c) { return std::iswspace(c) != 0; };
            s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), ws));
            s.erase(std::find_if_not(s.rbegin(), s.rend(), ws).base(), s.end());
            return s;
        }
    }

    Settings& Settings::Instance()
    {
        static Settings s;
        return s;
    }

    // ---- raw storage --------------------------------------------------------------------------------------------------

    hstring Settings::GetString(wchar_t const* key, wchar_t const* def)
    {
        std::lock_guard<std::recursive_mutex> g(m_lock);
        try { return unbox_value_or<hstring>(Values().TryLookup(key), def); }
        catch (...) { return def; }
    }

    int Settings::GetInt(wchar_t const* key, int def)
    {
        std::lock_guard<std::recursive_mutex> g(m_lock);
        try
        {
            auto v = Values().TryLookup(key);
            if (!v) return def;
            if (auto i = v.try_as<IReference<int32_t>>()) return i.Value();
            if (auto d = v.try_as<IReference<double>>()) return static_cast<int>(d.Value());
            return def;
        }
        catch (...) { return def; }
    }

    bool Settings::GetBool(wchar_t const* key, bool def)
    {
        std::lock_guard<std::recursive_mutex> g(m_lock);
        try { return unbox_value_or<bool>(Values().TryLookup(key), def); }
        catch (...) { return def; }
    }

    double Settings::GetDouble(wchar_t const* key, double def)
    {
        std::lock_guard<std::recursive_mutex> g(m_lock);
        try
        {
            auto v = Values().TryLookup(key);
            if (!v) return def;
            if (auto d = v.try_as<IReference<double>>()) return d.Value();
            if (auto i = v.try_as<IReference<int32_t>>()) return i.Value();
            return def;
        }
        catch (...) { return def; }
    }

    void Settings::Put(wchar_t const* key, IInspectable const& value)
    {
        bool changed = false;
        {
            std::lock_guard<std::recursive_mutex> g(m_lock);
            try
            {
                auto values = Values();
                auto old = values.TryLookup(key);
                auto oldPv = old.try_as<IPropertyValue>();
                auto newPv = value.try_as<IPropertyValue>();
                bool same = false;
                if (oldPv && newPv && oldPv.Type() == newPv.Type())
                {
                    switch (newPv.Type())
                    {
                    case PropertyType::String: same = oldPv.GetString() == newPv.GetString(); break;
                    case PropertyType::Int32: same = oldPv.GetInt32() == newPv.GetInt32(); break;
                    case PropertyType::Boolean: same = oldPv.GetBoolean() == newPv.GetBoolean(); break;
                    case PropertyType::Double: same = oldPv.GetDouble() == newPv.GetDouble(); break;
                    default: break;
                    }
                }
                if (!same)
                {
                    values.Insert(key, value);
                    changed = true;
                }
            }
            catch (hresult_error const& e)
            {
                Diag::Log(Diag::Level::Error, "settings", "store %ls failed: 0x%08x %ls", key,
                          static_cast<uint32_t>(e.code()), e.message().c_str());
                return;
            }
        }
        if (changed)
        {
            Diag::Log(Diag::Level::Info, "settings", "%ls changed", key);
            try { m_changed(hstring(key)); }
            catch (hresult_error const& e)
            {
                Diag::Log(Diag::Level::Error, "settings", "Changed handler for %ls threw 0x%08x %ls", key,
                          static_cast<uint32_t>(e.code()), e.message().c_str());
            }
            catch (...)
            {
                Diag::Log(Diag::Level::Error, "settings", "Changed handler for %ls threw", key);
            }
        }
    }

    event_token Settings::Changed(delegate<hstring> const& handler) { return m_changed.add(handler); }
    void Settings::Changed(event_token const& token) noexcept { m_changed.remove(token); }

    // ---- typed accessors -------------------------------------------------------------------------------------------

    hstring Settings::DeviceName()
    {
        auto v = GetString(kDeviceName, L"Xbox");
        return v.empty() ? hstring(L"Xbox") : v;
    }
    void Settings::DeviceName(hstring const& v)
    {
        auto t = Trim(std::wstring(v));
        if (t.empty()) t = L"Xbox";
        if (t.size() > 50) t.resize(50);    // mDNS labels are <= 63 bytes; RAOP adds "<12 hex>@"
        Put(kDeviceName, box_value(hstring(t)));
    }

    hstring Settings::DeviceId()
    {
        wchar_t buf[13];
        {
            std::lock_guard<std::recursive_mutex> g(m_lock);
            auto v = GetString(kDeviceId, L"");
            if (IsHex12(v)) return v;
            std::random_device rd;
            unsigned char b[6];
            for (auto& x : b) x = static_cast<unsigned char>(rd() & 0xff);
            b[0] = static_cast<unsigned char>((b[0] & 0xfe) | 0x02);   // unicast, locally administered (used as a MAC)
            swprintf_s(buf, L"%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
            try { Values().Insert(kDeviceId, box_value(hstring(buf))); }
            catch (...) {}
        }
        Diag::Log(Diag::Level::Info, "settings", "generated deviceId %ls", buf);
        return hstring(buf);
    }

    bool Settings::DeviceIdBytes(unsigned char out[6])
    {
        auto id = DeviceId();
        for (int i = 0; i < 6; i++)
        {
            unsigned v = 0;
            if (swscanf_s(id.c_str() + i * 2, L"%2x", &v) != 1) return false;
            out[i] = static_cast<unsigned char>(v);
        }
        return true;
    }

    hstring Settings::Resolution()
    {
        auto v = Pick(GetString(kResolution, L"1080p"), { L"1080p", L"1440p", L"2160p" });
        return v.empty() ? hstring(L"1080p") : v;
    }
    void Settings::Resolution(hstring const& v)
    {
        auto p = Pick(v, { L"1080p", L"1440p", L"2160p" });
        if (!p.empty()) Put(kResolution, box_value(p));
    }

    void Settings::ResolutionSize(int& width, int& height)
    {
        auto r = Resolution();
        if (r == L"2160p") { width = 3840; height = 2160; }
        else if (r == L"1440p") { width = 2560; height = 1440; }
        else { width = 1920; height = 1080; }
    }

    int Settings::MaxFps() { return GetInt(kMaxFps, 60) == 30 ? 30 : 60; }
    void Settings::MaxFps(int v) { Put(kMaxFps, box_value(static_cast<int32_t>(v <= 30 ? 30 : 60))); }

    bool Settings::AllowH265() { return GetBool(kAllowH265, true); }
    void Settings::AllowH265(bool v) { Put(kAllowH265, box_value(v)); }

    int Settings::PinMode() { return GetInt(kPinMode, 0) == 1 ? 1 : 0; }
    void Settings::PinMode(int v) { Put(kPinMode, box_value(static_cast<int32_t>(v == 1 ? 1 : 0))); }

    hstring Settings::ViewMode()
    {
        auto v = Pick(GetString(kViewMode, L"Auto"), { L"Fit", L"Fill", L"Zoom", L"Auto" });
        return v.empty() ? hstring(L"Auto") : v;
    }
    void Settings::ViewMode(hstring const& v)
    {
        auto p = Pick(v, { L"Fit", L"Fill", L"Zoom", L"Auto" });
        if (!p.empty()) Put(kViewMode, box_value(p));
    }

    double Settings::Zoom() { return (std::min)(3.0, (std::max)(1.0, GetDouble(kZoom, 1.0))); }
    void Settings::Zoom(double v) { Put(kZoom, box_value((std::min)(3.0, (std::max)(1.0, v)))); }

    bool Settings::AutoCrop() { return GetBool(kAutoCrop, true); }
    void Settings::AutoCrop(bool v) { Put(kAutoCrop, box_value(v)); }

    bool Settings::AudioEnabled() { return GetBool(kAudioEnabled, true); }
    void Settings::AudioEnabled(bool v) { Put(kAudioEnabled, box_value(v)); }

    int Settings::AudioOffsetMs() { return (std::min)(300, (std::max)(-300, GetInt(kAudioOffsetMs, 0))); }
    void Settings::AudioOffsetMs(int v) { Put(kAudioOffsetMs, box_value(static_cast<int32_t>((std::min)(300, (std::max)(-300, v))))); }

    bool Settings::ShowStats() { return GetBool(kShowStats, false); }
    void Settings::ShowStats(bool v) { Put(kShowStats, box_value(v)); }

    bool Settings::DevServer() { return GetBool(kDevServer, kDevServerDefault); }
    void Settings::DevServer(bool v) { Put(kDevServer, box_value(v)); }

    hstring Settings::MdnsMode()
    {
        auto v = Pick(GetString(kMdnsMode, L"builtin"), { L"builtin", L"os" });
        return v.empty() ? hstring(L"builtin") : v;
    }
    void Settings::MdnsMode(hstring const& v)
    {
        auto p = Pick(v, { L"builtin", L"os" });
        if (!p.empty()) Put(kMdnsMode, box_value(p));
    }

    // ---- generic -----------------------------------------------------------------------------------------------------

    bool Settings::Set(hstring const& key, IJsonValue const& value, std::wstring* error)
    {
        auto fail = [&](std::wstring const& why) { if (error) *error = why; return false; };
        KeyInfo const* info = nullptr;
        for (auto const& k : kKeys) if (key == k.key) info = &k;
        if (!info) return fail(L"unknown setting '" + std::wstring(key) + L"'");

        hstring s;
        double num = 0;
        bool flag = false;
        auto t = value.ValueType();
        if (t == JsonValueType::String) s = value.GetString();
        else if (t == JsonValueType::Number) { num = value.GetNumber(); s = to_hstring(num); }
        else if (t == JsonValueType::Boolean) { flag = value.GetBoolean(); s = flag ? L"true" : L"false"; }
        else return fail(L"value for '" + std::wstring(key) + L"' must be a string, number or bool");

        switch (info->kind)
        {
        case Kind::Bool:
            if (t == JsonValueType::Boolean) break;
            if (t == JsonValueType::Number) { flag = num != 0; break; }
            if (_wcsicmp(s.c_str(), L"true") == 0 || s == L"1") flag = true;
            else if (_wcsicmp(s.c_str(), L"false") == 0 || s == L"0") flag = false;
            else return fail(L"'" + std::wstring(key) + L"' expects a bool");
            break;
        case Kind::Int:
        case Kind::Double:
            if (t == JsonValueType::String)
            {
                wchar_t* end = nullptr;
                num = wcstod(s.c_str(), &end);
                if (end == s.c_str() || *end) return fail(L"'" + std::wstring(key) + L"' expects a number");
            }
            else if (t == JsonValueType::Boolean) num = flag ? 1 : 0;
            break;
        case Kind::String:
            break;
        }

        std::wstring k(key);
        if (k == kDeviceName) DeviceName(s);
        else if (k == kDeviceId)
        {
            if (!IsHex12(s)) return fail(L"deviceId must be 12 hex digits");
            std::wstring lower(s);
            for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
            Put(kDeviceId, box_value(hstring(lower)));
        }
        else if (k == kResolution) { if (Pick(s, { L"1080p", L"1440p", L"2160p" }).empty()) return fail(L"resolution: 1080p|1440p|2160p"); Resolution(s); }
        else if (k == kMaxFps) { if (num != 30 && num != 60) return fail(L"maxFps: 30|60"); MaxFps(static_cast<int>(num)); }
        else if (k == kAllowH265) AllowH265(flag);
        else if (k == kPinMode) { if (num != 0 && num != 1) return fail(L"pinMode: 0|1"); PinMode(static_cast<int>(num)); }
        else if (k == kViewMode) { if (Pick(s, { L"Fit", L"Fill", L"Zoom", L"Auto" }).empty()) return fail(L"viewMode: Fit|Fill|Zoom|Auto"); ViewMode(s); }
        else if (k == kZoom) Zoom(num);
        else if (k == kAutoCrop) AutoCrop(flag);
        else if (k == kAudioEnabled) AudioEnabled(flag);
        else if (k == kAudioOffsetMs) AudioOffsetMs(static_cast<int>(num));
        else if (k == kShowStats) ShowStats(flag);
        else if (k == kDevServer) DevServer(flag);
        else if (k == kMdnsMode) { if (Pick(s, { L"builtin", L"os" }).empty()) return fail(L"mdnsMode: builtin|os"); MdnsMode(s); }
        return true;
    }

    JsonObject Settings::ToJson()
    {
        JsonObject o;
        o.SetNamedValue(kDeviceName, JsonValue::CreateStringValue(DeviceName()));
        o.SetNamedValue(kDeviceId, JsonValue::CreateStringValue(DeviceId()));
        o.SetNamedValue(kResolution, JsonValue::CreateStringValue(Resolution()));
        o.SetNamedValue(kMaxFps, JsonValue::CreateNumberValue(MaxFps()));
        o.SetNamedValue(kAllowH265, JsonValue::CreateBooleanValue(AllowH265()));
        o.SetNamedValue(kPinMode, JsonValue::CreateNumberValue(PinMode()));
        o.SetNamedValue(kViewMode, JsonValue::CreateStringValue(ViewMode()));
        o.SetNamedValue(kZoom, JsonValue::CreateNumberValue(Zoom()));
        o.SetNamedValue(kAutoCrop, JsonValue::CreateBooleanValue(AutoCrop()));
        o.SetNamedValue(kAudioEnabled, JsonValue::CreateBooleanValue(AudioEnabled()));
        o.SetNamedValue(kAudioOffsetMs, JsonValue::CreateNumberValue(AudioOffsetMs()));
        o.SetNamedValue(kShowStats, JsonValue::CreateBooleanValue(ShowStats()));
        o.SetNamedValue(kDevServer, JsonValue::CreateBooleanValue(DevServer()));
        o.SetNamedValue(kMdnsMode, JsonValue::CreateStringValue(MdnsMode()));
        return o;
    }

    void Settings::ResetAll()
    {
        {
            std::lock_guard<std::recursive_mutex> g(m_lock);
            try { Values().Clear(); }
            catch (...) {}
        }
        Diag::Log(Diag::Level::Warn, "settings", "all settings reset to defaults");
        for (auto const& k : kKeys)
        {
            try { m_changed(hstring(k.key)); }
            catch (...) {}
        }
    }
}
