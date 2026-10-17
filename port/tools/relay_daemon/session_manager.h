#pragma once

#include "socket_compat.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace relay {

struct Endpoint {
    sockaddr_in address{};
    bool known = false;
};

class SessionManager {
public:
    bool Register(const std::string& matchId, const std::string& tokenHex,
                  const std::string& player1, const std::string& player2, uint32_t ttlSeconds);
    bool Remove(const std::string& matchId);
    bool Route(const uint8_t* token, uint8_t role, const sockaddr_in& source, Endpoint& destination);
    size_t PruneExpired();
    size_t Size() const;

private:
    struct Session {
        std::string matchId;
        std::string tokenHex;
        std::string player1;
        std::string player2;
        uint32_t ttlSeconds = 1200;
        Endpoint endpoint1;
        Endpoint endpoint2;
        std::chrono::steady_clock::time_point lastActivity = std::chrono::steady_clock::now();
        mutable std::mutex mutex;
    };

    static bool IsHexId(const std::string& value);
    static bool IsPlayerId(const std::string& value);
    static bool DecodeToken(const std::string& hex, std::array<uint8_t, 16>& token);
    static std::string TokenKey(const uint8_t* token);

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessionsByToken_;
    std::unordered_map<std::string, std::string> tokenByMatch_;
};

} // namespace relay
