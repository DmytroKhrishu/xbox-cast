#pragma once
// DnssdPublisher: advertises DNS-SD services through the OS (Windows.Networking.ServiceDiscovery.Dnssd).
// Owner: platform. Used by ReceiverHost in mdnsMode "os" (experimental; "builtin" = castcore's own responder is the
// default, see docs/contract.md §3) and by the dev probe /cmd {"mdnsTest":true}. No castcore dependency.
//
// The WinRT API only registers a service through a StreamSocketListener. We bind one on an ephemeral port purely to
// satisfy the API, then set instance.Port() to the port castcore actually listens on (Winsock), and log the
// registration status plus both ports so the console tells us whether the advertised port is honored.
//
// Publish() blocks on the async registration: call it from a castcore or thread-pool thread, never the UI thread.

#include <winrt/Windows.Networking.ServiceDiscovery.Dnssd.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace XboxCast
{
    class DnssdPublisher
    {
    public:
        struct Txt { std::string key; std::string value; };

        struct Result
        {
            bool ok = false;
            std::string status;          // DnssdRegistrationStatus name, or the exception text
            std::string fullName;        // "<instance>.<type>.local"
            uint16_t advertisedPort = 0; // instance.Port() after registration
            uint16_t listenerPort = 0;   // the ephemeral StreamSocketListener port
            std::string hostName;        // host name the instance was created with
        };

        // serviceType: "_airplay._tcp" | "_raop._tcp" | "_xboxcast-test._tcp" ...; instanceName: "Xbox", "0211223344556677@Xbox".
        Result Publish(std::string const& serviceType, std::string const& instanceName, uint16_t port,
                       std::vector<Txt> const& txt);
        void UnpublishAll();             // non-blocking: closes the listeners (deregisters their services)
        size_t Count();

        // Dev probe: registers "xbox-cast-test._xboxcast-test._tcp.local" on a thread-pool thread and logs the outcome
        // (tag mdns). enable=false removes it.
        static void SelfTest(bool enable);

    private:
        struct Entry
        {
            winrt::Windows::Networking::Sockets::StreamSocketListener listener{ nullptr };
            winrt::Windows::Networking::ServiceDiscovery::Dnssd::DnssdServiceInstance instance{ nullptr };
            std::string fullName;
        };
        std::mutex m_lock;
        std::vector<Entry> m_entries;
    };
}
