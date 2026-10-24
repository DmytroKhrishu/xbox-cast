#include "pch.h"
#include "core/ReceiverHost.h"
#include "core/DnssdPublisher.h"
#include "diag/Diag.h"
#include "settings/Settings.h"

using namespace winrt;
using namespace Windows::Data::Json;

namespace XboxCast
{
    char const* ReceiverStateName(ReceiverState s)
    {
        switch (s)
        {
        case ReceiverState::Idle: return "idle";
        case ReceiverState::Connecting: return "connecting";
        case ReceiverState::Mirroring: return "mirroring";
        default: return "stopped";
        }
    }

    namespace
    {
        template <typename E, typename A>
        void Raise(E& ev, A const& arg, char const* what)
        {
            try { ev(arg); }
            catch (hresult_error const& e)
            {
                Diag::Log(Diag::Level::Error, "core", "%s handler threw 0x%08x %ls", what, static_cast<uint32_t>(e.code()),
                          e.message().c_str());
            }
            catch (...)
            {
                Diag::Log(Diag::Level::Error, "core", "%s handler threw", what);
            }
        }

        char const* VideoCodecName(xc_video_codec c) { return c == XC_VIDEO_H265 ? "h265" : "h264"; }
        char const* AudioCodecName(xc_audio_codec c)
        {
            switch (c)
            {
            case XC_AUDIO_AAC_ELD: return "aac-eld";
            case XC_AUDIO_ALAC: return "alac";
            case XC_AUDIO_AAC_LC: return "aac-lc";
            case XC_AUDIO_PCM: return "pcm";
            default: return "?";
            }
        }
        hstring H(char const* s) { return s ? to_hstring(std::string(s)) : hstring(); }
    }

    ReceiverHost& ReceiverHost::Instance()
    {
        static ReceiverHost s;
        return s;
    }

    ReceiverHost::ReceiverHost()
    {
        m_dnssd = std::make_unique<DnssdPublisher>();
        Settings::Instance().Changed([this](hstring const& key) { OnSettingChanged(key); });
    }

    bool ReceiverHost::HasCastcore() const
    {
#ifdef XC_HAVE_CASTCORE
        return true;
#else
        return false;
#endif
    }

    std::string ReceiverHost::CastcoreVersion() const
    {
#ifdef XC_HAVE_CASTCORE
        char const* v = xc_version_string();
        return v ? v : "";
#else
        return "";
#endif
    }

    std::string ReceiverHost::LastError()
    {
        std::lock_guard<std::mutex> g(m_infoLock);
        return m_lastError;
    }

    ClientInfo ReceiverHost::Client()
    {
        std::lock_guard<std::mutex> g(m_infoLock);
        return m_client;
    }

    void ReceiverHost::SetState(ReceiverState s)
    {
        auto old = m_state.exchange(s);
        if (old == s) return;
        Diag::Log(Diag::Level::Info, "core", "state %s -> %s", ReceiverStateName(old), ReceiverStateName(s));
        Raise(m_stateChanged, s, "StateChanged");
    }

    void ReceiverHost::SetSinks(IVideoSink* video, IAudioSink* audio)
    {
        std::unique_lock<std::shared_mutex> g(m_sinkLock);
        m_video = video;
        m_audio = audio;
        Diag::Log(Diag::Level::Info, "core", "sinks set video=%d audio=%d", video ? 1 : 0, audio ? 1 : 0);
    }

    // Settings that change what the sender sees (advertisement / negotiated stream) restart the receiver. Restarts run on
    // the thread pool (Changed fires on the caller's thread, often the UI) and coalesce: only the last one in 300 ms runs.
    void ReceiverHost::OnSettingChanged(hstring const& key)
    {
        static wchar_t const* kRestartKeys[] = { Settings::kDeviceName, Settings::kDeviceId, Settings::kResolution,
                                                 Settings::kMaxFps, Settings::kAllowH265, Settings::kPinMode,
                                                 Settings::kMdnsMode };
        bool restart = false;
        for (auto k : kRestartKeys) if (key == k) restart = true;
        if (restart && m_state.load() != ReceiverState::Stopped)
        {
            uint32_t gen = ++m_restartGen;
            Windows::System::Threading::ThreadPool::RunAsync([this, gen, key](auto&&)
            {
                Sleep(300);
                if (gen != m_restartGen.load()) return;
                Diag::Log(Diag::Level::Info, "core", "setting %ls changed: restarting the receiver", key.c_str());
                Restart();
            });
        }
        if (key == Settings::kDevServer)
        {
            if (Settings::Instance().DevServer()) Diag::StartServer();
            else Diag::StopServer();
        }
    }

