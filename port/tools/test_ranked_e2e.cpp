#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include "net_transport.h"
#include "steam_net.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

namespace {
constexpr auto kTimeout = std::chrono::seconds(35);
std::string g_connectToken, g_sessionKey, g_expectedPlayer, g_localPlayer;
std::atomic<bool> g_authenticated{ false };
std::atomic<unsigned> g_replies{ 0 };

std::wstring Widen(const std::string& value)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, out.data(), n);
    out.resize(size_t(n - 1));
    return out;
}

std::string Extract(const std::string& json, const char* key)
{
    const std::string needle = std::string("\"") + key + "\"";
    size_t at = json.find(needle);
    if (at == std::string::npos || (at = json.find(':', at + needle.size())) == std::string::npos) return {};
    at = json.find('"', at + 1);
    if (at == std::string::npos) return {};
    const size_t end = json.find('"', at + 1);
    return end == std::string::npos ? std::string{} : json.substr(at + 1, end - at - 1);
}

bool HttpPost(const std::string& base, const std::string& token, const char* route,
              const std::string& body, std::string& response)
{
    response.clear();
    const std::wstring url = Widen(base);
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[512] = {};
    parts.lpszHostName = host; parts.dwHostNameLength = _countof(host);
    parts.lpszUrlPath = path; parts.dwUrlPathLength = _countof(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) return false;
    std::wstring requestPath(path, parts.dwUrlPathLength);
    requestPath += Widen(route);
    HINTERNET session = WinHttpOpen(L"TTT2 ranked E2E test", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    HINTERNET connection = WinHttpConnect(session, std::wstring(host, parts.dwHostNameLength).c_str(), parts.nPort, 0);
    if (!connection) { WinHttpCloseHandle(session); return false; }
    HINTERNET request = WinHttpOpenRequest(connection, L"POST", requestPath.c_str(), nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request) { WinHttpCloseHandle(connection); WinHttpCloseHandle(session); return false; }
    const std::wstring auth = L"Content-Type: application/json\r\nAuthorization: Bearer " + Widen(token) + L"\r\n";
    BOOL ok = WinHttpSendRequest(request, auth.c_str(), DWORD(auth.size()), const_cast<char*>(body.data()),
                                 DWORD(body.size()), DWORD(body.size()), 0) && WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0, size = sizeof(status);
    ok = ok && WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                   WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) && status == 200;
    if (ok)
    {
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available)
        {
            if (response.size() + available > 16384) { ok = FALSE; break; }
            const size_t offset = response.size(); response.resize(offset + available);
            DWORD read = 0;
            if (!WinHttpReadData(request, response.data() + offset, available, &read)) { ok = FALSE; break; }
            response.resize(offset + read);
            if (!read) break;
        }
    }
    WinHttpCloseHandle(request); WinHttpCloseHandle(connection); WinHttpCloseHandle(session);
    return ok != FALSE;
}

uint64_t PeerId(const std::string& endpoint)
{
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : endpoint) { hash ^= c; hash *= 1099511628211ull; }
    return 0x52414E4B00000000ull | (hash & 0xFFFFFFFFull);
}

void Receiver(uint64_t peer, net::Channel channel, const uint8_t* data, size_t size)
{
    if (channel == net::Channel::Reliable)
    {
        const std::string message(reinterpret_cast<const char*>(data), size);
        const std::string prefix = "H|" + g_connectToken + "|" + g_expectedPlayer + "|" + g_sessionKey;
        if (message == prefix)
        {
            g_authenticated = true;
            const std::string ack = "A|" + g_localPlayer;
            static_cast<void>(net::Send(peer, net::Channel::Reliable, ack.data(), ack.size()));
        }
        else if (message == "A|" + g_expectedPlayer) g_authenticated = true;
        return;
    }
    if (!g_authenticated.load()) return;
    const std::string message(reinterpret_cast<const char*>(data), size);
    if (message.rfind("P|", 0) == 0)
    {
        const std::string reply = "R|" + message.substr(2);
        static_cast<void>(net::Send(peer, net::Channel::Unreliable, reply.data(), reply.size()));
    }
    else if (message.rfind("R|", 0) == 0) ++g_replies;
}

