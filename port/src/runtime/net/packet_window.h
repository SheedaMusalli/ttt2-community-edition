#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>
namespace net {
// Packet recovery only: sequence numbers refer to datagrams, never guest simulation frames.
constexpr size_t kRedundancyWindow=4, kMaxRecoveryPayload=512, kRecoveryHeader=28;
struct RecoveryResult {
    bool valid=false, rtt_valid=false;
    float rtt_ms=0;
    uint32_t recovered=0, lost=0;
    std::vector<std::vector<uint8_t>> payloads;
};
class PacketWindow {
public:
    explicit PacketWindow(uint32_t epoch=1):epoch_(epoch){}
    std::vector<uint8_t> Encode(const uint8_t* data,size_t size,uint64_t nowMs);
    RecoveryResult Decode(const uint8_t* data,size_t size,uint64_t nowMs);
    static bool HasMagic(const uint8_t* data,size_t size);
private:
    uint32_t epoch_=1, nextSend_=0, remoteEpoch_=0, nextReceive_=0;
    bool haveRemote_=false, haveEcho_=false;
    uint16_t remoteTick_=0;
    uint64_t lastEchoSample_=0;
    std::deque<std::vector<uint8_t>> sent_;
    std::deque<uint64_t> timestamps_;
    std::array<uint32_t,4> retired_{};
    size_t retiredIndex_=0;
};
}
