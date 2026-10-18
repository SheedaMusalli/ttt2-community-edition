// ENet-backed peer transport. Channel 0 is sequenced-unreliable and channel 1 is reliable,
// ordered. Peer IDs remain owned by the XLive/session layer; this module maps them to IPv4.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "enet/enet.h"
#include "net_transport.h"
#include "steam_net.h"
#include "../net/net_adapter.h"
#include "../net/net_telemetry.h"
#include "../net/virtual_lan.h"
#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

namespace net {
namespace {
constexpr uint64_t kLoopTag = 0x4C4F4F5000000000ull;
constexpr uint64_t kEndpointTag = 0x4950563400000000ull; // "IPV4"
constexpr size_t kMaxPeers = 64;
constexpr enet_uint8 kInputChannel = uint8_t(Channel::CHANNEL_UNRELIABLE_SEQUENCED);
constexpr enet_uint8 kControlChannel = uint8_t(Channel::CHANNEL_RELIABLE_ORDERED);

std::mutex g_mutex;
std::mutex g_hostMutex;
std::atomic<bool> g_started{ false };
std::atomic<uint64_t> g_local{ 0 };
Receiver g_receiver = nullptr;
ENetHost* g_host = nullptr;
bool g_steam = false;
Adapter g_adapter;
std::atomic<float> g_simRift{0};
std::atomic<bool> g_simValid{false};
uint16_t g_requestedPort = 0;
std::map<uint64_t, ENetAddress> g_addresses;
std::map<uint64_t, ENetPeer*> g_peers;
std::map<ENetPeer*, uint64_t> g_peerIds;
struct PendingPacket { Channel channel{}; std::vector<uint8_t> bytes; };
std::map<uint64_t, std::deque<PendingPacket>> g_pendingPackets;
struct RelayRoute { sockaddr_in endpoint{}; uint8_t token[16]{}; uint8_t role = 0; std::chrono::steady_clock::time_point firstSend{}; bool hasSent = false; bool fallback = false; };
std::map<uint64_t, RelayRoute> g_relayRoutes;
SOCKET g_relaySocket = INVALID_SOCKET;

void FromSteam(uint64_t peer, bool reliable, const uint8_t* data, size_t size)
{
    Receiver receiver = g_receiver;
    if (receiver) receiver(peer, reliable ? Channel::Reliable : Channel::Unreliable, data, size);
}

uint64_t IdForAddress(const ENetAddress& address)
{
    for (const auto& [id, endpoint] : g_addresses)
        if (endpoint.host == address.host && endpoint.port == address.port) return id;
    if(VirtualLanEnabled())return kEndpointTag ^ (uint64_t(ntohl(address.host))<<16) ^ address.port;
    if ((address.host & 0xFFu) == 127u) return kLoopTag | address.port;
    uint64_t id = kEndpointTag ^ (uint64_t(address.host) << 16) ^ address.port;
    if (!id) id = kEndpointTag | 1;
    return id;
}

void ReceiveLoop()
{
    ENetEvent event{};
    while (g_started.load())
    {
        if (g_relaySocket != INVALID_SOCKET)
        {
            for (int i = 0; i < 32; ++i)
            {
                uint8_t wire[65536]; sockaddr_in from{}; int fromSize = sizeof(from);
                const int n = recvfrom(g_relaySocket, reinterpret_cast<char*>(wire), sizeof(wire), 0, reinterpret_cast<sockaddr*>(&from), &fromSize);
                if (n == SOCKET_ERROR) break;
                if (n < 18) continue;
                Receiver receiver = nullptr; uint64_t peer = 0; Channel channel{}; bool valid = false;
                {
                    std::lock_guard lock(g_mutex);
                    for (const auto& [id, route] : g_relayRoutes)
                    {
                        if (route.endpoint.sin_addr.s_addr != from.sin_addr.s_addr || route.endpoint.sin_port != from.sin_port ||
                            route.role == wire[16] || wire[16] < 1 || wire[16] > 2) continue;
                        unsigned diff = 0; for (size_t b = 0; b < 16; ++b) diff |= route.token[b] ^ wire[b];
                        if (diff) continue;
                        peer = id; channel = wire[17] == kControlChannel ? Channel::Reliable : Channel::Unreliable;
                        receiver = g_receiver; valid = true; break;
                    }
                }
                if (valid && receiver && n > 18) receiver(peer, channel, wire + 18, size_t(n - 18));
            }
        }
        int result = 0;
        {
            std::lock_guard hostLock(g_hostMutex);
            result = enet_host_service(g_host, &event, 10);
        }
        if (result <= 0) continue;
        if (event.type == ENET_EVENT_TYPE_CONNECT)
        {
            uint64_t id = 0;
            std::deque<PendingPacket> pending;
            {
                std::lock_guard lock(g_mutex);
                id = IdForAddress(event.peer->address);
                g_peers[id] = event.peer;
                g_peerIds[event.peer] = id;
                char ip[16]{}; enet_address_get_host_ip(&event.peer->address,ip,sizeof(ip));
                const auto endpoint=std::string(ip)+":"+std::to_string(event.peer->address.port);
                telemetry::SetPeer(endpoint.c_str(),telemetry::State::Connected);
                auto route = g_relayRoutes.find(id);
                if (route != g_relayRoutes.end()) route->second.hasSent = false;
                auto queued = g_pendingPackets.find(id);
                if (queued != g_pendingPackets.end())
                {
                    pending.swap(queued->second);
                    g_pendingPackets.erase(queued);
                }
            }
            if (!pending.empty())
            {
                std::lock_guard hostLock(g_hostMutex);
                for (PendingPacket& item : pending)
                {
                    const enet_uint8 channel = item.channel == Channel::Reliable ? kControlChannel : kInputChannel;
                    const enet_uint32 flags = item.channel == Channel::Reliable ? ENET_PACKET_FLAG_RELIABLE : VirtualLanEnabled()?ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT:0;
                    ENetPacket* packet = enet_packet_create(item.bytes.data(), item.bytes.size(), flags);
                    if (!packet || enet_peer_send(event.peer, channel, packet) != 0)
                    {
                        if (packet) enet_packet_destroy(packet);
                        std::fprintf(stderr, "[net] queued packet could not be sent after peer connection\n");
                    }
                }
                enet_host_flush(g_host);
            }
        }
        else if (event.type == ENET_EVENT_TYPE_RECEIVE)
        {
            Receiver receiver = nullptr;
            uint64_t id = 0;
            {
                std::lock_guard lock(g_mutex);
                auto found = g_peerIds.find(event.peer);
                if (found == g_peerIds.end())
                {
                    id = IdForAddress(event.peer->address);
                    g_peerIds[event.peer] = id;
                    g_peers[id] = event.peer;
                }
                else id = found->second;
                receiver = g_receiver;
            }
            if (receiver && event.packet && event.packet->dataLength)
            {
                const Channel channel = event.channelID == kControlChannel ? Channel::Reliable : Channel::Unreliable;
                receiver(id, channel, event.packet->data, event.packet->dataLength);
            }
            if (event.packet) enet_packet_destroy(event.packet);
        }
        else if (event.type == ENET_EVENT_TYPE_DISCONNECT)
        {
            std::lock_guard lock(g_mutex);
            auto found = g_peerIds.find(event.peer);
            if (found != g_peerIds.end())
            {
                telemetry::SetState(telemetry::State::Searching);
                g_pendingPackets.erase(found->second);
                auto peer = g_peers.find(found->second);
                if (peer != g_peers.end() && peer->second == event.peer) g_peers.erase(peer);
                g_peerIds.erase(found);
            }
        }
    }
}

bool Resolve(uint64_t peer, ENetAddress& address)
{
    const auto it = g_addresses.find(peer);
    if (it != g_addresses.end()) { address = it->second; return true; }
    if ((peer & 0xFFFFFFFF00000000ull) == kLoopTag)
    {
        if (enet_address_set_host_ip(&address, "127.0.0.1") != 0) return false;
        address.port = uint16_t(peer & 0xFFFFu);
        return address.port != 0;
    }
    return false;
}
} // namespace

bool SetListenPort(uint16_t port)
{
    std::lock_guard lock(g_mutex);
    if (g_started.load()) return false;
    g_requestedPort = port;
    return true;
}

bool Start(Receiver receiver)
{
    std::lock_guard lock(g_mutex);
    if (g_started.load()) return true;
    const char* choice = std::getenv("TTT2_NET");
    const bool needSteam = choice && std::strcmp(choice, "steam") == 0;
    if (choice && std::strcmp(choice, "loopback") != 0 && !needSteam && std::strcmp(choice, "ipv4") != 0 && !VirtualLanEnabled())
    {
        std::fprintf(stderr, "[net] unknown TTT2_NET backend\n");
        return false;
    }
    g_receiver = receiver;
    if (needSteam)
    {
        if (!steamnet::Start(FromSteam)) return false;
        g_steam = true;
        g_local = steamnet::LocalId();
        g_started = true;
        return true;
    }
    if (enet_initialize() != 0) return false;
    ENetAddress bindAddress{};
    bindAddress.host = ENET_HOST_ANY;
    if(VirtualLanEnabled()) {
        if(!SelectAdapter(g_adapter,std::getenv("TTT2_LOCAL_IP"))) { std::fprintf(stderr,"[net] no active adapter matches TTT2_LOCAL_IP\n");enet_deinitialize();return false; }
        bindAddress.host=htonl(g_adapter.ip_host);
        telemetry::SetAdapter(g_adapter.name.c_str(),g_adapter.ip.c_str(),RedundancyEnabled());
        std::fprintf(stderr,"[net] adapter %s, priority %d, IPv4 %s, mask %08X, broadcast %s\n",g_adapter.name.c_str(),g_adapter.priority,g_adapter.ip.c_str(),g_adapter.mask_host,g_adapter.broadcast.c_str());
    } else telemetry::SetAdapter("IPv4 / ENet","auto",false);
    const char* portEnv = std::getenv("TTT2_NET_PORT");
    uint16_t port = g_requestedPort;
    if (portEnv && *portEnv)
    {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(portEnv, &end, 10);
        if (!end || *end || parsed > 65535) return false;
        port = uint16_t(parsed);
    }
    bindAddress.port = port;
    g_host = enet_host_create(&bindAddress, kMaxPeers, 2, 0, 0);
    if (!g_host) return false;
    port = g_host->address.port;
    if (!port) { enet_host_destroy(g_host); g_host = nullptr; return false; }
    const char* backend = choice && std::strcmp(choice, "ipv4") == 0 ? "IPv4/ENet" : "ENet loopback/LAN";
    // Preserve a stable local identity format consumed by XNADDR and the existing socket shim.
    g_local = VirtualLanEnabled() ? (kEndpointTag ^ (uint64_t(g_adapter.ip_host)<<16) ^ port) : kLoopTag | port;
    g_started = true;
    g_relaySocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_relaySocket != INVALID_SOCKET) { u_long nonblocking = 1; ioctlsocket(g_relaySocket, FIONBIO, &nonblocking); }
    if(VirtualLanEnabled()) {
        int bytes=262144;BOOL yes=TRUE;
        setsockopt(g_host->socket,SOL_SOCKET,SO_BROADCAST,reinterpret_cast<const char*>(&yes),sizeof(yes));
        setsockopt(g_host->socket,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&bytes),sizeof(bytes));
        setsockopt(g_host->socket,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<const char*>(&bytes),sizeof(bytes));
        lan::SetDatagramReceiver([](uint64_t peer,const uint8_t* data,size_t size){if(auto fn=g_receiver)fn(peer,Channel::Unreliable,data,size);});
        if(!lan::Start(g_adapter,g_local.load(),port)){g_started=false;enet_host_destroy(g_host);g_host=nullptr;if(g_relaySocket!=INVALID_SOCKET){closesocket(g_relaySocket);g_relaySocket=INVALID_SOCKET;}return false;}
        telemetry::SetState(telemetry::State::Searching);
    }
    std::thread(ReceiveLoop).detach();
    std::fprintf(stderr, "[net] %s listening on UDP port %u\n", backend, unsigned(port));
    return true;
}