    JsonObject ReceiverHost::StatusJson()
    {
        JsonObject o;
        o.SetNamedValue(L"state", JsonValue::CreateStringValue(H(ReceiverStateName(m_state.load()))));
        o.SetNamedValue(L"castcore", JsonValue::CreateBooleanValue(HasCastcore()));
        o.SetNamedValue(L"castcoreVersion", JsonValue::CreateStringValue(H(CastcoreVersion().c_str())));
        o.SetNamedValue(L"mdns", JsonValue::CreateStringValue(m_builtinMdns ? L"builtin" : L"os"));
        o.SetNamedValue(L"dnssdRegistrations", JsonValue::CreateNumberValue(static_cast<double>(m_dnssd->Count())));
        o.SetNamedValue(L"videoFrames", JsonValue::CreateNumberValue(static_cast<double>(m_videoFrames.load())));
        o.SetNamedValue(L"videoBytes", JsonValue::CreateNumberValue(static_cast<double>(m_videoBytes.load())));
        o.SetNamedValue(L"audioPackets", JsonValue::CreateNumberValue(static_cast<double>(m_audioPackets.load())));
        std::lock_guard<std::mutex> g(m_infoLock);
        if (!m_client.name.empty() || !m_client.address.empty())
        {
            JsonObject c;
            c.SetNamedValue(L"name", JsonValue::CreateStringValue(m_client.name));
            c.SetNamedValue(L"model", JsonValue::CreateStringValue(m_client.model));
            c.SetNamedValue(L"address", JsonValue::CreateStringValue(m_client.address));
            if (m_connectedAt)
                c.SetNamedValue(L"connectedSec", JsonValue::CreateNumberValue(
                    static_cast<double>((static_cast<int64_t>(GetTickCount64()) - m_connectedAt) / 1000)));
            o.SetNamedValue(L"client", c);
        }
        else
        {
            o.SetNamedValue(L"client", JsonValue::CreateNullValue());
        }
        o.SetNamedValue(L"videoFormat", JsonValue::CreateStringValue(H(m_videoFormat.c_str())));
        o.SetNamedValue(L"audioFormat", JsonValue::CreateStringValue(H(m_audioFormat.c_str())));
        o.SetNamedValue(L"lastError", JsonValue::CreateStringValue(H(m_lastError.c_str())));
        JsonObject stats;
        if (!m_lastStats.empty()) JsonObject::TryParse(H(m_lastStats.c_str()), stats);
        o.SetNamedValue(L"coreStats", stats);
        return o;
    }

    // ---- lifecycle -----------------------------------------------------------------------------------------------------

    bool ReceiverHost::Start()
    {
        std::lock_guard<std::mutex> g(m_lifecycle);
        return StartLocked(false);
    }

    void ReceiverHost::Stop()
    {
        std::lock_guard<std::mutex> g(m_lifecycle);
        StopLocked();
    }

    bool ReceiverHost::Restart()
    {
        std::lock_guard<std::mutex> g(m_lifecycle);
        StopLocked();
        return StartLocked(false);
    }

    void ReceiverHost::Disconnect()
    {
#ifdef XC_HAVE_CASTCORE
        std::lock_guard<std::mutex> g(m_lifecycle);
        if (m_receiver)
        {
            Diag::Log(Diag::Level::Info, "core", "disconnect requested");
            xc_disconnect(static_cast<xc_receiver*>(m_receiver));
        }
#else
        Diag::Log(Diag::Level::Info, "core", "disconnect requested (no castcore built in)");
#endif
    }

#ifdef XC_HAVE_CASTCORE
    namespace
    {
        ReceiverHost* Self(void* ctx) { return static_cast<ReceiverHost*>(ctx); }

