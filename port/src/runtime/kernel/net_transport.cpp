// ENet-backed peer transport. Channel 0 is sequenced-unreliable and channel 1 is reliable,
// ordered. Peer IDs remain owned by the XLive/session layer; this module maps them to IPv4.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "enet/enet.h"
#include "net_transport.h"
#include "steam_net.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>

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
uint16_t g_requestedPort = 0;
std::map<uint64_t, ENetAddress> g_addresses;
std::map<uint64_t, ENetPeer*> g_peers;
std::map<ENetPeer*, uint64_t> g_peerIds;

void FromSteam(uint64_t peer, bool reliable, const uint8_t* data, size_t size)
{
    Receiver receiver = g_receiver;
    if (receiver) receiver(peer, reliable ? Channel::Reliable : Channel::Unreliable, data, size);
}

uint64_t IdForAddress(const ENetAddress& address)
{
    for (const auto& [id, endpoint] : g_addresses)
        if (endpoint.host == address.host && endpoint.port == address.port) return id;
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
        int result = 0;
        {
            std::lock_guard hostLock(g_hostMutex);
            result = enet_host_service(g_host, &event, 10);
        }
        if (result <= 0) continue;
        if (event.type == ENET_EVENT_TYPE_CONNECT)
        {
            std::lock_guard lock(g_mutex);
            uint64_t id = IdForAddress(event.peer->address);
            g_peers[id] = event.peer;
            g_peerIds[event.peer] = id;
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
    if (choice && std::strcmp(choice, "loopback") != 0 && !needSteam && std::strcmp(choice, "ipv4") != 0)
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
    g_local = kLoopTag | port;
    g_started = true;
    std::thread(ReceiveLoop).detach();
    std::fprintf(stderr, "[net] %s listening on UDP port %u\n", backend, unsigned(port));
    return true;
}

uint64_t LocalPeer() { return g_local.load(); }
const char* BackendName() { return g_steam ? "steam" : "enet-ipv4"; }

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

bool Send(uint64_t peer, Channel channel, const void* data, size_t size)
{
    if (g_steam) return steamnet::Send(peer, channel == Channel::Reliable, data, size);
    if (!g_started.load() || !data || size == 0 || size > 1024 * 1024) return false;
    std::lock_guard lock(g_mutex);
    ENetPeer* connection = nullptr;
    auto found = g_peers.find(peer);
    if (found != g_peers.end()) connection = found->second;
    if (!connection)
    {
        ENetAddress address{};
        if (!Resolve(peer, address)) return false;
        {
            std::lock_guard hostLock(g_hostMutex);
            connection = enet_host_connect(g_host, &address, 2, 0);
        }
        if (!connection) return false;
        g_peers[peer] = connection;
        g_peerIds[connection] = peer;
    }
    const enet_uint8 channelId = channel == Channel::Reliable ? kControlChannel : kInputChannel;
    const enet_uint32 flags = channel == Channel::Reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
    ENetPacket* packet = enet_packet_create(data, size, flags);
    if (!packet) return false;
    std::lock_guard hostLock(g_hostMutex);
    if (enet_peer_send(connection, channelId, packet) != 0)
    {
        enet_packet_destroy(packet);
        return false;
    }
    enet_host_flush(g_host);
    return true;
}

} // namespace net
