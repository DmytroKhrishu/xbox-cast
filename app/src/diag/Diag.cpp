#include "pch.h"
#include "diag/Diag.h"

#include <algorithm>
#include <deque>
#include <map>

using namespace winrt;

namespace XboxCast
{
    namespace
    {
        constexpr size_t kRingLines = 4000;
        constexpr size_t kMaxLineBytes = 2000;

        struct LogState
        {
            std::mutex lock;
            std::deque<Diag::Line> ring;
            uint64_t seq = 0;
        };
        LogState& State()
        {
            static LogState s;   // function-local: safe for logging from static initializers
            return s;
        }

        struct CommandState
        {
            std::mutex lock;
            event<delegate<hstring>> ev;
            std::map<std::string, Diag::StatusProvider> providers;
        };
        CommandState& Cmd()
        {
            static CommandState s;
            return s;
        }

        char LevelChar(Diag::Level l)
        {
            switch (l)
            {
            case Diag::Level::Debug: return 'D';
            case Diag::Level::Info: return 'I';
            case Diag::Level::Warn: return 'W';
            default: return 'E';
            }
        }

        void Timestamp(char* buf, size_t n)
        {
            SYSTEMTIME st;
            GetLocalTime(&st);
            snprintf(buf, n, "%04u-%02u-%02uT%02u:%02u:%02u.%03u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                     st.wSecond, st.wMilliseconds);
        }

        void Append(Diag::Level level, char const* tag, char const* msg, size_t len)
        {
            while (len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == '\r')) len--;
            if (len > kMaxLineBytes) len = kMaxLineBytes;
            char ts[32];
            Timestamp(ts, sizeof ts);
            std::string line;
            line.reserve(len + 40);
            line += ts;
            line += ' ';
            line += LevelChar(level);
            line += ' ';
            line += tag ? tag : "app";
            line += ' ';
            line.append(msg, len);
            for (auto& c : line) if (c == '\r' || c == '\t') c = ' ';

            {
                auto& s = State();
                std::lock_guard<std::mutex> g(s.lock);
                s.ring.push_back({ ++s.seq, line });
                while (s.ring.size() > kRingLines) s.ring.pop_front();
            }
            line += '\n';
            OutputDebugStringA(line.c_str());
        }
    }

    void Diag::LogV(Level level, char const* tag, char const* fmt, va_list args)
    {
        char buf[kMaxLineBytes + 1];
        int n = vsnprintf(buf, sizeof buf, fmt ? fmt : "", args);
        if (n < 0) { strcpy_s(buf, "(log format error)"); n = static_cast<int>(strlen(buf)); }
        if (static_cast<size_t>(n) >= sizeof buf) n = static_cast<int>(sizeof buf - 1);
        Append(level, tag, buf, static_cast<size_t>(n));
    }

    void Diag::Log(Level level, char const* tag, char const* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        LogV(level, tag, fmt, args);
        va_end(args);
    }

    void Diag::LogRaw(Level level, char const* tag, char const* msg)
    {
        if (!msg) return;
        char const* p = msg;
        while (*p)
        {
            char const* e = strchr(p, '\n');
            size_t len = e ? static_cast<size_t>(e - p) : strlen(p);
            if (len > 0) Append(level, tag, p, len);
            if (!e) break;
            p = e + 1;
        }
    }

    std::vector<Diag::Line> Diag::Since(uint64_t since, size_t max, uint64_t* next)
    {
        std::vector<Line> out;
        auto& s = State();
        std::lock_guard<std::mutex> g(s.lock);
        // Find the first line with seq > since; the ring is contiguous in seq.
        size_t start = 0;
        if (!s.ring.empty())
        {
            uint64_t first = s.ring.front().seq;
            if (since >= first) start = static_cast<size_t>((std::min<uint64_t>)(since - first + 1, s.ring.size()));
        }
        // When more than `max` lines are pending, return the newest `max` (the client is behind; it sees a seq gap).
        if (s.ring.size() - start > max) start = s.ring.size() - max;
        for (size_t i = start; i < s.ring.size(); i++) out.push_back(s.ring[i]);
        if (next) *next = out.empty() ? (std::max)(since, s.seq) : out.back().seq;
        if (next && since > s.seq) *next = s.seq;   // client from a previous app run: restart from the current seq
        return out;
    }

    uint64_t Diag::LastSeq()
    {
        auto& s = State();
        std::lock_guard<std::mutex> g(s.lock);
        return s.seq;
    }

    event_token Diag::Command(delegate<hstring> const& handler)
    {
        auto& c = Cmd();
        std::lock_guard<std::mutex> g(c.lock);
        return c.ev.add(handler);
    }

    void Diag::Command(event_token const& token) noexcept
    {
        auto& c = Cmd();
        std::lock_guard<std::mutex> g(c.lock);
        c.ev.remove(token);
    }

    void Diag::RaiseCommand(hstring const& json)
    {
        // winrt::event snapshots its handler list, so invoking outside the lock is safe and lets handlers unsubscribe.
        auto& c = Cmd();
        try { c.ev(json); }
        catch (hresult_error const& e)
        {
            Log(Level::Error, "diag", "Command handler threw 0x%08x %ls", static_cast<uint32_t>(e.code()), e.message().c_str());
        }
        catch (...)
        {
            Log(Level::Error, "diag", "Command handler threw");
        }
    }

    void Diag::SetStatusProvider(std::string const& name, StatusProvider provider)
    {
        auto& c = Cmd();
        std::lock_guard<std::mutex> g(c.lock);
        if (provider) c.providers[name] = std::move(provider);
        else c.providers.erase(name);
    }
}

// The status builder and the HTTP server live in DevServer.cpp; they need the status providers.
namespace XboxCast
{
    std::vector<std::pair<std::string, Diag::StatusProvider>> DiagStatusProviders()
    {
        auto& c = Cmd();
        std::lock_guard<std::mutex> g(c.lock);
        return { c.providers.begin(), c.providers.end() };
    }
}
