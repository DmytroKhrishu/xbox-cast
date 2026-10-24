#include "pch.h"
#include "core/DnssdPublisher.h"
#include "diag/Diag.h"

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Networking;
using namespace Windows::Networking::Connectivity;
using namespace Windows::Networking::ServiceDiscovery::Dnssd;
using namespace Windows::Networking::Sockets;

namespace XboxCast
{
    namespace
    {
        char const* StatusName(DnssdRegistrationStatus s)
        {
            switch (s)
            {
            case DnssdRegistrationStatus::Success: return "Success";
            case DnssdRegistrationStatus::InvalidServiceName: return "InvalidServiceName";
            case DnssdRegistrationStatus::ServerError: return "ServerError";
            case DnssdRegistrationStatus::SecurityError: return "SecurityError";
            default: return "Unknown";
            }
        }

        // The OS answers A/AAAA for its own host name, so in "os" mode the instance points at it.
        HostName LocalDomainHostName()
        {
            try
            {
                for (auto const& h : NetworkInformation::GetHostNames())
                    if (h.Type() == HostNameType::DomainName) return h;
            }
            catch (...) {}
            return nullptr;
        }
    }

    DnssdPublisher::Result DnssdPublisher::Publish(std::string const& serviceType, std::string const& instanceName,
                                                   uint16_t port, std::vector<Txt> const& txt)
    {
        Result r;
        r.fullName = instanceName + "." + serviceType + ".local";
        try
        {
            StreamSocketListener listener;
            listener.Control().KeepAlive(false);
            listener.ConnectionReceived([](StreamSocketListener const&, StreamSocketListenerConnectionReceivedEventArgs const& a)
            {
                // Nothing is served here: the real service is castcore's Winsock socket on the advertised port.
                Diag::Log(Diag::Level::Warn, "mdns", "connection on the DNS-SD placeholder listener; closing it");
                a.Socket().Close();
            });
            listener.BindServiceNameAsync(L"").get();
            r.listenerPort = static_cast<uint16_t>(_wtoi(listener.Information().LocalPort().c_str()));

            auto host = LocalDomainHostName();
            if (!host) throw hresult_error(E_FAIL, L"no local DomainName host name to advertise");
            r.hostName = to_string(host.CanonicalName());
            DnssdServiceInstance instance(to_hstring(r.fullName), host, port);
            auto attrs = instance.TextAttributes();
            for (auto const& t : txt) attrs.Insert(to_hstring(t.key), to_hstring(t.value));
            instance.Port(port);

            auto res = instance.RegisterStreamSocketListenerAsync(listener).get();
            r.status = StatusName(res.Status());
            r.advertisedPort = instance.Port();
            r.ok = res.Status() == DnssdRegistrationStatus::Success;
            std::string ip = res.IPAddress() ? to_string(res.IPAddress().CanonicalName()) : std::string("-");
            Diag::Log(r.ok ? Diag::Level::Info : Diag::Level::Error, "mdns",
                      "register %s host=%s status=%s nameChanged=%d ip=%s advertisedPort=%u wanted=%u listenerPort=%u txt=%zu",
                      r.fullName.c_str(), r.hostName.c_str(), r.status.c_str(), res.HasInstanceNameChanged() ? 1 : 0,
                      ip.c_str(), r.advertisedPort, port, r.listenerPort, txt.size());
            if (r.ok)
            {
                std::lock_guard<std::mutex> g(m_lock);
                m_entries.push_back({ listener, instance, r.fullName });
            }
            else
            {
                listener.Close();
            }
        }
        catch (hresult_error const& e)
        {
            char buf[256];
            snprintf(buf, sizeof buf, "0x%08x %ls", static_cast<uint32_t>(e.code()), e.message().c_str());
            r.status = buf;
            Diag::Log(Diag::Level::Error, "mdns", "register %s failed: %s", r.fullName.c_str(), buf);
        }
        catch (std::exception const& e)
        {
            r.status = e.what();
            Diag::Log(Diag::Level::Error, "mdns", "register %s failed: %s", r.fullName.c_str(), e.what());
        }
        return r;
    }

    void DnssdPublisher::UnpublishAll()
    {
        std::vector<Entry> entries;
        {
            std::lock_guard<std::mutex> g(m_lock);
            entries.swap(m_entries);
        }
        for (auto& e : entries)
        {
            try { e.listener.Close(); }
            catch (...) {}
            Diag::Log(Diag::Level::Info, "mdns", "unregistered %s", e.fullName.c_str());
        }
    }

    size_t DnssdPublisher::Count()
    {
        std::lock_guard<std::mutex> g(m_lock);
        return m_entries.size();
    }

    void DnssdPublisher::SelfTest(bool enable)
    {
        static DnssdPublisher s_test;
        if (!enable)
        {
            s_test.UnpublishAll();
            return;
        }
        Windows::System::Threading::ThreadPool::RunAsync([](auto&&)
        {
            s_test.UnpublishAll();
            Diag::Log(Diag::Level::Info, "mdns", "self-test: registering a dummy _xboxcast-test._tcp service");
            auto r = s_test.Publish("_xboxcast-test._tcp", "xbox-cast-test", Diag::kServerPort,
                                    { { "v", "1" }, { "app", "xbox-cast" } });
            Diag::Log(r.ok ? Diag::Level::Info : Diag::Level::Error, "mdns",
                      "self-test result ok=%d status=%s advertisedPort=%u listenerPort=%u (browse: dns-sd -B _xboxcast-test._tcp)",
                      r.ok ? 1 : 0, r.status.c_str(), r.advertisedPort, r.listenerPort);
        });
    }
}