bool WaitUntil(const std::function<bool()>& predicate, std::chrono::steady_clock::time_point deadline)
{
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

int Client(const char* base, const char* token, const char* player, uint16_t port)
{
    g_localPlayer = player;
    if (!net::SetListenPort(port) || !net::Start(Receiver)) return 10;
    char endpoint[40] = {};
    std::snprintf(endpoint, sizeof(endpoint), "127.0.0.1:%u", unsigned(port));
    const std::string queue = std::string("{\"build\":\"ttt2-e2e\",\"region\":\"local\",\"listen_endpoint\":\"") + endpoint + "\"}";
    std::string response;
    if (!HttpPost(base, token, "/v1/queue", queue, response)) return 11;
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    while (Extract(response, "state") != "reserved" && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!HttpPost(base, token, "/v1/status", "{}", response)) return 12;
    }
    if (Extract(response, "state") != "reserved") return 13;
    g_connectToken = Extract(response, "match_token");
    g_sessionKey = Extract(response, "session_key_bytes");
    const std::string a = Extract(response, "player_a"), b = Extract(response, "player_b");
    g_expectedPlayer = a == player ? b : a;
    const std::string endpointText = Extract(response, "opponent_endpoint");
    const size_t colon = endpointText.rfind(':');
    if (colon == std::string::npos || g_connectToken.empty() || g_sessionKey.size() != 32 || g_expectedPlayer.empty()) return 14;
    const std::string ip = endpointText.substr(0, colon);
    char* end = nullptr;
    const unsigned long remotePort = std::strtoul(endpointText.c_str() + colon + 1, &end, 10);
    if (!end || *end || !remotePort || remotePort > 65535) return 15;
    const uint64_t peer = PeerId(endpointText);
    if (!net::RegisterPeer(peer, ip.c_str(), uint16_t(remotePort))) return 16;
    const std::string handshake = "H|" + g_connectToken + "|" + g_localPlayer + "|" + g_sessionKey;
    while (!g_authenticated.load() && std::chrono::steady_clock::now() < deadline)
    {
        static_cast<void>(net::Send(peer, net::Channel::Reliable, handshake.data(), handshake.size()));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!g_authenticated.load()) return 17;
    for (unsigned i = 0; i < 20; ++i)
    {
        const std::string ping = std::string("P|") + player + "|" + std::to_string(i);
        if (!net::Send(peer, net::Channel::Unreliable, ping.data(), ping.size())) return 18;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!WaitUntil([] { return g_replies.load() >= 20; }, deadline)) return 19;
    std::printf("%s: reservation, ENet reliable handshake and 20 bidirectional UDP-channel replies passed\n", player);
    return 0;
}

bool StartChild(const char* executable, const char* base, const char* token, const char* player, int port,
                PROCESS_INFORMATION& process)
{
    char command[2048] = {};
    const int count = std::snprintf(command, sizeof(command), "\"%s\" --client \"%s\" \"%s\" %s %d",
                                    executable, base, token, player, port);
    if (count < 0 || size_t(count) >= sizeof(command)) return false;
    STARTUPINFOA startup{}; startup.cb = sizeof(startup);
    return CreateProcessA(executable, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                          &startup, &process) != FALSE;
}
} // namespace

// Standalone ENet test target does not enable Steam.
namespace steamnet {
bool Start(Receiver) { return false; }
uint64_t LocalId() { return 0; }
bool Send(uint64_t, bool, const void*, size_t) { return false; }
} // namespace steamnet

int main(int argc, char** argv)
{
    if (argc == 6 && std::strcmp(argv[1], "--client") == 0)
    {
        char* end = nullptr;
        const unsigned long port = std::strtoul(argv[5], &end, 10);
        if (!end || *end || !port || port > 65535) return 2;
        return Client(argv[2], argv[3], argv[4], uint16_t(port));
    }
    if (argc != 4) return 1;
    char executable[MAX_PATH] = {};
    if (!GetModuleFileNameA(nullptr, executable, MAX_PATH)) return 1;
    PROCESS_INFORMATION alice{}, bob{};
    if (!StartChild(executable, argv[1], argv[2], "alice", 9001, alice)) return 3;
    CloseHandle(alice.hThread);
    if (!StartChild(executable, argv[1], argv[3], "bob", 9002, bob))
    { TerminateProcess(alice.hProcess, 3); CloseHandle(alice.hProcess); return 4; }
    CloseHandle(bob.hThread);
    HANDLE processes[] = { alice.hProcess, bob.hProcess };
    const DWORD wait = WaitForMultipleObjects(2, processes, TRUE, DWORD(kTimeout.count() * 1000));
    DWORD aliceExit = 1, bobExit = 1;
    GetExitCodeProcess(alice.hProcess, &aliceExit); GetExitCodeProcess(bob.hProcess, &bobExit);
    if (wait != WAIT_OBJECT_0)
    {
        TerminateProcess(alice.hProcess, 20); TerminateProcess(bob.hProcess, 20);
    }
    CloseHandle(alice.hProcess); CloseHandle(bob.hProcess);
    if (wait != WAIT_OBJECT_0 || aliceExit || bobExit)
    {
        std::fprintf(stderr, "ranked E2E failed (wait=%lu, alice=%lu, bob=%lu)\n", wait, aliceExit, bobExit);
        return 5;
    }
    std::puts("ranked HTTP reservation and ENet IPv4 peer flow passed");
    return 0;
}
