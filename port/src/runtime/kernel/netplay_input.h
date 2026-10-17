#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace netplay {

// Host-neutral controller sample. Wire serialization is explicit; no platform/XInput
// structure layout or guest ABI is exposed to the transport.
struct PadInput
{
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0;
    uint8_t rightTrigger = 0;
    int16_t leftX = 0;
    int16_t leftY = 0;
    int16_t rightX = 0;
    int16_t rightY = 0;

    bool operator==(const PadInput&) const = default;
};

constexpr size_t kInputPacketSize = 24;
using InputPacket = std::array<uint8_t, kInputPacketSize>;
bool EncodeInputPacket(uint32_t frame, const PadInput& input, InputPacket& out);
bool DecodeInputPacket(const uint8_t* bytes, size_t size, uint32_t& frame, PadInput& input);

// Bounded, single-owner timeline for a lockstep caller. Capture a local sample at sampleFrame,
// send the returned target frame to the peer, buffer received peer input under that same frame,
// then call TryConsume(frame) at the verified simulation boundary. A false return means the
// caller must not advance that simulation frame. This class never blocks or touches guest state.
class SynchronizedInputBuffer
{
public:
    explicit SynchronizedInputBuffer(uint32_t delayFrames = 2, size_t capacity = 64);
    void Reset(uint32_t currentFrame);
    bool QueueLocal(uint32_t sampleFrame, const PadInput& input, uint32_t& targetFrame);
    bool QueueRemote(uint32_t frame, const PadInput& input);
    bool TryConsume(uint32_t frame, PadInput& local, PadInput& remote);
    uint32_t CurrentFrame() const { return currentFrame_; }
    uint32_t DelayFrames() const { return delayFrames_; }

private:
    struct Slot
    {
        bool localPresent = false;
        bool remotePresent = false;
        PadInput local{};
        PadInput remote{};
    };
    bool InWindow(uint32_t frame) const;

    std::array<Slot, 128> slots_{};
    size_t capacity_ = 64;
    uint32_t currentFrame_ = 0;
    uint32_t delayFrames_ = 2;
};

} // namespace netplay
