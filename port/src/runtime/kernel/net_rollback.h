#pragma once

// Standalone rollback coordinator. It deliberately knows nothing about the guest ABI:
// callers provide deterministic save/load/step functions after establishing a verified
// simulation-state boundary.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

namespace rollback {

struct GameSnapshot
{
    uint32_t frame_index = 0;
    uint32_t checksum = 0;
    std::vector<uint8_t> state_buffer;
};

uint32_t StateChecksum(std::span<const uint8_t> bytes);

class Manager
{
public:
    using Input = uint32_t;
    using SaveFn = std::function<bool(std::vector<uint8_t>&)>;
    using LoadFn = std::function<bool(std::span<const uint8_t>)>;
    using StepFn = std::function<bool(Input local, Input remote, bool resimulating)>;

    struct Callbacks { SaveFn save; LoadFn load; StepFn step; };

    // historyFrames is clamped to [2, 10], matching the bounded rollback window.
    explicit Manager(Callbacks callbacks, size_t historyFrames = 10, uint32_t inputDelay = 1);
    bool Initialize(Input initialRemote = 0);
    bool Advance(Input localInput);
    bool ReceiveRemote(uint32_t frame, Input input);

    uint32_t CurrentFrame() const { return currentFrame_; }
    uint32_t RollbackCount() const { return rollbackCount_; }
    uint32_t LastRollbackDepth() const { return lastRollbackDepth_; }
    uint32_t InputDelay() const { return inputDelay_; }
    const GameSnapshot* Snapshot(uint32_t frame) const;

private:
    struct Record
    {
        bool valid = false;
        uint32_t frame = 0;
        GameSnapshot snapshot;
        Input local = 0;
        Input remote = 0;
        bool remoteWasActual = false;
    };

    Record* Find(uint32_t frame);
    const Record* Find(uint32_t frame) const;
    bool Capture(GameSnapshot& out, uint32_t frame);
    bool SimulateRecord(Record& record, Input& previousRemote, bool resimulating);
    void PruneOldInputs();

    Callbacks callbacks_;
    std::vector<Record> history_;
    std::deque<Input> delayedLocal_;
    std::unordered_map<uint32_t, Input> remoteInputs_;
    uint32_t currentFrame_ = 0;
    uint32_t inputDelay_ = 0;
    Input lastRemote_ = 0;
    Input initialRemote_ = 0;
    uint32_t rollbackCount_ = 0;
    uint32_t lastRollbackDepth_ = 0;
    bool initialized_ = false;
};

} // namespace rollback
