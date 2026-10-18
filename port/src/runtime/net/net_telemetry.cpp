#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "net_telemetry.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
namespace net::telemetry {
namespace {
static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<float>::is_always_lock_free);
template<size_t N> struct Text {
    std::array<std::atomic<char>,N> data{};
    void Set(const char* value) { size_t i=0; for(;value && value[i] && i+1<N;++i) data[i].store(value[i],std::memory_order_relaxed); for(;i<N;++i)data[i].store(0,std::memory_order_relaxed); }
    void Get(char* out) const { for(size_t i=0;i<N;++i)out[i]=data[i].load(std::memory_order_relaxed);out[N-1]=0; }
};
Text<64> adapter, peer; Text<16> local;
std::atomic<State> state{State::Idle};
std::atomic<bool> valid{false}, redundancy{false}, simValid{false};
std::atomic<float> rtt{0}, jitter{0}, rift{0};
std::atomic<int> pace{0};
std::atomic<uint64_t> recoveredTotal{0}, lostTotal{0};
std::atomic<uint32_t> samples{0};
struct Point { std::atomic<uint64_t> tag{0}; std::atomic<float> rtt{0},jitter{0}; std::atomic<uint8_t> event{0}; };
std::array<Point,kHistory> history;
std::atomic<int> hud{-1};
}
uint64_t NowMs() {
    static const int64_t frequency=[] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    LARGE_INTEGER q{};QueryPerformanceCounter(&q);
    return uint64_t((q.QuadPart/frequency)*1000+(q.QuadPart%frequency)*1000/frequency);
}
void SetAdapter(const char* name,const char* ip,bool enabled){adapter.Set(name);local.Set(ip);redundancy=enabled;}
void SetPeer(const char* endpoint,State value){peer.Set(endpoint);state=value;}
void SetState(State value){state=value;}
void Observe(float sample,uint32_t recovered,uint32_t lost,bool validRtt){
    recoveredTotal.fetch_add(recovered,std::memory_order_relaxed);lostTotal.fetch_add(lost,std::memory_order_relaxed);
    float smooth=rtt.load(std::memory_order_relaxed), j=jitter.load(std::memory_order_relaxed);
    if(validRtt && std::isfinite(sample) && sample>=0 && sample<=10000){
        if(!valid.exchange(true)){smooth=sample;j=0;}else{j+=(std::abs(sample-smooth)-j)*0.25f;smooth+=(sample-smooth)*0.125f;}
        rtt.store(smooth,std::memory_order_relaxed);jitter.store(j,std::memory_order_relaxed);
    }
    if(!validRtt && !recovered && !lost)return;
    const uint32_t index=samples.fetch_add(1,std::memory_order_relaxed);auto& p=history[index%kHistory];
    p.tag.store(0,std::memory_order_release);p.rtt.store(smooth,std::memory_order_relaxed);p.jitter.store(j,std::memory_order_relaxed);
    p.event.store(lost?2:recovered?1:0,std::memory_order_relaxed);p.tag.store(uint64_t(index)+1,std::memory_order_release);
}
Snapshot Read(){
    Snapshot s;adapter.Get(s.adapter_name);local.Get(s.local_ip);peer.Get(s.peer_endpoint);s.state=state.load();
    s.rtt_valid=valid.load();s.redundancy_enabled=redundancy.load();s.simulation_sync_valid=simValid.load();
    s.smoothed_rtt_ms=rtt.load();s.jitter_ms=jitter.load();s.frame_rift_delta=rift.load();s.pacing_adjust_us=pace.load();
    s.recovered_frames_count=recoveredTotal.load();s.unrecovered_drops_count=lostTotal.load();
    const uint32_t end=samples.load();const uint32_t count=(std::min)(end,uint32_t(kHistory));
    for(uint32_t i=0;i<count;++i){const uint32_t index=end-count+i;const auto& p=history[index%kHistory];const auto before=p.tag.load(std::memory_order_acquire);
        const float a=p.rtt.load(),b=p.jitter.load();const auto e=p.event.load();
        if(before==uint64_t(index)+1 && p.tag.load(std::memory_order_acquire)==before){const auto n=s.sample_count++;s.rtt_graph_ring[n]=a;s.jitter_graph_ring[n]=b;s.graph_event[n]=e;}}
    return s;
}
void SetSimulationSync(float value,int us,bool enabled){rift=value;pace=us;simValid=enabled;}
void SetHudVisible(bool value){hud=value?1:0;}
bool HudVisible(){int v=hud.load();if(v<0){const char* e=std::getenv("TTT2_NET_HUD");int initial=e&&std::strcmp(e,"1")==0?1:0;hud.compare_exchange_strong(v,initial);v=hud.load();}return v!=0;}
void ToggleHud(){SetHudVisible(!HudVisible());}
}
