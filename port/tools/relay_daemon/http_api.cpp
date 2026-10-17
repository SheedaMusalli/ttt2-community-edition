#include "http_api.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <string_view>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace relay {
namespace {
std::string JsonString(const std::string& body, const char* key)
{
    const std::string needle = std::string("\"") + key + "\"";
    size_t p = body.find(needle); if (p == std::string::npos) return {};
    p = body.find(':', p + needle.size()); if (p == std::string::npos) return {};
    p = body.find('"', p + 1); if (p == std::string::npos) return {};
    const size_t end = body.find('"', p + 1); if (end == std::string::npos) return {};
    return body.substr(p + 1, end - p - 1);
}
uint32_t JsonUint(const std::string& body, const char* key)
{
    const std::string needle = std::string("\"") + key + "\"";
    size_t p = body.find(needle); if (p == std::string::npos) return 0;
    p = body.find(':', p + needle.size()); if (p == std::string::npos) return 0;
    ++p; while (p < body.size() && std::isspace(static_cast<unsigned char>(body[p]))) ++p;
    uint32_t value = 0; bool any = false;
    while (p < body.size() && body[p] >= '0' && body[p] <= '9') { any = true; value = value * 10 + uint32_t(body[p++] - '0'); }
    return any ? value : 0;
}
bool ConstantEqual(const std::string& a, const std::string& b)
{
    if (a.size() != b.size() || a.empty()) return false;
    unsigned diff = 0; for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}
void Reply(RelaySocket client, int code, const char* reason, const std::string& body)
{
    std::ostringstream out; out << "HTTP/1.1 " << code << ' ' << reason << "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " << body.size() << "\r\n\r\n" << body;
    const std::string response = out.str();
#ifdef _WIN32
    (void)send(client, response.data(), static_cast<int>(response.size()), 0);
#else
    (void)send(client, response.data(), response.size(), MSG_NOSIGNAL);
#endif
}
std::string Header(const std::string& request, const std::string& name)
{
    const std::string lowerName = [&] { std::string v = name; std::transform(v.begin(),v.end(),v.begin(),[](unsigned char c){return char(std::tolower(c));}); return v; }();
    size_t p = request.find("\r\n") + 2;
    while (p < request.size()) {
        const size_t e = request.find("\r\n", p); if (e == std::string::npos || e == p) break;
        std::string line = request.substr(p, e - p); const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon); std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return char(std::tolower(c));});
            if (key == lowerName) { size_t s = line.find_first_not_of(" \t", colon + 1); return s == std::string::npos ? "" : line.substr(s); }
        }
        p = e + 2;
    }
    return {};
}
}

void RunHttpApi(const std::string& bindAddress, uint16_t port, const std::string& adminToken,
                SessionManager& sessions, const std::atomic<bool>& running)
{
    if (adminToken.empty()) return;
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_PASSIVE;
    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(bindAddress.empty() ? nullptr : bindAddress.c_str(), service.c_str(), &hints, &result) != 0) return;
    RelaySocket listener = kInvalidRelaySocket;
    for (auto* item = result; item; item = item->ai_next) {
        listener = static_cast<RelaySocket>(socket(item->ai_family, item->ai_socktype, item->ai_protocol));
        if (listener == kInvalidRelaySocket) continue;
        int yes = 1; (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
        if (bind(listener, item->ai_addr, static_cast<int>(item->ai_addrlen)) == 0 && listen(listener, 16) == 0) break;
        CloseRelaySocket(listener); listener = kInvalidRelaySocket;
    }
    freeaddrinfo(result); if (listener == kInvalidRelaySocket) return;
    SetRelayNonBlocking(listener);
    while (running.load()) {
#ifdef _WIN32
        WSAPOLLFD pfd{listener, POLLRDNORM, 0}; if (WSAPoll(&pfd, 1, 200) <= 0) continue;
#else
        pollfd pfd{listener, POLLIN, 0}; if (poll(&pfd, 1, 200) <= 0) continue;
#endif
        sockaddr_storage address{};
#ifdef _WIN32
        int addressSize = sizeof(address);
#else
        socklen_t addressSize = sizeof(address);
#endif
        RelaySocket client = accept(listener, reinterpret_cast<sockaddr*>(&address), &addressSize);
        if (client == kInvalidRelaySocket) continue;
        SetRelayNonBlocking(client);
        std::string request; request.reserve(2048); char chunk[2048]; size_t wanted = 0;
        for (int attempt = 0; attempt < 10 && request.size() < 8192; ++attempt) {
#ifdef _WIN32
            const int n = recv(client, chunk, sizeof(chunk), 0); if (n == SOCKET_ERROR) { if (RelayWouldBlock()) { Sleep(10); continue; } break; }
#else
            const ssize_t n = recv(client, chunk, sizeof(chunk), 0); if (n < 0) { if (RelayWouldBlock()) { usleep(10000); continue; } break; }
#endif
            if (n <= 0) break; request.append(chunk, static_cast<size_t>(n));
            const size_t headerEnd = request.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                try { wanted = static_cast<size_t>(std::stoul(Header(request, "Content-Length"))); } catch (...) { wanted = 0; }
                if (request.size() >= headerEnd + 4 + wanted) break;
            }
        }
        const size_t split = request.find("\r\n\r\n");
        if (split == std::string::npos || wanted > 4096 || request.size() < split + 4 + wanted) { Reply(client, 400, "Bad Request", "{\"error\":\"invalid request\"}"); }
        else {
            const size_t firstEnd = request.find("\r\n"); const std::string first = request.substr(0, firstEnd);
            const std::string auth = Header(request, "Authorization");
            if (!ConstantEqual(auth, "Bearer " + adminToken)) Reply(client, 401, "Unauthorized", "{\"error\":\"unauthorized\"}");
            else if (first.rfind("POST /v1/relay/sessions ", 0) == 0) {
                const std::string body = request.substr(split + 4, wanted);
                const bool ok = sessions.Register(JsonString(body,"match_id"), JsonString(body,"relay_token"), JsonString(body,"p1_id"), JsonString(body,"p2_id"), JsonUint(body,"ttl"));
                Reply(client, ok ? 200 : 400, ok ? "OK" : "Bad Request", ok ? "{\"registered\":true}" : "{\"error\":\"invalid session\"}");
            } else if (first.rfind("DELETE /v1/relay/sessions/", 0) == 0) {
                const size_t start = std::strlen("DELETE /v1/relay/sessions/");
                const size_t end = first.find(' ', start); const std::string id = first.substr(start, end - start);
                const bool ok = sessions.Remove(id); Reply(client, ok ? 200 : 404, ok ? "OK" : "Not Found", ok ? "{\"removed\":true}" : "{\"error\":\"not found\"}");
            } else Reply(client, 404, "Not Found", "{\"error\":\"not found\"}");
        }
        CloseRelaySocket(client);
    }
    CloseRelaySocket(listener);
}
}