        int CbPublish(void* ctx, char const* type, char const* inst, unsigned short port, xc_txt_entry const* txt, size_t n)
        {
            return Self(ctx)->OnCoreMdnsPublish(type, inst, port, txt, n);
        }
        void CbUnpublish(void* ctx) { Self(ctx)->OnCoreMdnsUnpublishAll(); }
        void CbLog(void* ctx, xc_log_level l, char const* m) { Self(ctx)->OnCoreLog(static_cast<int>(l), m); }
        void CbConnected(void* ctx, char const* n, char const* m, char const* a) { Self(ctx)->OnCoreConnected(n, m, a); }
        void CbDisconnected(void* ctx, char const* r) { Self(ctx)->OnCoreDisconnected(r); }
        void CbPin(void* ctx, char const* p) { Self(ctx)->OnCorePin(p); }
        void CbVideoFormat(void* ctx, xc_video_codec c, int w, int h) { Self(ctx)->OnCoreVideoFormat(c, w, h); }
        void CbVideo(void* ctx, xc_video_frame const* f) { Self(ctx)->OnCoreVideo(f); }
        void CbVideoFlush(void* ctx) { Self(ctx)->OnCoreVideoFlush(); }
        void CbAudioFormat(void* ctx, xc_audio_codec c, int r, int ch, uint8_t const* cfg, size_t n)
        {
            Self(ctx)->OnCoreAudioFormat(c, r, ch, cfg, n);
        }
        void CbAudio(void* ctx, xc_audio_packet const* p) { Self(ctx)->OnCoreAudio(p); }
        void CbAudioFlush(void* ctx) { Self(ctx)->OnCoreAudioFlush(); }
        void CbVolume(void* ctx, float db) { Self(ctx)->OnCoreVolume(db); }
        void CbStats(void* ctx, char const* j) { Self(ctx)->OnCoreStats(j); }
    }
#endif

    bool ReceiverHost::StartLocked(bool forceBuiltinMdns)
    {
#ifdef XC_HAVE_CASTCORE
        if (m_receiver) return true;
        auto& s = Settings::Instance();

        std::string name = to_string(s.DeviceName());
        std::string dataDir;
        try { dataDir = to_string(Windows::Storage::ApplicationData::Current().LocalFolder().Path()); }
        catch (...) { Diag::Log(Diag::Level::Error, "core", "LocalFolder path unavailable"); }

        xc_config cfg{};
        cfg.name = name.c_str();
        if (!s.DeviceIdBytes(cfg.device_id)) Diag::Log(Diag::Level::Warn, "core", "deviceId not parseable; using zeros");
        s.ResolutionSize(cfg.display_width, cfg.display_height);
        cfg.display_refresh = 60;
        cfg.max_fps = s.MaxFps();
        cfg.allow_h265 = s.AllowH265() ? 1 : 0;
        cfg.pin_mode = s.PinMode();
        cfg.port = 0;
        cfg.data_dir = dataDir.c_str();
        m_builtinMdns = forceBuiltinMdns || s.MdnsMode() != L"os";
        cfg.builtin_mdns = m_builtinMdns ? 1 : 0;

        xc_callbacks cb{};
        cb.ctx = this;
        cb.mdns_publish = CbPublish;
        cb.mdns_unpublish_all = CbUnpublish;
        cb.on_log = CbLog;
        cb.on_client_connected = CbConnected;
        cb.on_client_disconnected = CbDisconnected;
        cb.on_pin = CbPin;
        cb.on_video_format = CbVideoFormat;
        cb.on_video = CbVideo;
        cb.on_video_flush = CbVideoFlush;
        cb.on_audio_format = CbAudioFormat;
        cb.on_audio = CbAudio;
        cb.on_audio_flush = CbAudioFlush;
        cb.on_volume = CbVolume;
        cb.on_stats = CbStats;

        Diag::Log(Diag::Level::Info, "core",
                  "starting %s (api %d, app built for %d) name='%s' id=%ls display=%dx%d@%d maxFps=%d h265=%d pin=%d mdns=%s data=%s",
                  CastcoreVersion().c_str(), xc_api_version(), XC_API_VERSION, name.c_str(), s.DeviceId().c_str(),
                  cfg.display_width, cfg.display_height, cfg.display_refresh, cfg.max_fps, cfg.allow_h265, cfg.pin_mode,
                  m_builtinMdns ? "builtin" : "os", dataDir.c_str());
        if (xc_api_version() != XC_API_VERSION)
            Diag::Log(Diag::Level::Warn, "core", "castcore API version mismatch: lib %d, header %d", xc_api_version(), XC_API_VERSION);

        m_publishFailed = false;
        auto r = xc_create(&cfg, &cb);
        if (!r)
        {
            std::lock_guard<std::mutex> g(m_infoLock);
            m_lastError = "xc_create failed (see core log)";
            Diag::Log(Diag::Level::Error, "core", "xc_create failed");
            return false;
        }
        int rc = xc_start(r);
        if (!m_builtinMdns && m_publishFailed.load())
        {
            // "os" mode and the OS refused to advertise: fall back to castcore's own responder (contract §3 mdnsMode).
            Diag::Log(Diag::Level::Warn, "mdns", "OS DNS-SD registration failed (xc_start=%d): falling back to builtin mDNS", rc);
            xc_stop(r);
            xc_destroy(r);
            m_dnssd->UnpublishAll();
            return StartLocked(true);
        }
        if (rc != 0)
        {
            Diag::Log(Diag::Level::Error, "core", "xc_start failed: %d", rc);
            xc_stop(r);
            xc_destroy(r);
            m_dnssd->UnpublishAll();
            std::lock_guard<std::mutex> g(m_infoLock);
            m_lastError = "xc_start failed: " + std::to_string(rc);
            return false;
        }
        m_receiver = r;
        {
            std::lock_guard<std::mutex> g(m_infoLock);
            m_lastError.clear();
        }
        SetState(ReceiverState::Idle);
        Diag::Log(Diag::Level::Info, "core", "receiver running (mdns %s)", m_builtinMdns ? "builtin" : "os");
        return true;
#else
        (void)forceBuiltinMdns;
        {
            std::lock_guard<std::mutex> g(m_infoLock);
            m_lastError = "castcore is not built into this app";
        }
        Diag::Log(Diag::Level::Warn, "core", "start: castcore is not built in (XC_HAVE_CASTCORE off): idle screen only");
        return false;
#endif
    }

