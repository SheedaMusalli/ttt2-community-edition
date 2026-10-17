#include "net_rollback.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
struct State { uint64_t value = 0; uint32_t ticks = 0; };

std::vector<uint8_t> Encode(const State& state)
{
    std::vector<uint8_t> bytes(sizeof(state.value) + sizeof(state.ticks));
    std::memcpy(bytes.data(), &state.value, sizeof(state.value));
    std::memcpy(bytes.data() + sizeof(state.value), &state.ticks, sizeof(state.ticks));
    return bytes;
}

bool Decode(State& state, std::span<const uint8_t> bytes)
{
    if (bytes.size() != sizeof(state.value) + sizeof(state.ticks)) return false;
    std::memcpy(&state.value, bytes.data(), sizeof(state.value));
    std::memcpy(&state.ticks, bytes.data() + sizeof(state.value), sizeof(state.ticks));
    return true;
}

void Step(State& state, uint32_t local, uint32_t remote)
{
    state.value = (state.value ^ (uint64_t(local) << 32 | remote)) * 0x9E3779B185EBCA87ull + state.ticks;
    ++state.ticks;
}

bool Check(bool condition, const char* message)
{
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}
} // namespace

int main()
{
    constexpr uint32_t frameCount = 9;
    const std::array<uint32_t, frameCount> local = { 1, 2, 4, 8, 16, 32, 64, 128, 256 };
    const std::array<uint32_t, frameCount> remote = { 0, 0, 3, 3, 5, 5, 5, 7, 7 };

    State corrected;
    rollback::Manager manager({
        [&corrected](std::vector<uint8_t>& bytes) { bytes = Encode(corrected); return true; },
        [&corrected](std::span<const uint8_t> bytes) { return Decode(corrected, bytes); },
        [&corrected](uint32_t l, uint32_t r, bool) { Step(corrected, l, r); return true; }
    }, 10, 0);

    bool ok = Check(manager.Initialize(), "manager initializes");
    for (uint32_t frame = 0; frame < frameCount; ++frame)
    {
        if (frame == 7) ok &= Check(manager.ReceiveRemote(frame, remote[frame]), "future input is buffered");
        ok &= Check(manager.Advance(local[frame]), "predicted frame advances");
    }
    ok &= Check(manager.ReceiveRemote(4, remote[4]), "late frame input is accepted");
    ok &= Check(manager.ReceiveRemote(2, remote[2]), "second late input is accepted");

    State reference;
    for (uint32_t frame = 0; frame < frameCount; ++frame) Step(reference, local[frame], remote[frame]);
    ok &= Check(corrected.value == reference.value && corrected.ticks == reference.ticks,
                "late inputs rollback and resimulate to bit-identical linear state");
    ok &= Check(manager.RollbackCount() == 2, "each changed late prediction triggers one rollback");
    ok &= Check(manager.LastRollbackDepth() == 7, "rollback depth reports frames replayed");

    const rollback::GameSnapshot* snapshot = manager.Snapshot(4);
    ok &= Check(snapshot && snapshot->frame_index == 4 &&
                snapshot->checksum == rollback::StateChecksum(snapshot->state_buffer),
                "historical snapshot checksum is stable");
    ok &= Check(!manager.ReceiveRemote(4, 99), "conflicting duplicate remote input is rejected");

    State delayed;
    rollback::Manager delayedManager({
        [&delayed](std::vector<uint8_t>& bytes) { bytes = Encode(delayed); return true; },
        [&delayed](std::span<const uint8_t> bytes) { return Decode(delayed, bytes); },
        [&delayed](uint32_t l, uint32_t r, bool) { Step(delayed, l, r); return true; }
    }, 10, 2);
    ok &= Check(delayedManager.Initialize(), "delayed manager initializes");
    ok &= Check(delayedManager.Advance(11) && delayedManager.Advance(22) && delayedManager.Advance(33),
                "delayed local inputs advance");
    State delayedReference;
    Step(delayedReference, 0, 0);
    Step(delayedReference, 0, 0);
    Step(delayedReference, 11, 0);
    ok &= Check(delayed.value == delayedReference.value, "configured two-frame local input delay is applied");
    ok &= Check(manager.Snapshot(0) != nullptr, "snapshot remains available inside the history window");

    State bounded;
    rollback::Manager boundedManager({
        [&bounded](std::vector<uint8_t>& bytes) { bytes = Encode(bounded); return true; },
        [&bounded](std::span<const uint8_t> bytes) { return Decode(bounded, bytes); },
        [&bounded](uint32_t l, uint32_t r, bool) { Step(bounded, l, r); return true; }
    }, 2, 0);
    ok &= Check(boundedManager.Initialize() && boundedManager.Advance(1) &&
                boundedManager.Advance(2) && boundedManager.Advance(3), "bounded history advances");
    ok &= Check(!boundedManager.ReceiveRemote(0, 9), "input older than retained snapshot window is rejected");

    if (!ok) return 1;
    std::puts("PASS: rollback loop, prediction, bounded snapshots, checksum, and input delay");
    return 0;
}
