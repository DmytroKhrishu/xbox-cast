#pragma once
// ReceiverHost (docs/contract.md §3). Owner: platform. Process-wide singleton that owns the castcore receiver
// (xc_receiver) and the DNS-SD publisher, maps Settings -> xc_config and castcore callbacks -> Diag logs, sinks and events.
//
// Threading (read this before subscribing):
//   * Every event below is raised on a castcore or thread-pool thread, NEVER the UI thread. Handlers must return quickly
//     and marshal to their CoreDispatcher (Dispatcher().RunAsync(...)) before touching XAML.
//   * Sink methods are called on castcore threads; they must copy and return immediately (contract §1, ~1 ms budget).
//     SetSinks waits for in-flight sink calls to return, so after SetSinks(nullptr, nullptr) returns the old sinks are
//     never called again (safe to destroy them).
//   * StateChanged fires while Start/Stop hold the lifecycle lock: never call Start/Stop/Restart/Disconnect from a
//     StateChanged handler synchronously (post it to the Dispatcher or the thread pool).
//   * Start/Stop/Restart block (socket setup, DNS-SD registration). Call them off the UI thread when you can
//     (App uses the thread pool for Start; Stop on suspend is synchronous under the suspension deferral).
//
// Without castcore (XC_HAVE_CASTCORE undefined, until native/ builds in CI) everything compiles and Start() only logs that
// the receiver is not built in: State() stays Stopped and no events fire.

#if __has_include(<castcore.h>)
#include <cstddef>
#include <cstdint>
extern "C" {
#include <castcore.h>
}
#else
// castcore.h is not available yet (native/ not built): the public types the sinks need, copied verbatim from the
// contract (docs/contract.md §1). Only used when the real header is absent.
#include <cstddef>
#include <cstdint>
extern "C" {
typedef enum { XC_LOG_DEBUG, XC_LOG_INFO, XC_LOG_WARN, XC_LOG_ERROR } xc_log_level;
typedef enum { XC_VIDEO_H264 = 1, XC_VIDEO_H265 = 2 } xc_video_codec;
typedef enum { XC_AUDIO_AAC_ELD = 1, XC_AUDIO_ALAC = 2, XC_AUDIO_AAC_LC = 3, XC_AUDIO_PCM = 4 } xc_audio_codec;
typedef struct {
    const uint8_t *data;
    size_t         size;
    int64_t        pts_ns;
    int            is_keyframe;
    xc_video_codec codec;
    int            width, height;
} xc_video_frame;
typedef struct {
    const uint8_t *data;
    size_t         size;
    int64_t        pts_ns;
} xc_audio_packet;
}
#endif

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>

namespace XboxCast
{
    struct IVideoSink
    {
        virtual void OnFormat(xc_video_codec codec, int w, int h) = 0;
        virtual void OnFrame(const xc_video_frame& frame) = 0;
        virtual void OnFlush() = 0;
    };

    struct IAudioSink
    {
        virtual void OnFormat(xc_audio_codec codec, int rate, int ch, const uint8_t* cfg, size_t n) = 0;
        virtual void OnPacket(const xc_audio_packet& packet) = 0;
        virtual void OnFlush() = 0;
        virtual void OnVolume(float db) = 0;
    };

    // Stopped: receiver not running (not built in, start failed, or suspended).
    // Idle: advertising, no client. Connecting: a client connected, no video yet. Mirroring: video is flowing.
    enum class ReceiverState { Stopped, Idle, Connecting, Mirroring };
    char const* ReceiverStateName(ReceiverState s);   // "stopped" | "idle" | "connecting" | "mirroring"

    struct ClientInfo
    {
        winrt::hstring name;     // e.g. "iPhone" (whatever the sender reports)
        winrt::hstring model;    // e.g. "iPhone16,1"
        winrt::hstring address;  // sender IP
    };

    class DnssdPublisher;

    class ReceiverHost
    {
    public:
        static ReceiverHost& Instance();

        bool Start();        // no-op if running; returns true when the receiver runs afterwards
        void Stop();         // unpublish + close everything (state -> Stopped)
        bool Restart();      // Stop + Start (settings that change the advertisement do this automatically)
        void Disconnect();   // drop the current client, keep advertising

