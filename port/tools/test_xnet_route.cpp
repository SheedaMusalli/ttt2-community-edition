#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "net_transport.h"
#include "steam_net.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

namespace {

constexpr char kHandshake[] = "AUTH:loopback-test-reservation-token";
constexpr char kAccepted[] = "AUTHENTICATED";
constexpr char kSocketPayload[] = "SOCKET-UDP-PING-42";
constexpr char kSocketReply[] = "SOCKET-UDP-PONG-42";
constexpr auto kTimeout = std::chrono::seconds(10);

std::atomic<bool> g_authenticated{ false };
std::atomic<bool> g_clientAccepted{ false };
std::atomic<bool> g_serverDone{ false };
std::atomic<bool> g_clientReceivedReply{ false };

void Receiver(uint64_t from, net::Channel channel, const uint8_t* data, size_t size)
{
    if (channel == net::Channel::Reliable && size == sizeof(kHandshake) - 1 &&
        std::memcmp(data, kHandshake, size) == 0)
    {
        g_authenticated = true;
        static_cast<void>(net::Send(from, net::Channel::Reliable, kAccepted, sizeof(kAccepted) - 1));
        return;
    }
    if (channel == net::Channel::Reliable && size == sizeof(kAccepted) - 1 &&
        std::memcmp(data, kAccepted, size) == 0)
    {
        g_clientAccepted = true;
        return;
    }
    if (channel == net::Channel::Unreliable && g_authenticated && size == sizeof(kSocketPayload) - 1 &&
        std::memcmp(data, kSocketPayload, size) == 0)
    {
        static_cast<void>(net::Send(from, net::Channel::Unreliable, kSocketReply, sizeof(kSocketReply) - 1));
        g_serverDone = true;
        return;
    }
    if (channel == net::Channel::Unreliable && g_clientAccepted && size == sizeof(kSocketReply) - 1 &&
        std::memcmp(data, kSocketReply, size) == 0)
    {
        g_clientReceivedReply = true;
    }
}

bool WaitFor(const std::atomic<bool>& flag)
{
    const auto until = std::chrono::steady_clock::now() + kTimeout;
    while (std::chrono::steady_clock::now() < until)
    {
        if (flag.load()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return flag.load();
}

int Server(const char* peerFile)
{
    if (!net::Start(Receiver)) return 2;
    const uint64_t peer = net::LocalPeer();
    {
        std::ofstream out(peerFile, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(&peer), sizeof(peer));
        if (!out) return 3;
    }
    if (!WaitFor(g_serverDone)) return 4;
    return 0;
}

bool ReadPeer(const char* peerFile, uint64_t& peer)
{
    const auto until = std::chrono::steady_clock::now() + kTimeout;
    while (std::chrono::steady_clock::now() < until)
    {
        std::ifstream in(peerFile, std::ios::binary);
        if (in.read(reinterpret_cast<char*>(&peer), sizeof(peer)) && peer != 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

// Test-only unavailable Steam backend, so this target exercises the real localhost ENet backend.
namespace steamnet {
bool Start(Receiver) { return false; }
uint64_t LocalId() { return 0; }
bool Send(uint64_t, bool, const void*, size_t) { return false; }
} // namespace steamnet

int main(int argc, char** argv)
{
    if (argc == 3 && std::strcmp(argv[1], "--server") == 0) return Server(argv[2]);
    if (argc != 1) return 1;

    char peerFile[MAX_PATH] = {};
    char tempPath[MAX_PATH] = {};
    if (!GetTempPathA(MAX_PATH, tempPath) ||
        std::snprintf(peerFile, sizeof(peerFile), "%sttt2-xnet-route-%lu.bin", tempPath,
                      static_cast<unsigned long>(GetCurrentProcessId())) < 0)
        return 1;
    DeleteFileA(peerFile);

    char executable[MAX_PATH] = {};
    const DWORD executableSize = GetModuleFileNameA(nullptr, executable, MAX_PATH);
    if (!executableSize || executableSize >= MAX_PATH) return 1;
    char commandLine[2 * MAX_PATH + 32] = {};
    const int commandLength = std::snprintf(commandLine, sizeof(commandLine), "\"%s\" --server \"%s\"",
                                            executable, peerFile);
    if (commandLength < 0 || static_cast<size_t>(commandLength) >= sizeof(commandLine)) return 1;
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessA(executable, commandLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &child)) return 1;
    CloseHandle(child.hThread);

    uint64_t peer = 0;
    int result = 0;
    if (!ReadPeer(peerFile, peer) || !net::Start(Receiver)) result = 2;
    if (!result && !net::Send(peer, net::Channel::Reliable, kHandshake, sizeof(kHandshake) - 1)) result = 3;
    if (!result && !WaitFor(g_clientAccepted)) result = 4;
    if (!result && !net::Send(peer, net::Channel::Unreliable, kSocketPayload, sizeof(kSocketPayload) - 1)) result = 5;
    if (!result && !WaitFor(g_clientReceivedReply)) result = 6;
    int childExit = -1;
    if (WaitForSingleObject(child.hProcess, DWORD(kTimeout.count() * 1000)) != WAIT_OBJECT_0)
        result = 7;
    else
    {
        DWORD exitCode = 1;
        if (GetExitCodeProcess(child.hProcess, &exitCode)) childExit = int(exitCode);
        if (childExit != 0) result = 7;
    }
    CloseHandle(child.hProcess);
    DeleteFileA(peerFile);
    if (result)
    {
        std::fprintf(stderr, "XNet peer route test failed at stage %d (server exit %d)\n", result, childExit);
        return result;
    }
    std::puts("ENet loopback reliable handshake and unreliable datagram round-trip passed");
    return 0;
}
