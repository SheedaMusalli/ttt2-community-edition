#pragma once
#include <cstdint>

namespace ranked {
struct Reservation
{
    uint64_t sessionId = 0;
    uint64_t peerId = 0;
    char opponentId[65] = {};
    uint8_t sessionKey[16] = {};
    char opponentIp[16] = {};
    uint16_t opponentPort = 0;
};

// Uses TTT2_RANKED_SERVICE, TTT2_RANKED_TOKEN, TTT2_RANKED_PLAYER, optional
// TTT2_RANKED_BUILD / TTT2_RANKED_REGION / TTT2_RANKED_ADVERTISE_IP.
bool Enabled();
bool AcquireReservation(Reservation& result, char* error, uint32_t errorCapacity);
} // namespace ranked
