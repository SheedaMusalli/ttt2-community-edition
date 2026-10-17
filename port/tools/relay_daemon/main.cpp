#include "http_api.h"
#include "relay_engine.h"
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace { std::atomic<bool> g_running{true}; void Stop(int) { g_running = false; } }

int main(int argc, char** argv)
{
    uint16_t udpPort = 9100, httpPort = 9101; unsigned workers = 1; std::string httpBind = "127.0.0.1";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto number = [&](uint32_t& out) { if (i + 1 >= argc) return false; try { out = static_cast<uint32_t>(std::stoul(argv[++i])); return out <= 65535; } catch (...) { return false; } };
        uint32_t value = 0;
        if (arg == "--udp-port") { if (!number(value)) return 2; udpPort = static_cast<uint16_t>(value); }
        else if (arg == "--http-port") { if (!number(value)) return 2; httpPort = static_cast<uint16_t>(value); }
        else if (arg == "--workers") { if (!number(value) || value == 0 || value > 64) return 2; workers = value; }
        else if (arg == "--http-bind" && i + 1 < argc) httpBind = argv[++i];
        else if (arg == "--help") { std::cout << "ttt2_relay_daemon [--udp-port 9100] [--http-port 9101] [--workers 1] [--http-bind 127.0.0.1]\n"; return 0; }
        else { std::cerr << "Unknown/invalid argument: " << arg << '\n'; return 2; }
    }
#ifdef _WIN32
    WSADATA wsa{}; if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) return 1;
#endif
    RelaySocket udp = static_cast<RelaySocket>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (udp == kInvalidRelaySocket) { std::cerr << "UDP socket creation failed\n"; return 1; }
    if (!relay::ConfigureUdpSocket(udp)) std::cerr << "Warning: could not set requested UDP buffer sizes\n";
    sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_ANY); local.sin_port = htons(udpPort);
    if (bind(udp, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) { std::cerr << "UDP bind failed\n"; CloseRelaySocket(udp); return 1; }
    std::signal(SIGINT, Stop); std::signal(SIGTERM, Stop);
    relay::SessionManager sessions;
    const char* token = std::getenv("TTT2_RELAY_ADMIN_TOKEN");
    if (!token || !*token) { std::cerr << "Set TTT2_RELAY_ADMIN_TOKEN before starting the relay.\n"; CloseRelaySocket(udp); return 2; }
    std::cout << "Relay listening UDP " << udpPort << ", HTTP " << httpBind << ':' << httpPort << ", workers " << workers << '\n';
    std::thread api([&] { relay::RunHttpApi(httpBind, httpPort, token, sessions, g_running); });
    std::thread sweeper([&] { while (g_running.load()) { for (int i=0; i<600 && g_running.load(); ++i) {
#ifdef _WIN32
        Sleep(100);
#else
        usleep(100000);
#endif
    } sessions.PruneExpired(); } });
    std::vector<std::thread> pool; pool.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) pool.emplace_back([&] { relay::RunRelayWorker(udp, sessions, g_running); });
    for (auto& worker : pool) worker.join();
    g_running = false; if (api.joinable()) api.join(); if (sweeper.joinable()) sweeper.join();
    CloseRelaySocket(udp);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
