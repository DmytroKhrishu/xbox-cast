#include "pch.h"
#include "diag/Diag.h"
#include "core/DnssdPublisher.h"
#include "core/ReceiverHost.h"
#include "settings/Settings.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <thread>

using namespace winrt;
using namespace Windows::Data::Json;

// Dev HTTP server (docs/contract.md §4): TCP 7099, LAN peers only, one connection at a time, Connection: close.
//   GET  /status            -> JSON (version, state, client, settings, receiver, media, memory)
//   GET  /log?since=<seq>   -> {"seq":N,"lines":["..."]}  (since=0: everything in the ring)
//   POST /cmd  <json>       -> {"ok":true} | {"ok":false,"error":"..."}; body forwarded verbatim via Diag::Command
namespace XboxCast
{
    std::vector<std::pair<std::string, Diag::StatusProvider>> DiagStatusProviders();   // Diag.cpp

    namespace
    {
        constexpr size_t kMaxRequest = 64 * 1024;
        constexpr size_t kMaxLogLinesPerRequest = 2000;

        std::mutex g_serverLock;
        std::thread g_thread;
        std::atomic<bool> g_stop{ false };
        std::atomic<bool> g_running{ false };
        SOCKET g_listen = INVALID_SOCKET;

        std::string Narrow(hstring const& s) { return to_string(s); }

        bool IsLanPeer(sockaddr_in const& a)
        {
            uint32_t ip = ntohl(a.sin_addr.s_addr);
            uint8_t b0 = ip >> 24, b1 = (ip >> 16) & 0xff;
            return b0 == 10 || b0 == 127 || (b0 == 172 && (b1 & 0xf0) == 16) || (b0 == 192 && b1 == 168) ||
                   (b0 == 169 && b1 == 254) || (b0 == 100 && (b1 & 0xc0) == 64);
        }

        void SendAll(SOCKET s, std::string const& data)
        {
            size_t off = 0;
            while (off < data.size())
            {
                int n = send(s, data.data() + off, static_cast<int>((std::min)(data.size() - off, size_t(1) << 20)), 0);
                if (n <= 0) return;
                off += static_cast<size_t>(n);
            }
        }

        void Respond(SOCKET s, int code, char const* reason, std::string const& body, char const* type = "application/json")
        {
            char head[256];
            snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\nContent-Type: %s; charset=utf-8\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     code, reason, type, body.size());
            SendAll(s, head + body);
        }

        std::string ErrorJson(std::string const& msg)
        {
            JsonObject o;
            o.SetNamedValue(L"ok", JsonValue::CreateBooleanValue(false));
            o.SetNamedValue(L"error", JsonValue::CreateStringValue(to_hstring(msg)));
            return Narrow(o.Stringify());
        }

        std::string HandleLog(std::string const& query)
        {
            uint64_t since = 0;
            auto p = query.find("since=");
            if (p != std::string::npos) since = _strtoui64(query.c_str() + p + 6, nullptr, 10);
            uint64_t next = 0;
            auto lines = Diag::Since(since, kMaxLogLinesPerRequest, &next);
            JsonArray arr;
            for (auto const& l : lines) arr.Append(JsonValue::CreateStringValue(to_hstring(l.text)));
            JsonObject o;
            o.SetNamedValue(L"seq", JsonValue::CreateNumberValue(static_cast<double>(next)));
            o.SetNamedValue(L"first", JsonValue::CreateNumberValue(lines.empty() ? 0.0 : static_cast<double>(lines.front().seq)));
            o.SetNamedValue(L"lines", arr);
            return Narrow(o.Stringify());
        }

        // Handles the platform-level keys, then forwards the body verbatim to the UI (Diag::Command).
        std::string HandleCmd(std::string const& body, int& code)
        {
            JsonObject cmd{ nullptr };
            if (!JsonObject::TryParse(to_hstring(body), cmd))
            {
                code = 400;
                return ErrorJson("body is not a JSON object");
            }
            Diag::Log(Diag::Level::Info, "diag", "cmd %s", body.size() > 300 ? (body.substr(0, 300) + "...").c_str() : body.c_str());
            std::vector<std::string> errors;
            auto& s = Settings::Instance();

            if (cmd.HasKey(L"viewMode"))
            {
                std::wstring err;
                if (!s.Set(Settings::kViewMode, cmd.GetNamedValue(L"viewMode"), &err)) errors.push_back(to_string(err));
            }
            if (cmd.HasKey(L"zoom"))
            {
                std::wstring err;
                if (!s.Set(Settings::kZoom, cmd.GetNamedValue(L"zoom"), &err)) errors.push_back(to_string(err));
            }
            if (cmd.HasKey(L"setting"))
            {
                auto v = cmd.GetNamedValue(L"setting");
                if (v.ValueType() != JsonValueType::Object) errors.push_back("setting must be an object");
                else
                {
                    for (auto const& kv : v.GetObject())
                    {
                        std::wstring err;
                        if (!s.Set(kv.Key(), kv.Value(), &err)) errors.push_back(to_string(err));
                    }
                }
            }
            if (cmd.GetNamedBoolean(L"resetSettings", false)) s.ResetAll();
            if (cmd.GetNamedBoolean(L"disconnect", false)) ReceiverHost::Instance().Disconnect();
            if (cmd.GetNamedBoolean(L"restartReceiver", false))
            {
                Windows::System::Threading::ThreadPool::RunAsync([](auto&&) { ReceiverHost::Instance().Restart(); });
            }
            if (cmd.HasKey(L"mdnsTest"))
            {
                auto v = cmd.GetNamedValue(L"mdnsTest");
                DnssdPublisher::SelfTest(v.ValueType() != JsonValueType::Boolean || v.GetBoolean());
            }

            Diag::RaiseCommand(to_hstring(body));

            JsonObject o;
            o.SetNamedValue(L"ok", JsonValue::CreateBooleanValue(errors.empty()));
            if (!errors.empty())
            {
                std::string all;
                for (auto const& e : errors) all += (all.empty() ? "" : "; ") + e;
                o.SetNamedValue(L"error", JsonValue::CreateStringValue(to_hstring(all)));
                code = 400;
            }
            return Narrow(o.Stringify());
        }