uint64_t LocalPeer() { return g_local.load(); }
const char* BackendName() { return g_steam ? "steam" : "enet-ipv4"; }
bool IsBroadcastIPv4(uint32_t address){return address==0xFFFFFFFFu||(VirtualLanEnabled()&&g_adapter.ip_host&&address==(g_adapter.ip_host|~g_adapter.mask_host));}
uint32_t LocalIPv4(){return g_adapter.ip_host;}
uint16_t ListenPort(){return uint16_t(g_local.load()&0xFFFFu);}
bool BroadcastDatagram(const void* bytes,size_t size){return VirtualLanEnabled()&&lan::Broadcast(static_cast<const uint8_t*>(bytes),size);}
void ReportSimulationSync(uint64_t localFrame,uint64_t remoteFrame,float remoteAdvantage){
    if(!std::isfinite(remoteAdvantage))return;
    const double diff=double(localFrame)-double(remoteFrame);
    g_simRift=float(std::clamp((diff-double(remoteAdvantage))*0.5,-120.0,120.0));g_simValid=true;
    telemetry::SetSimulationSync(g_simRift.load(),0,true);
}
void NetTransport_ApplyFrameRiftPacing(){
    if(!RedundancyEnabled()||!g_simValid.load())return;
    const float rift=g_simRift.load();const int us=rift>0.5f?std::clamp(int(rift*1200),100,3000):0;
    telemetry::SetSimulationSync(rift,us,true);if(us)std::this_thread::sleep_for(std::chrono::microseconds(us));
}

