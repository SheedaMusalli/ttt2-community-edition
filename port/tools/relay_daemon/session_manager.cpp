#include "session_manager.h"
#include <algorithm>
#include <cstring>

namespace relay {
namespace {
int Nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}

bool SessionManager::IsHexId(const std::string& value)
{
    return value.size() == 32 && std::all_of(value.begin(), value.end(), [](char c) { return Nibble(c) >= 0; });
}

bool SessionManager::IsPlayerId(const std::string& value)
{
    return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    });
}

bool SessionManager::DecodeToken(const std::string& hex, std::array<uint8_t, 16>& token)
{
    if (hex.size() != 32) return false;
    for (size_t i = 0; i < token.size(); ++i) {
        const int high = Nibble(hex[i * 2]), low = Nibble(hex[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        token[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return std::any_of(token.begin(), token.end(), [](uint8_t b) { return b != 0; });
}

std::string SessionManager::TokenKey(const uint8_t* token)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string key(32, '0');
    for (size_t i = 0; i < 16; ++i) {
        key[i * 2] = hex[token[i] >> 4];
        key[i * 2 + 1] = hex[token[i] & 15];
    }
    return key;
}

bool SessionManager::Register(const std::string& matchId, const std::string& tokenHex,
                              const std::string& player1, const std::string& player2, uint32_t ttlSeconds)
{
    std::array<uint8_t, 16> token{};
    if (!IsHexId(matchId) || !DecodeToken(tokenHex, token) || !IsPlayerId(player1) ||
        !IsPlayerId(player2) || player1 == player2 || ttlSeconds < 30 || ttlSeconds > 86400) return false;
    auto session = std::make_shared<Session>();
    session->matchId = matchId;
    session->tokenHex = TokenKey(token.data());
    session->player1 = player1;
    session->player2 = player2;
    session->ttlSeconds = ttlSeconds;
    std::unique_lock lock(mutex_);
    auto old = tokenByMatch_.find(matchId);
    if (old != tokenByMatch_.end()) sessionsByToken_.erase(old->second);
    sessionsByToken_[session->tokenHex] = session;
    tokenByMatch_[matchId] = session->tokenHex;
    return true;
}

bool SessionManager::Remove(const std::string& matchId)
{
    std::unique_lock lock(mutex_);
    const auto found = tokenByMatch_.find(matchId);
    if (found == tokenByMatch_.end()) return false;
    sessionsByToken_.erase(found->second);
    tokenByMatch_.erase(found);
    return true;
}

bool SessionManager::Route(const uint8_t* token, uint8_t role, const sockaddr_in& source, Endpoint& destination)
{
    if (!token || (role != 1 && role != 2)) return false;
    const std::string key = TokenKey(token);
    std::shared_ptr<Session> session;
    {
        std::shared_lock lock(mutex_);
        const auto found = sessionsByToken_.find(key);
        if (found == sessionsByToken_.end()) return false;
        session = found->second;
    }
    std::lock_guard lock(session->mutex);
    const auto now = std::chrono::steady_clock::now();
    if (now - session->lastActivity > std::chrono::seconds(session->ttlSeconds)) return false;
    session->lastActivity = now;
    Endpoint& sourceSlot = role == 1 ? session->endpoint1 : session->endpoint2;
    Endpoint& targetSlot = role == 1 ? session->endpoint2 : session->endpoint1;
    sourceSlot.address = source;
    sourceSlot.known = true;
    destination = targetSlot;
    return true;
}

size_t SessionManager::PruneExpired()
{
    const auto now = std::chrono::steady_clock::now();
    std::unique_lock lock(mutex_);
    size_t removed = 0;
    for (auto it = sessionsByToken_.begin(); it != sessionsByToken_.end();) {
        std::lock_guard sessionLock(it->second->mutex);
        if (now - it->second->lastActivity > std::chrono::seconds(it->second->ttlSeconds)) {
            tokenByMatch_.erase(it->second->matchId);
            it = sessionsByToken_.erase(it);
            ++removed;
        } else ++it;
    }
    return removed;
}

size_t SessionManager::Size() const
{
    std::shared_lock lock(mutex_);
    return sessionsByToken_.size();
}

} // namespace relay
