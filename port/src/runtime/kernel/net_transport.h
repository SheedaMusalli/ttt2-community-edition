// Packet transport for virtual XNet sockets. ENet supplies IPv4 peers and delivery semantics;
// the optional Steam implementation remains an explicit separate backend.
#pragma once
#include <cstddef>
#include <cstdint>

namespace net {

// Channel 0 is ENet's sequenced-unreliable mode: late frame inputs are discarded.
// Channel 1 is ENet's reliable ordered mode: session transitions and control messages.
enum class Channel : uint8_t {
    CHANNEL_UNRELIABLE_SEQUENCED = 0,
    CHANNEL_RELIABLE_ORDERED = 1,
    Unreliable = CHANNEL_UNRELIABLE_SEQUENCED, // Existing call-site compatibility.
    Reliable = CHANNEL_RELIABLE_ORDERED
};

// Versioned application handshake sent on the reliable channel after ENet connect.
// Reservation token and player ID must come from the matchmaking service out of band.
// Wire format is encoded explicitly by the caller; never send this host struct as raw bytes.
struct Handshake {
    const char* matchToken = nullptr;
    size_t matchTokenSize = 0;
    const char* playerId = nullptr;
    size_t playerIdSize = 0;
};
constexpr size_t kMaxMatchTokenSize = 128;
constexpr size_t kMaxPlayerIdSize = 64;

inline bool ConstantTimeCompare(const char* a, size_t aSize, const char* b, size_t bSize)
{
    if (!a || !b || aSize != bSize || !aSize) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < aSize; ++i)
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}

// Compares peer credentials to the reservation context. Constant-time token compare avoids
// leaking matching-prefix length; callers must reject and disconnect on false.
inline bool ValidateHandshake(const Handshake& received, const Handshake& expected)
{
    if (!received.matchToken || !expected.matchToken || !received.playerId || !expected.playerId ||
        !received.matchTokenSize || received.matchTokenSize != expected.matchTokenSize ||
        received.matchTokenSize > kMaxMatchTokenSize || !received.playerIdSize ||
        received.playerIdSize != expected.playerIdSize || received.playerIdSize > kMaxPlayerIdSize)
        return false;
    return ConstantTimeCompare(received.matchToken, received.matchTokenSize,
                               expected.matchToken, expected.matchTokenSize) &&
           ConstantTimeCompare(received.playerId, received.playerIdSize,
                               expected.playerId, expected.playerIdSize);
}

using Receiver = void (*)(uint64_t fromPeer, Channel channel, const uint8_t* data, size_t size);

// Starts the backend once (idempotent); packets arrive on its own thread through `receiver`.
bool Start(Receiver receiver);
uint64_t LocalPeer();                   // 0 before Start
// Associates an application peer ID with a numeric IPv4 endpoint. Names are numeric IPv4 only;
// DNS and public-address discovery belong to the matchmaking/configuration layer.
bool RegisterPeer(uint64_t peer, const char* ipv4, uint16_t port);
// Optional NAT-fallback route. The 16-byte relay token is provided by a confirmed reservation;
// role must be 1 or 2. Relay framing is applied internally after the 500 ms direct-connect grace.
bool RegisterRelayPeer(uint64_t peer, const char* ipv4, uint16_t port, const uint8_t token[16], uint8_t role);
bool SetListenPort(uint16_t port);       // set before Start; 0 asks the OS for an ephemeral port
bool Send(uint64_t peer, Channel channel, const void* data, size_t size);
const char* BackendName();

} // namespace net