    void ReceiverHost::StopLocked()
    {
#ifdef XC_HAVE_CASTCORE
        if (!m_receiver) return;
        auto r = static_cast<xc_receiver*>(m_receiver);
        m_receiver = nullptr;
        Diag::Log(Diag::Level::Info, "core", "stopping the receiver");
        xc_stop(r);
        xc_destroy(r);
        m_dnssd->UnpublishAll();
        {
            std::lock_guard<std::mutex> g(m_infoLock);
            m_client = {};
            m_connectedAt = 0;
        }
        SetState(ReceiverState::Stopped);
        Diag::Log(Diag::Level::Info, "core", "receiver stopped");
#endif
    }

    // ---- castcore callbacks (core threads: copy and return) ----------------------------------------------------------

    void ReceiverHost::OnCoreLog(int level, char const* msg)
    {
        Diag::Level l = level <= 0 ? Diag::Level::Debug : level == 1 ? Diag::Level::Info
                      : level == 2 ? Diag::Level::Warn : Diag::Level::Error;
        Diag::LogRaw(l, "core", msg);
    }

    void ReceiverHost::OnCoreConnected(char const* name, char const* model, char const* addr)
    {
        ClientInfo c{ H(name), H(model), H(addr) };
        {
            std::lock_guard<std::mutex> g(m_infoLock);
            m_client = c;
            m_connectedAt = static_cast<int64_t>(GetTickCount64());
            m_videoFormat.clear();
            m_audioFormat.clear();
        }
        m_videoFrames = 0;
        m_videoBytes = 0;
        m_audioPackets = 0;
        Diag::Log(Diag::Level::Info, "core", "client connected name='%s' model='%s' addr=%s", name ? name : "",
                  model ? model : "", addr ? addr : "");
        SetState(ReceiverState::Connecting);
        Raise(m_connected, c, "Connected");
    }

    void ReceiverHost::OnCoreDisconnected(char const* reason)
    {
        int64_t secs = 0;
        {
            std::lock_guard<std::mutex> g(m_infoLock);
            if (m_connectedAt) secs = (static_cast<int64_t>(GetTickCount64()) - m_connectedAt) / 1000;
            m_client = {};
            m_connectedAt = 0;
        }
        Diag::Log(Diag::Level::Info, "core", "client disconnected reason='%s' after %llds frames=%llu audio=%llu",
                  reason ? reason : "", secs, m_videoFrames.load(), m_audioPackets.load());
        if (m_state.load() != ReceiverState::Stopped) SetState(ReceiverState::Idle);
        Raise(m_disconnected, H(reason), "Disconnected");
    }

