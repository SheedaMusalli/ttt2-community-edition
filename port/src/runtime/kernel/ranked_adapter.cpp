#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include "net_transport.h"
#include "ranked_adapter.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#pragma comment(lib, "winhttp.lib")

namespace ranked {
namespace {

const char* Env(const char* name, const char* fallback = "")
{
    const char* value = std::getenv(name);
    return value ? value : fallback;
}

std::wstring Widen(const std::string& value)
{
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring out(size_t(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), out.data(), count);
    return out;
}

std::string JsonString(const std::string& json, const char* key)
{
    const std::string needle = std::string("\"") + key + "\"";
    size_t at = json.find(needle);
    if (at == std::string::npos || (at = json.find(':', at + needle.size())) == std::string::npos) return {};
    at = json.find('"', at + 1);
    if (at == std::string::npos) return {};
    const size_t end = json.find('"', at + 1);
    if (end == std::string::npos) return {};
    return json.substr(at + 1, end - at - 1);
}

std::string EscapeJson(const char* value)
{
    std::string out;
    for (; *value; ++value)
    {
        if (*value == '"' || *value == '\\') out.push_back('\\');
        if (static_cast<unsigned char>(*value) < 0x20) return {};
        out.push_back(*value);
    }
    return out;
}

bool Post(const std::string& base, const char* token, const char* route, const std::string& payload,
          std::string& response)
{
    response.clear();
    const std::wstring wideBase = Widen(base);
    const std::wstring wideToken = Widen(token);
    if (wideBase.empty() || wideToken.empty()) return false;
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[512] = {};
    parts.lpszHostName = host; parts.dwHostNameLength = _countof(host);
    parts.lpszUrlPath = path; parts.dwUrlPathLength = _countof(path);
    if (!WinHttpCrackUrl(wideBase.c_str(), 0, 0, &parts) || parts.dwHostNameLength == 0 || parts.dwHostNameLength >= _countof(host)) return false;
    std::wstring fullPath(path, parts.dwUrlPathLength);
    while (!fullPath.empty() && fullPath.back() == L'/') fullPath.pop_back();
    fullPath += Widen(std::string(route));
    HINTERNET session = WinHttpOpen(L"TTT2 Ranked Runtime/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 3000, 3000, 5000, 5000);
    HINTERNET connection = WinHttpConnect(session, std::wstring(host, parts.dwHostNameLength).c_str(), parts.nPort, 0);
    if (!connection) { WinHttpCloseHandle(session); return false; }
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connection, L"POST", fullPath.c_str(), nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) { WinHttpCloseHandle(connection); WinHttpCloseHandle(session); return false; }
    const std::wstring headers = L"Content-Type: application/json\r\nAuthorization: Bearer " + wideToken + L"\r\n";
    BOOL ok = WinHttpSendRequest(request, headers.c_str(), DWORD(headers.size()),
                                 const_cast<char*>(payload.data()), DWORD(payload.size()), DWORD(payload.size()), 0) &&
              WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0, statusBytes = sizeof(status);
    ok = ok && WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                   WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusBytes, WINHTTP_NO_HEADER_INDEX) &&
         status == 200;
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

bool ParseEndpoint(const std::string& text, char ip[16], uint16_t& port)
{
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon >= 16 || colon + 1 >= text.size()) return false;
    std::memcpy(ip, text.data(), colon); ip[colon] = '\0';
    char* end = nullptr;
    const unsigned long value = std::strtoul(text.c_str() + colon + 1, &end, 10);
    if (!end || *end || !value || value > 65535) return false;
    port = uint16_t(value);
    return true;
}

bool DecodeHex16(const std::string& hex, uint8_t out[16])
{
    if (hex.size() != 32) return false;
    for (size_t i = 0; i < 16; ++i)
    {
        unsigned value = 0;
        if (std::sscanf(hex.substr(i * 2, 2).c_str(), "%2x", &value) != 1) return false;
        out[i] = uint8_t(value);
    }
    return true;
}

uint64_t PeerId(const std::string& endpoint)
{
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : endpoint) { hash ^= c; hash *= 1099511628211ull; }
    return 0x52414E4B00000000ull | (hash & 0xFFFFFFFFull);
}

void Error(char* error, uint32_t capacity, const char* message)
{
    if (error && capacity) { std::strncpy(error, message, capacity - 1); error[capacity - 1] = '\0'; }
}
} // namespace

bool Enabled() { return std::strcmp(Env("TTT2_RANKED_RUNTIME"), "1") == 0; }