bool RegisterPeer(uint64_t peer, const char* ipv4, uint16_t port)
{
    if (!peer || !ipv4 || !*ipv4 || !port) return false;
    ENetAddress address{};
    if (enet_address_set_host_ip(&address, ipv4) != 0) return false;
    address.port = port;
    std::lock_guard lock(g_mutex);
    if (g_addresses.size() >= kMaxPeers && g_addresses.find(peer) == g_addresses.end()) return false;
    g_addresses[peer] = address;
    return true;
}

bool RegisterRelayPeer(uint64_t peer, const char* ipv4, uint16_t port, const uint8_t token[16], uint8_t role)
{
    if (!peer || !ipv4 || !*ipv4 || !port || !token || (role != 1 && role != 2) || g_steam) return false;
    if (g_relaySocket == INVALID_SOCKET) return false;
    sockaddr_in endpoint{}; endpoint.sin_family = AF_INET; endpoint.sin_port = htons(port);
    if (InetPtonA(AF_INET, ipv4, &endpoint.sin_addr) != 1) return false;
    RelayRoute route{}; route.endpoint = endpoint; route.role = role; std::memcpy(route.token, token, 16);
    std::lock_guard lock(g_mutex); g_relayRoutes[peer] = route; return true;
}

bool Send(uint64_t peer, Channel channel, const void* data, size_t size)
{
    if (g_steam) return steamnet::Send(peer, channel == Channel::Reliable, data, size);
    if (!g_started.load() || !data || size == 0 || size > 1024 * 1024) return false;
    std::lock_guard lock(g_mutex);
    auto relay = g_relayRoutes.find(peer);
    if (relay != g_relayRoutes.end())
    {
        RelayRoute& route = relay->second;
        if (!route.hasSent) { route.firstSend = std::chrono::steady_clock::now(); route.hasSent = true; }
        const auto elapsed = std::chrono::steady_clock::now() - route.firstSend;
        auto direct = g_peers.find(peer);
        const bool directReady = !route.fallback && direct != g_peers.end() && direct->second && direct->second->state == ENET_PEER_STATE_CONNECTED;
        if (!directReady && elapsed >= std::chrono::milliseconds(500)) route.fallback = true;
        if (route.fallback)
        {
            if (size > 65507 - 18) return false;
            std::vector<uint8_t> packet(18 + size);
            std::memcpy(packet.data(), route.token, 16); packet[16] = route.role;
            packet[17] = channel == Channel::Reliable ? kControlChannel : kInputChannel;
            std::memcpy(packet.data() + 18, data, size);
            return sendto(g_relaySocket, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
                          reinterpret_cast<const sockaddr*>(&route.endpoint), sizeof(route.endpoint)) == static_cast<int>(packet.size());
        }
        // Teach the relay this client's observed NAT endpoint during the direct-connect grace.
        if (!route.fallback && !directReady)
        {
            uint8_t probe[19]{}; std::memcpy(probe, route.token, 16); probe[16] = route.role; probe[17] = kInputChannel; probe[18] = 0xFE;
            (void)sendto(g_relaySocket, reinterpret_cast<const char*>(probe), sizeof(probe), 0,
                         reinterpret_cast<const sockaddr*>(&route.endpoint), sizeof(route.endpoint));
        }
    }
    ENetPeer* connection = nullptr;
    auto found = g_peers.find(peer);
    if (found != g_peers.end()) connection = found->second;
    if (!connection)
    {
        ENetAddress address{};
        if (!Resolve(peer, address))
        {
            std::fprintf(stderr, "[net] peer %016llX has no registered endpoint\n", (unsigned long long)peer);
            return false;
        }
        {
            std::lock_guard hostLock(g_hostMutex);
            connection = enet_host_connect(g_host, &address, 2, 0);
        }
        if (!connection)
        {
            std::fprintf(stderr, "[net] ENet could not allocate peer for %u.%u.%u.%u:%u\n",
                         address.host & 0xFFu, (address.host >> 8) & 0xFFu,
                         (address.host >> 16) & 0xFFu, (address.host >> 24) & 0xFFu,
                         unsigned(address.port));
            return false;
        }
        g_peers[peer] = connection;
        g_peerIds[connection] = peer;
    }
    const enet_uint8 channelId = channel == Channel::Reliable ? kControlChannel : kInputChannel;
    const enet_uint32 flags = channel == Channel::Reliable ? ENET_PACKET_FLAG_RELIABLE : VirtualLanEnabled()?ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT:0;
    ENetPacket* packet = enet_packet_create(data, size, flags);
    if (!packet) return false;
    std::lock_guard hostLock(g_hostMutex);
    if (connection->state == ENET_PEER_STATE_CONNECTING)
    {
        enet_packet_destroy(packet);
        auto& pending = g_pendingPackets[peer];
        if (pending.size() >= 64) return false;
        PendingPacket queued;
        queued.channel = channel;
        queued.bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
        pending.push_back(std::move(queued));
        return true;
    }
    if (connection->state != ENET_PEER_STATE_CONNECTED)
    {
        enet_packet_destroy(packet);
        return false;
    }
    if (enet_peer_send(connection, channelId, packet) != 0)
    {
        std::fprintf(stderr, "[net] ENet send rejected packet for peer %016llX (state %u, channel %u)\n",
                     (unsigned long long)peer, unsigned(connection->state), unsigned(channelId));
        enet_packet_destroy(packet);
        return false;
    }
    enet_host_flush(g_host);
    return true;
}

} // namespace net
