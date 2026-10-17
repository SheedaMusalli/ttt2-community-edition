#define _GNU_SOURCE
#include "relay_engine.h"
#include <array>
#include <cerrno>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/types.h>
#endif
#endif

namespace relay {
namespace {
constexpr size_t kBatch = 32;
constexpr size_t kPacketMax = 4096;

bool ValidPacket(const uint8_t* data, size_t size)
{
    return size >= 18 && (data[16] == 1 || data[16] == 2);
}
}

bool ConfigureUdpSocket(RelaySocket socket)
{
    int bytes = 4 * 1024 * 1024;
    if (setsockopt(socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes)) != 0) return false;
    if (setsockopt(socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes)) != 0) return false;
#ifndef _WIN32
    int enabled = 1;
    (void)setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
#ifdef __linux__
    (void)setsockopt(socket, SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled));
#endif
#endif
    return true;
}

void RunRelayWorker(RelaySocket socket, SessionManager& sessions, const std::atomic<bool>& running)
{
#ifdef __linux__
    struct Slot { std::array<uint8_t, kPacketMax> data{}; sockaddr_in source{}; sockaddr_in target{}; };
    std::array<Slot, kBatch> slots{};
    std::array<mmsghdr, kBatch> incoming{};
    std::array<iovec, kBatch> inIov{};
    std::array<mmsghdr, kBatch> outgoing{};
    std::array<iovec, kBatch> outIov{};
    while (running.load()) {
        for (size_t i = 0; i < kBatch; ++i) {
            incoming[i] = {};
            inIov[i] = { slots[i].data.data(), slots[i].data.size() };
            incoming[i].msg_hdr.msg_iov = &inIov[i]; incoming[i].msg_hdr.msg_iovlen = 1;
            incoming[i].msg_hdr.msg_name = &slots[i].source; incoming[i].msg_hdr.msg_namelen = sizeof(sockaddr_in);
        }
        timespec timeout{0, 100000000};
        const int count = recvmmsg(socket, incoming.data(), static_cast<unsigned>(kBatch), 0, &timeout);
        if (count <= 0) { if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue; continue; }
        unsigned sendCount = 0;
        for (int i = 0; i < count; ++i) {
            const size_t size = incoming[i].msg_len;
            auto& slot = slots[static_cast<size_t>(i)];
            if (!ValidPacket(slot.data.data(), size)) continue;
            Endpoint target{};
            if (!sessions.Route(slot.data.data(), slot.data[16], slot.source, target) || !target.known) continue;
            outIov[sendCount] = { slot.data.data(), size };
            outgoing[sendCount] = {};
            outgoing[sendCount].msg_hdr.msg_iov = &outIov[sendCount]; outgoing[sendCount].msg_hdr.msg_iovlen = 1;
            outgoing[sendCount].msg_hdr.msg_name = &target.address; outgoing[sendCount].msg_hdr.msg_namelen = sizeof(sockaddr_in);
            ++sendCount;
        }
        if (sendCount) (void)sendmmsg(socket, outgoing.data(), sendCount, MSG_DONTWAIT);
    }
#else
    std::array<uint8_t, kPacketMax> buffer{};
    while (running.load()) {
#ifdef _WIN32
        WSAPOLLFD descriptor{ socket, POLLRDNORM, 0 };
        if (WSAPoll(&descriptor, 1, 100) <= 0) continue;
#else
        pollfd descriptor{ socket, POLLIN, 0 };
        if (poll(&descriptor, 1, 100) <= 0) continue;
#endif
        for (size_t i = 0; i < kBatch; ++i) {
            sockaddr_in source{};
#ifdef _WIN32
            int sourceSize = sizeof(source);
            const int received = recvfrom(socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0,
                reinterpret_cast<sockaddr*>(&source), &sourceSize);
            if (received == SOCKET_ERROR) break;
#else
            socklen_t sourceSize = sizeof(source);
            const ssize_t received = recvfrom(socket, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr*>(&source), &sourceSize);
            if (received < 0) break;
#endif
            const size_t size = static_cast<size_t>(received);
            if (!ValidPacket(buffer.data(), size)) continue;
            Endpoint target{};
            if (!sessions.Route(buffer.data(), buffer[16], source, target) || !target.known) continue;
#ifdef _WIN32
            (void)sendto(socket, reinterpret_cast<const char*>(buffer.data()), static_cast<int>(size), 0,
                reinterpret_cast<const sockaddr*>(&target.address), sizeof(target.address));
#else
            (void)sendto(socket, buffer.data(), size, 0, reinterpret_cast<const sockaddr*>(&target.address), sizeof(target.address));
#endif
        }
    }
#endif
}

} // namespace relay