    void ReceiverHost::OnCorePin(char const* pin)
    {
        Diag::Log(Diag::Level::Info, "core", pin && *pin ? "show PIN" : "hide PIN");
        Raise(m_pin, H(pin), "Pin");
    }

    void ReceiverHost::OnCoreVideoFormat(xc_video_codec codec, int w, int h)
    {
        char buf[64];
        snprintf(buf, sizeof buf, "%s %dx%d", VideoCodecName(codec), w, h);
        {
            std::lock_guard<std::mutex> g(m_infoLock);
            m_videoFormat = buf;
        }
        Diag::Log(Diag::Level::Info, "video", "format %s", buf);
        std::shared_lock<std::shared_mutex> g(m_sinkLock);
        if (m_video) m_video->OnFormat(codec, w, h);
    }

    void ReceiverHost::OnCoreVideo(xc_video_frame const* frame)
    {
        if (!frame) return;
        if (m_videoFrames++ == 0)
        {
            Diag::Log(Diag::Level::Info, "video", "first frame %zu bytes key=%d %dx%d", frame->size, frame->is_keyframe,
                      frame->width, frame->height);
        }
        m_videoBytes += frame->size;
        if (m_state.load() == ReceiverState::Connecting) SetState(ReceiverState::Mirroring);
        std::shared_lock<std::shared_mutex> g(m_sinkLock);
        if (m_video) m_video->OnFrame(*frame);
    }

    void ReceiverHost::OnCoreVideoFlush()
    {
        Diag::Log(Diag::Level::Info, "video", "flush");
        std::shared_lock<std::shared_mutex> g(m_sinkLock);
        if (m_video) m_video->OnFlush();
    }

    void ReceiverHost::OnCoreAudioFormat(xc_audio_codec codec, int rate, int ch, uint8_t const* cfg, size_t n)
    {
        char buf[64];
        snprintf(buf, sizeof buf, "%s %dHz %dch", AudioCodecName(codec), rate, ch);
        {
            std::lock_guard<std::mutex> g(m_infoLock);
            m_audioFormat = buf;
        }
        Diag::Log(Diag::Level::Info, "audio", "format %s config=%zu bytes", buf, n);
        std::shared_lock<std::shared_mutex> g(m_sinkLock);
        if (m_audio) m_audio->OnFormat(codec, rate, ch, cfg, n);
    }

    void ReceiverHost::OnCoreAudio(xc_audio_packet const* packet)
    {
        if (!packet) return;
        if (m_audioPackets++ == 0) Diag::Log(Diag::Level::Info, "audio", "first packet %zu bytes", packet->size);
        std::shared_lock<std::shared_mutex> g(m_sinkLock);
        if (m_audio) m_audio->OnPacket(*packet);
    }

    void ReceiverHost::OnCoreAudioFlush()
    {
        Diag::Log(Diag::Level::Info, "audio", "flush");
        std::shared_lock<std::shared_mutex> g(m_sinkLock);
        if (m_audio) m_audio->OnFlush();
    }

    void ReceiverHost::OnCoreVolume(float db)
    {
        Diag::Log(Diag::Level::Info, "audio", "volume %.1f dB", db);
        std::shared_lock<std::shared_mutex> g(m_sinkLock);
        if (m_audio) m_audio->OnVolume(db);
    }

    void ReceiverHost::OnCoreStats(char const* json)
    {
        {
            std::lock_guard<std::mutex> g(m_infoLock);
            m_lastStats = json ? json : "";
        }
        Raise(m_stats, H(json), "Stats");
    }

    int ReceiverHost::OnCoreMdnsPublish(char const* type, char const* instance, unsigned short port, void const* txt, size_t count)
    {
#ifdef XC_HAVE_CASTCORE
        std::vector<DnssdPublisher::Txt> entries;
        auto t = static_cast<xc_txt_entry const*>(txt);
        for (size_t i = 0; i < count; i++)
            entries.push_back({ t[i].key ? t[i].key : "", t[i].value ? t[i].value : "" });
        auto r = m_dnssd->Publish(type ? type : "", instance ? instance : "", port, entries);
        if (!r.ok) m_publishFailed = true;
        return r.ok ? 0 : -1;
#else
        (void)type; (void)instance; (void)port; (void)txt; (void)count;
        return -1;
#endif
    }

    void ReceiverHost::OnCoreMdnsUnpublishAll()
    {
        m_dnssd->UnpublishAll();
    }
}
