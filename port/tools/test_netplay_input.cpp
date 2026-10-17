#include "netplay_input.h"

#include <cstdio>

namespace {
bool Check(bool condition, const char* message)
{
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}
}

int main()
{
    bool ok = true;
    const netplay::PadInput local{ 0x1234, 12, 34, -32768, 1000, -2000, 32767 };
    netplay::InputPacket packet{};
    ok &= Check(netplay::EncodeInputPacket(0x01020304, local, packet), "encode input packet");
    uint32_t decodedFrame = 0;
    netplay::PadInput decoded{};
    ok &= Check(netplay::DecodeInputPacket(packet.data(), packet.size(), decodedFrame, decoded) &&
                decodedFrame == 0x01020304 && decoded == local, "packet round trip preserves frame and all controls");
    ok &= Check(!netplay::DecodeInputPacket(packet.data(), packet.size() - 1, decodedFrame, decoded),
                "truncated packet is rejected");
    packet[4] = 99;
    ok &= Check(!netplay::DecodeInputPacket(packet.data(), packet.size(), decodedFrame, decoded),
                "unknown packet version is rejected");

    netplay::SynchronizedInputBuffer timeline(3, 8);
    netplay::SynchronizedInputBuffer peerTimeline(3, 8);
    const netplay::PadInput remote{ 0x00A5, 255, 0, 120, -450, 600, -700 };
    timeline.Reset(3);
    peerTimeline.Reset(3);
    uint32_t target = 0;
    uint32_t peerTarget = 0;
    ok &= Check(timeline.QueueLocal(0, local, target) && target == 3, "local input is assigned configured delayed frame");
    ok &= Check(peerTimeline.QueueLocal(0, remote, peerTarget) && peerTarget == target,
                "peer input is assigned the same delayed frame");
    netplay::PadInput gotLocal{}, gotRemote{};
    ok &= Check(!timeline.TryConsume(3, gotLocal, gotRemote), "frame stalls until remote input arrives");
    netplay::InputPacket peerPacket{};
    ok &= Check(netplay::EncodeInputPacket(peerTarget, remote, peerPacket), "peer serializes input for transport");
    uint32_t wireFrame = 0;
    netplay::PadInput wireInput{};
    ok &= Check(netplay::DecodeInputPacket(peerPacket.data(), peerPacket.size(), wireFrame, wireInput) &&
                timeline.QueueRemote(wireFrame, wireInput), "received peer frame is buffered");
    netplay::InputPacket localPacket{};
    ok &= Check(netplay::EncodeInputPacket(target, local, localPacket) &&
                netplay::DecodeInputPacket(localPacket.data(), localPacket.size(), wireFrame, wireInput) &&
                peerTimeline.QueueRemote(wireFrame, wireInput), "peer buffers received local frame");
    ok &= Check(timeline.TryConsume(3, gotLocal, gotRemote) && gotLocal == local && gotRemote == remote,
                "frame advances with the exact paired input samples");
    ok &= Check(peerTimeline.TryConsume(3, gotLocal, gotRemote) && gotLocal == remote && gotRemote == local,
                "both peers advance with symmetric player inputs");
    ok &= Check(timeline.CurrentFrame() == 4, "successful consume advances exactly one frame");
    ok &= Check(!timeline.QueueRemote(3, remote), "stale frame packet is rejected");

    uint32_t future = 0;
    ok &= Check(!timeline.QueueLocal(100, local, future), "out-of-window local input is rejected");
    ok &= Check(!timeline.QueueRemote(100, remote), "out-of-window remote input is rejected");

    if (!ok) return 1;
    std::puts("PASS: synchronized input framing, delay assignment, packet validation, and tick gate");
    return 0;
}