        void SetSinks(IVideoSink* video, IAudioSink* audio);

        ReceiverState State() const { return m_state.load(); }
        ClientInfo Client();
        bool HasCastcore() const;
        std::string CastcoreVersion() const;    // "" without castcore
        std::string LastError();
        winrt::Windows::Data::Json::JsonObject StatusJson();   // /status "receiver" section

        // Events (any thread, see the threading note at the top).
        winrt::event_token Connected(winrt::delegate<ClientInfo> const& h) { return m_connected.add(h); }
        void Connected(winrt::event_token const& t) noexcept { m_connected.remove(t); }
        winrt::event_token Disconnected(winrt::delegate<winrt::hstring> const& h) { return m_disconnected.add(h); }  // reason
        void Disconnected(winrt::event_token const& t) noexcept { m_disconnected.remove(t); }
        winrt::event_token Pin(winrt::delegate<winrt::hstring> const& h) { return m_pin.add(h); }   // "" = hide the PIN
        void Pin(winrt::event_token const& t) noexcept { m_pin.remove(t); }
        winrt::event_token Stats(winrt::delegate<winrt::hstring> const& h) { return m_stats.add(h); }  // castcore JSON, ~1 Hz
        void Stats(winrt::event_token const& t) noexcept { m_stats.remove(t); }
        winrt::event_token StateChanged(winrt::delegate<ReceiverState> const& h) { return m_stateChanged.add(h); }
        void StateChanged(winrt::event_token const& t) noexcept { m_stateChanged.remove(t); }

        // castcore callback trampolines (public so the C callbacks in ReceiverHost.cpp can reach them).
        void OnCoreLog(int level, char const* msg);
        void OnCoreConnected(char const* name, char const* model, char const* addr);
        void OnCoreDisconnected(char const* reason);
        void OnCorePin(char const* pin);
        void OnCoreVideoFormat(xc_video_codec codec, int w, int h);
        void OnCoreVideo(xc_video_frame const* frame);
        void OnCoreVideoFlush();
        void OnCoreAudioFormat(xc_audio_codec codec, int rate, int ch, uint8_t const* cfg, size_t n);
        void OnCoreAudio(xc_audio_packet const* packet);
        void OnCoreAudioFlush();
        void OnCoreVolume(float db);
        void OnCoreStats(char const* json);
        int OnCoreMdnsPublish(char const* type, char const* instance, unsigned short port, void const* txt, size_t count);
        void OnCoreMdnsUnpublishAll();

    private:
        ReceiverHost();
        void SetState(ReceiverState s);
        void OnSettingChanged(winrt::hstring const& key);
        bool StartLocked(bool forceBuiltinMdns);
        void StopLocked();

        std::mutex m_lifecycle;                 // serializes Start/Stop/Restart
        std::shared_mutex m_sinkLock;           // shared: sink calls; exclusive: SetSinks
        IVideoSink* m_video = nullptr;
        IAudioSink* m_audio = nullptr;
        std::atomic<ReceiverState> m_state{ ReceiverState::Stopped };
        void* m_receiver = nullptr;             // xc_receiver*
        std::unique_ptr<DnssdPublisher> m_dnssd;
        std::atomic<bool> m_publishFailed{ false };
        bool m_builtinMdns = true;              // effective mode of the running receiver
        std::atomic<uint32_t> m_restartGen{ 0 };

        std::mutex m_infoLock;                  // guards the fields below
        ClientInfo m_client;
        std::string m_lastError;
        std::string m_lastStats;
        std::string m_videoFormat;
        std::string m_audioFormat;
        int64_t m_connectedAt = 0;              // GetTickCount64 ms, 0 = none
        std::atomic<uint64_t> m_videoFrames{ 0 };
        std::atomic<uint64_t> m_videoBytes{ 0 };
        std::atomic<uint64_t> m_audioPackets{ 0 };

        winrt::event<winrt::delegate<ClientInfo>> m_connected;
        winrt::event<winrt::delegate<winrt::hstring>> m_disconnected;
        winrt::event<winrt::delegate<winrt::hstring>> m_pin;
        winrt::event<winrt::delegate<winrt::hstring>> m_stats;
        winrt::event<winrt::delegate<ReceiverState>> m_stateChanged;
    };
}
