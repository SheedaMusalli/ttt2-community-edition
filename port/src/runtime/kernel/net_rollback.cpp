#include "net_rollback.h"

#include <utility>

namespace rollback {

uint32_t StateChecksum(std::span<const uint8_t> bytes)
{
    uint32_t hash = 2166136261u;
    for (uint8_t byte : bytes) hash = (hash ^ byte) * 16777619u;
    return hash;
}

Manager::Manager(Callbacks callbacks, size_t historyFrames, uint32_t inputDelay)
    : callbacks_(std::move(callbacks)),
      history_(std::clamp<size_t>(historyFrames, 2, 10)),
      inputDelay_(std::min<uint32_t>(inputDelay, 8))
{
    delayedLocal_.assign(inputDelay_, 0);
}

bool Manager::Capture(GameSnapshot& out, uint32_t frame)
{
    out.frame_index = frame;
    if (!callbacks_.save || !callbacks_.save(out.state_buffer)) return false;
    out.checksum = StateChecksum(out.state_buffer);
    return true;
}

bool Manager::Initialize(Input initialRemote)
{
    if (!callbacks_.save || !callbacks_.load || !callbacks_.step) return false;
    GameSnapshot initial;
    if (!Capture(initial, 0)) return false;
    currentFrame_ = 0;
    lastRemote_ = initialRemote;
    initialRemote_ = initialRemote;
    rollbackCount_ = 0;
    lastRollbackDepth_ = 0;
    remoteInputs_.clear();
    for (Record& record : history_) record = {};
    initialized_ = true;
    return true;
}

Manager::Record* Manager::Find(uint32_t frame)
{
    Record& record = history_[frame % history_.size()];
    return record.valid && record.frame == frame ? &record : nullptr;
}

const Manager::Record* Manager::Find(uint32_t frame) const
{
    const Record& record = history_[frame % history_.size()];
    return record.valid && record.frame == frame ? &record : nullptr;
}

bool Manager::SimulateRecord(Record& record, Input& previousRemote, bool resimulating)
{
    const auto actual = remoteInputs_.find(record.frame);
    record.remoteWasActual = actual != remoteInputs_.end();
    record.remote = record.remoteWasActual ? actual->second : previousRemote;
    if (!callbacks_.step(record.local, record.remote, resimulating)) return false;
    previousRemote = record.remote;
    return true;
}

bool Manager::Advance(Input localInput)
{
    if (!initialized_) return false;
    Record& record = history_[currentFrame_ % history_.size()];
    record = {};
    record.valid = true;
    record.frame = currentFrame_;
    if (!Capture(record.snapshot, currentFrame_)) { record.valid = false; return false; }

    delayedLocal_.push_back(localInput);
    record.local = delayedLocal_.front();
    delayedLocal_.pop_front();

    Input previous = lastRemote_;
    if (!SimulateRecord(record, previous, false)) { record.valid = false; return false; }
    lastRemote_ = previous;
    ++currentFrame_;
    PruneOldInputs();
    return true;
}

bool Manager::ReceiveRemote(uint32_t frame, Input input)
{
    if (!initialized_ || (frame > currentFrame_ && frame - currentFrame_ > 120)) return false;
    if (frame < currentFrame_ && !Find(frame)) return false;
    auto existing = remoteInputs_.find(frame);
    if (existing != remoteInputs_.end() && existing->second != input) return false;
    remoteInputs_[frame] = input;

    if (frame >= currentFrame_) return true;
    Record* changed = Find(frame);
    if (!changed) return false;
    if (changed->remote == input)
    {
        changed->remoteWasActual = true;
        return true;
    }

    const uint32_t oldCurrent = currentFrame_;
    if (!callbacks_.load(changed->snapshot.state_buffer)) return false;
    Input previous = frame == 0 ? initialRemote_ : (Find(frame - 1) ? Find(frame - 1)->remote : initialRemote_);
    for (uint32_t replayFrame = frame; replayFrame < oldCurrent; ++replayFrame)
    {
        Record* record = Find(replayFrame);
        if (!record || !Capture(record->snapshot, replayFrame) || !SimulateRecord(*record, previous, true))
            return false;
    }
    lastRemote_ = previous;
    ++rollbackCount_;
    lastRollbackDepth_ = oldCurrent - frame;
    PruneOldInputs();
    return true;
}

void Manager::PruneOldInputs()
{
    const uint32_t oldest = currentFrame_ > history_.size()
        ? currentFrame_ - static_cast<uint32_t>(history_.size()) : 0;
    for (auto it = remoteInputs_.begin(); it != remoteInputs_.end();)
    {
        if (it->first < oldest) it = remoteInputs_.erase(it);
        else ++it;
    }
}

const GameSnapshot* Manager::Snapshot(uint32_t frame) const
{
    const Record* record = Find(frame);
    return record ? &record->snapshot : nullptr;
}

} // namespace rollback
