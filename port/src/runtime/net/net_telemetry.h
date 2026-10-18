#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
namespace net::telemetry {
constexpr size_t kHistory = 120;
enum class State : uint32_t { Idle, Searching, Connected, Synced };
struct Snapshot {
    char adapter_name[64]{}, local_ip[16]{}, peer_endpoint[64]{};
    State state = State::Idle;
    bool rtt_valid = false, redundancy_enabled = false, simulation_sync_valid = false;
    float smoothed_rtt_ms = 0, jitter_ms = 0, frame_rift_delta = 0;
    int pacing_adjust_us = 0;
    uint64_t recovered_frames_count = 0, unrecovered_drops_count = 0;
    uint32_t sample_count = 0;
    std::array<float,kHistory> rtt_graph_ring{}, jitter_graph_ring{};
    std::array<uint8_t,kHistory> graph_event{}; // 1 recovery, 2 unrecoverable gap
};
// Eventually consistent, bounded atomic reads. No renderer/network mutex, allocation or spin loop.
Snapshot Read();
void SetAdapter(const char* name, const char* ip, bool redundancy);
void SetPeer(const char* endpoint, State state);
void SetState(State state);
void Observe(float rttMs, uint32_t recovered = 0, uint32_t lost = 0, bool validRtt = true);
void SetSimulationSync(float rift, int pacingUs, bool valid);
void SetHudVisible(bool visible);
bool HudVisible();
void ToggleHud();
uint64_t NowMs();
}
