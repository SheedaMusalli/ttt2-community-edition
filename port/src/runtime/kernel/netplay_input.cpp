#include "netplay_input.h"

#include <algorithm>

namespace netplay {
namespace {
constexpr uint8_t kMagic[4] = { 'T', '2', 'I', 'N' };
constexpr uint8_t kVersion = 1;

void Put16(uint8_t* out, uint16_t value)
{
    out[0] = static_cast<uint8_t>(value >> 8);
    out[1] = static_cast<uint8_t>(value);
}

uint16_t Get16(const uint8_t* in)
{
    return static_cast<uint16_t>((uint16_t(in[0]) << 8) | in[1]);
}

void Put32(uint8_t* out, uint32_t value)
{
    out[0] = static_cast<uint8_t>(value >> 24);
    out[1] = static_cast<uint8_t>(value >> 16);
    out[2] = static_cast<uint8_t>(value >> 8);
    out[3] = static_cast<uint8_t>(value);
}

uint32_t Get32(const uint8_t* in)
{
    return (uint32_t(in[0]) << 24) | (uint32_t(in[1]) << 16) |
           (uint32_t(in[2]) << 8) | in[3];
}

void PutSigned16(uint8_t* out, int16_t value)
{
    Put16(out, static_cast<uint16_t>(value));
}

int16_t GetSigned16(const uint8_t* in)
{
    return static_cast<int16_t>(Get16(in));
}
} // namespace

bool EncodeInputPacket(uint32_t frame, const PadInput& input, InputPacket& out)
{
    out.fill(0);
    std::copy(std::begin(kMagic), std::end(kMagic), out.begin());
    out[4] = kVersion;
    out[5] = 0;
    Put16(out.data() + 6, static_cast<uint16_t>(kInputPacketSize));
    Put32(out.data() + 8, frame);
    Put16(out.data() + 12, input.buttons);
    out[14] = input.leftTrigger;
    out[15] = input.rightTrigger;
    PutSigned16(out.data() + 16, input.leftX);
    PutSigned16(out.data() + 18, input.leftY);
    PutSigned16(out.data() + 20, input.rightX);
    PutSigned16(out.data() + 22, input.rightY);
    return true;
}

bool DecodeInputPacket(const uint8_t* bytes, size_t size, uint32_t& frame, PadInput& input)
{
    if (!bytes || size != kInputPacketSize || !std::equal(std::begin(kMagic), std::end(kMagic), bytes) ||
        bytes[4] != kVersion || bytes[5] != 0 || Get16(bytes + 6) != kInputPacketSize)
        return false;
    frame = Get32(bytes + 8);
    input.buttons = Get16(bytes + 12);
    input.leftTrigger = bytes[14];
    input.rightTrigger = bytes[15];
    input.leftX = GetSigned16(bytes + 16);
    input.leftY = GetSigned16(bytes + 18);
    input.rightX = GetSigned16(bytes + 20);
    input.rightY = GetSigned16(bytes + 22);
    return true;
}

SynchronizedInputBuffer::SynchronizedInputBuffer(uint32_t delayFrames, size_t capacity)
    : capacity_(std::clamp<size_t>(capacity, 2, slots_.size())),
      delayFrames_(std::clamp<uint32_t>(delayFrames, 1, 8))
{}

void SynchronizedInputBuffer::Reset(uint32_t currentFrame)
{
    currentFrame_ = currentFrame;
    slots_.fill({});
}

bool SynchronizedInputBuffer::InWindow(uint32_t frame) const
{
    return frame >= currentFrame_ && uint64_t(frame) - currentFrame_ < capacity_;
}

bool SynchronizedInputBuffer::QueueLocal(uint32_t sampleFrame, const PadInput& input, uint32_t& targetFrame)
{
    if (sampleFrame > UINT32_MAX - delayFrames_) return false;
    targetFrame = sampleFrame + delayFrames_;
    if (!InWindow(targetFrame)) return false;
    Slot& slot = slots_[targetFrame % slots_.size()];
    if (slot.localPresent && slot.local != input) return false;
    slot.localPresent = true;
    slot.local = input;
    return true;
}

bool SynchronizedInputBuffer::QueueRemote(uint32_t frame, const PadInput& input)
{
    if (!InWindow(frame)) return false;
    Slot& slot = slots_[frame % slots_.size()];
    if (slot.remotePresent && slot.remote != input) return false;
    slot.remotePresent = true;
    slot.remote = input;
    return true;
}

bool SynchronizedInputBuffer::TryConsume(uint32_t frame, PadInput& local, PadInput& remote)
{
    if (frame != currentFrame_ || !InWindow(frame)) return false;
    Slot& slot = slots_[frame % slots_.size()];
    if (!slot.localPresent || !slot.remotePresent) return false;
    local = slot.local;
    remote = slot.remote;
    slot = {};
    ++currentFrame_;
    return true;
}

} // namespace netplay