        void HandleClient(SOCKET c)
        {
            DWORD timeout = 5000;
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char const*>(&timeout), sizeof timeout);
            setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char const*>(&timeout), sizeof timeout);

            std::string req;
            char buf[4096];
            size_t headerEnd = std::string::npos;
            while (headerEnd == std::string::npos && req.size() < kMaxRequest)
            {
                int n = recv(c, buf, sizeof buf, 0);
                if (n <= 0) return;
                req.append(buf, static_cast<size_t>(n));
                headerEnd = req.find("\r\n\r\n");
            }
            if (headerEnd == std::string::npos) { Respond(c, 431, "Request Header Fields Too Large", ErrorJson("headers too large")); return; }

            std::string head = req.substr(0, headerEnd);
            std::string lower = head;
            for (auto& ch : lower) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            size_t contentLength = 0;
            auto cl = lower.find("\r\ncontent-length:");
            if (cl != std::string::npos) contentLength = static_cast<size_t>(strtoul(lower.c_str() + cl + 17, nullptr, 10));
            if (contentLength > kMaxRequest) { Respond(c, 413, "Payload Too Large", ErrorJson("body too large")); return; }
            std::string body = req.substr(headerEnd + 4);
            while (body.size() < contentLength)
            {
                int n = recv(c, buf, sizeof buf, 0);
                if (n <= 0) break;
                body.append(buf, static_cast<size_t>(n));
            }
            if (body.size() > contentLength) body.resize(contentLength);

            auto sp1 = head.find(' ');
            auto sp2 = sp1 == std::string::npos ? std::string::npos : head.find(' ', sp1 + 1);
            if (sp2 == std::string::npos) { Respond(c, 400, "Bad Request", ErrorJson("bad request line")); return; }
            std::string method = head.substr(0, sp1);
            std::string target = head.substr(sp1 + 1, sp2 - sp1 - 1);
            std::string path = target, query;
            auto q = target.find('?');
            if (q != std::string::npos) { path = target.substr(0, q); query = target.substr(q + 1); }

            try
            {
                if (method == "GET" && path == "/status")
                    Respond(c, 200, "OK", Narrow(Diag::BuildStatus().Stringify()));
                else if (method == "GET" && path == "/log")
                    Respond(c, 200, "OK", HandleLog(query));
                else if (method == "POST" && path == "/cmd")
                {
                    int code = 200;
                    auto out = HandleCmd(body, code);
                    Respond(c, code, code == 200 ? "OK" : "Bad Request", out);
                }
                else if (method == "GET" && path == "/")
                    Respond(c, 200, "OK", "xbox-cast dev server: GET /status, GET /log?since=<seq>, POST /cmd\n", "text/plain");
                else
                    Respond(c, 404, "Not Found", ErrorJson("unknown endpoint " + method + " " + path));
            }
            catch (hresult_error const& e)
            {
                Diag::Log(Diag::Level::Error, "diag", "%s %s failed: 0x%08x %ls", method.c_str(), path.c_str(),
                          static_cast<uint32_t>(e.code()), e.message().c_str());
                Respond(c, 500, "Internal Server Error", ErrorJson(Narrow(e.message())));
            }
            catch (std::exception const& e)
            {
                Respond(c, 500, "Internal Server Error", ErrorJson(e.what()));
            }
        }

        void ServerLoop()
        {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            Diag::Log(Diag::Level::Info, "diag", "dev server listening on TCP %u (LAN only)", Diag::kServerPort);
            while (!g_stop.load())
            {
                fd_set rd;
                FD_ZERO(&rd);
                FD_SET(g_listen, &rd);
                timeval tv{ 0, 250 * 1000 };
                int n = select(0, &rd, nullptr, nullptr, &tv);
                if (n == SOCKET_ERROR)
                {
                    if (g_stop.load()) break;
                    Diag::Log(Diag::Level::Error, "diag", "select failed: %d", WSAGetLastError());
                    Sleep(500);
                    continue;
                }
                if (n == 0) continue;
                sockaddr_in peer{};
                int len = sizeof peer;
                SOCKET c = accept(g_listen, reinterpret_cast<sockaddr*>(&peer), &len);
                if (c == INVALID_SOCKET) continue;
                char ip[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
                if (!IsLanPeer(peer))
                {
                    Diag::Log(Diag::Level::Warn, "diag", "rejected non-LAN peer %s", ip);
                    closesocket(c);
                    continue;
                }
                HandleClient(c);
                shutdown(c, SD_SEND);
                closesocket(c);
            }
            Diag::Log(Diag::Level::Info, "diag", "dev server stopped");
            winrt::uninit_apartment();
        }
    }

    void Diag::StartServer()
    {
        std::lock_guard<std::mutex> g(g_serverLock);
        if (g_running.load()) return;
        if (!Settings::Instance().DevServer())
        {
            Log(Level::Info, "diag", "dev server disabled (devServer=false)");
            return;
        }
        WSADATA wsa;
        if (int e = WSAStartup(MAKEWORD(2, 2), &wsa)) { Log(Level::Error, "diag", "WSAStartup failed: %d", e); return; }
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
        {
            Log(Level::Error, "diag", "socket failed: %d", WSAGetLastError());
            WSACleanup();
            return;
        }
        BOOL excl = TRUE;   // a previous instance's lingering socket must not share the port
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char const*>(&excl), sizeof excl);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(kServerPort);
        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == SOCKET_ERROR || listen(s, 8) == SOCKET_ERROR)
        {
            Log(Level::Error, "diag", "bind/listen on %u failed: %d", kServerPort, WSAGetLastError());
            closesocket(s);
            WSACleanup();
            return;
        }
        g_listen = s;
        g_stop = false;
        g_running = true;
        g_thread = std::thread(ServerLoop);
    }

    void Diag::StopServer()
    {
        std::lock_guard<std::mutex> g(g_serverLock);
        if (!g_running.load()) return;
        g_stop = true;
        if (g_thread.joinable()) g_thread.join();   // loop wakes within 250 ms; a client in progress has 5 s timeouts
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        WSACleanup();
        g_running = false;
    }

    bool Diag::ServerRunning() { return g_running.load(); }

    JsonObject Diag::BuildStatus()
    {
        JsonObject o;
        try
        {
            auto pkg = Windows::ApplicationModel::Package::Current();
            auto v = pkg.Id().Version();
            o.SetNamedValue(L"version", JsonValue::CreateStringValue(to_hstring(v.Major) + L"." + to_hstring(v.Minor) + L"." +
                                                                     to_hstring(v.Build) + L"." + to_hstring(v.Revision)));
            o.SetNamedValue(L"package", JsonValue::CreateStringValue(pkg.Id().Name()));
        }
        catch (...) {}
        auto& host = ReceiverHost::Instance();
        o.SetNamedValue(L"state", JsonValue::CreateStringValue(to_hstring(ReceiverStateName(host.State()))));
        auto receiver = host.StatusJson();
        o.SetNamedValue(L"client", JsonValue::Parse(receiver.GetNamedValue(L"client").Stringify()));
        receiver.Remove(L"client");
        o.SetNamedValue(L"receiver", receiver);
        o.SetNamedValue(L"devServer", JsonValue::CreateBooleanValue(ServerRunning()));
        o.SetNamedValue(L"settings", Settings::Instance().ToJson());
        o.SetNamedValue(L"logSeq", JsonValue::CreateNumberValue(static_cast<double>(LastSeq())));
        o.SetNamedValue(L"uptimeSec", JsonValue::CreateNumberValue(static_cast<double>(GetTickCount64() / 1000)));
        try
        {
            JsonObject mem;
            using Windows::System::MemoryManager;
            mem.SetNamedValue(L"usageMB", JsonValue::CreateNumberValue(static_cast<double>(MemoryManager::AppMemoryUsage() >> 20)));
            mem.SetNamedValue(L"limitMB", JsonValue::CreateNumberValue(static_cast<double>(MemoryManager::AppMemoryUsageLimit() >> 20)));
            auto report = MemoryManager::GetAppMemoryReport();
            mem.SetNamedValue(L"peakMB", JsonValue::CreateNumberValue(static_cast<double>(report.PeakPrivateCommitUsage() >> 20)));
            o.SetNamedValue(L"memory", mem);
        }
        catch (...) {}
        for (auto const& p : DiagStatusProviders())
        {
            try
            {
                auto section = p.second();
                o.SetNamedValue(to_hstring(p.first), section ? IJsonValue(section) : IJsonValue(JsonValue::CreateNullValue()));
            }
            catch (hresult_error const& e)
            {
                o.SetNamedValue(to_hstring(p.first), JsonValue::CreateStringValue(L"error: " + e.message()));
            }
            catch (...)
            {
                o.SetNamedValue(to_hstring(p.first), JsonValue::CreateStringValue(L"error"));
            }
        }
        return o;
    }
}
