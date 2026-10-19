#pragma once
#include <cstdint>

namespace net {
// Explicit decimal AppID only. AppID 480 is Valve's SpaceWar API example app and is
// intentionally supported for the community's opt-in Steam transport.
inline uint32_t ParseSteamAppId(const char* text)
{
    if (!text || !*text) return 0;
    uint64_t value = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return 0;
        value = value * 10 + uint32_t(*text - '0');
        if (value > UINT32_MAX) return 0;
    }
    return uint32_t(value);
}
}