bool AcquireReservation(Reservation& result, char* error, uint32_t errorCapacity)
{
    const char* base = Env("TTT2_RANKED_SERVICE");
    const char* token = Env("TTT2_RANKED_TOKEN");
    const char* player = Env("TTT2_RANKED_PLAYER");
    const char* build = Env("TTT2_RANKED_BUILD", "ttt2-community");
    const char* region = Env("TTT2_RANKED_REGION", "global");
    if (!*base || !*token || !*player) { Error(error, errorCapacity, "Ranked runtime credentials are not configured."); return false; }
    if (!net::LocalPeer()) { Error(error, errorCapacity, "Peer transport has not started yet."); return false; }
    if (std::strcmp(net::BackendName(), "enet-ipv4") != 0)
    { Error(error, errorCapacity, "Ranked IPv4 routing requires the ENet IPv4 transport."); return false; }
    char listenIp[16] = {};
    const char* advertise = Env("TTT2_RANKED_ADVERTISE_IP");
    if (!*advertise)
    {
        if (std::strstr(base, "127.0.0.1") || std::strstr(base, "localhost")) advertise = "127.0.0.1";
        else { Error(error, errorCapacity, "Set TTT2_RANKED_ADVERTISE_IP to a reachable IPv4 address."); return false; }
    }
    strncpy_s(listenIp, advertise, _TRUNCATE);
    const uint16_t listenPort = uint16_t(net::LocalPeer() & 0xFFFFu);
    char endpoint[40] = {};
    if (!listenPort || std::snprintf(endpoint, sizeof(endpoint), "%s:%u", listenIp, unsigned(listenPort)) < 0)
    { Error(error, errorCapacity, "Could not determine the local UDP endpoint."); return false; }
    const std::string body = std::string("{\"build\":\"") + EscapeJson(build) + "\",\"region\":\"" +
                             EscapeJson(region) + "\",\"listen_endpoint\":\"" + endpoint + "\"}";
    std::string response;
    if (!Post(base, token, "/v1/queue", body, response))
    { Error(error, errorCapacity, "Ranked queue request failed."); return false; }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const std::string state = JsonString(response, "state");
        if (state == "reserved") break;
        if (!Post(base, token, "/v1/status", "{}", response))
        { Error(error, errorCapacity, "Ranked status request failed."); return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (JsonString(response, "state") != "reserved")
    { Error(error, errorCapacity, "No opponent reservation was found before the queue timed out."); return false; }
    const std::string matchId = JsonString(response, "id");
    const std::string opponent = JsonString(response, "player_a") == player ? JsonString(response, "player_b") : JsonString(response, "player_a");
    const std::string endpointText = JsonString(response, "opponent_endpoint");
    const std::string key = JsonString(response, "session_key_bytes");
    if (matchId.size() < 16 || opponent.empty() || endpointText.empty() ||
        !ParseEndpoint(endpointText, result.opponentIp, result.opponentPort) || !DecodeHex16(key, result.sessionKey))
    { Error(error, errorCapacity, "Ranked reservation payload was incomplete or malformed."); return false; }
    result.sessionId = 0;
    for (size_t i = 0; i < 16; ++i)
    {
        const char digit = matchId[i];
        const unsigned nibble = digit >= '0' && digit <= '9' ? unsigned(digit - '0') :
                                digit >= 'a' && digit <= 'f' ? unsigned(digit - 'a' + 10) : 16u;
        if (nibble > 15) { Error(error, errorCapacity, "Ranked match ID is invalid."); return false; }
        result.sessionId = (result.sessionId << 4) | nibble;
    }
    const size_t opponentLength = (std::min)(opponent.size(), sizeof(result.opponentId) - 1);
    std::memcpy(result.opponentId, opponent.data(), opponentLength);
    result.opponentId[opponentLength] = '\0';
    result.peerId = PeerId(endpointText);
    if (!net::RegisterPeer(result.peerId, result.opponentIp, result.opponentPort))
    { Error(error, errorCapacity, "Could not register the opponent IPv4 endpoint."); return false; }
    const std::string readyBody = std::string("{\"match_id\":\"") + matchId + "\"}";
    if (!Post(base, token, "/v1/ready", readyBody, response))
    { Error(error, errorCapacity, "Could not mark the ranked reservation ready."); return false; }
    const auto readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (JsonString(response, "state") != "active" && std::chrono::steady_clock::now() < readyDeadline)
    {
        if (!Post(base, token, "/v1/status", "{}", response))
        { Error(error, errorCapacity, "Ranked activation status request failed."); return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (JsonString(response, "state") != "active")
    { Error(error, errorCapacity, "Opponent did not ready the ranked reservation in time."); return false; }
    return true;
}
} // namespace ranked
