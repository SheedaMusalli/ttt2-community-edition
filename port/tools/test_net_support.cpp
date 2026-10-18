#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "net/packet_window.h"
#include "net/net_adapter.h"
#include "net/net_telemetry.h"
#include "net/net_hud_surface.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <string>
namespace {
void Check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
std::vector<uint8_t> Packet(net::PacketWindow& w,unsigned value,uint64_t time){uint8_t b=uint8_t(value);return w.Encode(&b,1,time);}
void Codec(){
    Check(net::ValidPeerTargets("")&&net::ValidPeerTargets("26.49.12.142, 127.0.0.1:29877"),"valid peer target list");
    for(const char* value:{"invalid","26.1.2.3,","26.1.2.3:65536","26.1.2.3:0","0.0.0.0","224.0.0.1"})Check(!net::ValidPeerTargets(value),"invalid peer target rejection");
    net::PacketWindow a(11),b(22);auto p0=Packet(a,0,65530);auto first=b.Decode(p0.data(),p0.size(),65540);Check(first.valid&&first.payloads.size()==1,"first packet");
    auto echo=Packet(b,50,65540);auto rt=a.Decode(echo.data(),echo.size(),65550);Check(rt.rtt_valid&&rt.rtt_ms==20,"timestamp rollover echo RTT");
    Check(b.Decode(p0.data(),p0.size(),65560).payloads.empty(),"duplicate delivery");
    std::vector<uint8_t> late;for(unsigned i=1;i<4;++i)late=Packet(a,i,65550+i);auto p4=Packet(a,4,65560);
    auto recovered=b.Decode(p4.data(),p4.size(),65570);Check(recovered.valid&&recovered.recovered==3&&recovered.lost==0&&recovered.payloads.size()==4,"recover 3 consecutive drops");
    for(unsigned i=0;i<4;++i)Check(recovered.payloads[i][0]==i+1,"FIFO ordering");
    Check(b.Decode(late.data(),late.size(),65575).payloads.empty(),"out-of-order deduplication");
    for(unsigned i=5;i<9;++i)Packet(a,i,65580+i);auto p9=Packet(a,9,65600);
    Check(!b.Decode(p9.data(),p9.size()-1,65610).valid,"truncation must not change delivery state");
    auto gap=b.Decode(p9.data(),p9.size(),65610);Check(gap.lost==1&&gap.recovered==3&&gap.payloads.size()==4,"gap beyond recovery window");
    auto bad=p9;bad[5]=5;Check(!b.Decode(bad.data(),bad.size(),65612).valid,"malformed count");
    auto invalidRestart=p9;invalidRestart[11]=99;invalidRestart[12]=0x01;invalidRestart[13]=0xFF;
    Check(!b.Decode(invalidRestart.data(),invalidRestart.size(),65612).valid,"implausible restart must not mutate epoch");
    auto p10=Packet(a,10,65613);Check(b.Decode(p10.data(),p10.size(),65614).payloads.size()==1,"live stream survives invalid restart");
    bad=p9;bad[28]=0xFF;bad[29]=0xFF;Check(!b.Decode(bad.data(),bad.size(),65613).valid,"malformed length");
    net::PacketWindow restarted(33);auto reset=Packet(restarted,100,65620);Check(b.Decode(reset.data(),reset.size(),65621).payloads.size()==1,"new stream epoch");Check(!b.Decode(p9.data(),p9.size(),65622).valid,"retired stream epoch replay");
    uint8_t tooLarge[513]{};Check(a.Encode(tooLarge,sizeof(tooLarge),65630).empty(),"oversize exclusion");
    Check(!net::PacketWindow::HasMagic(reinterpret_cast<const uint8_t*>("beacon"),6),"raw beacon passthrough");
    std::puts("Packet codec: 3-drop recovery, 4-drop gap, FIFO, dedup, reorder, restart, malformed lengths, 16-bit timestamp wrap passed");
}
void Telemetry(){
    net::telemetry::SetAdapter("LAN","127.0.0.1",true);net::telemetry::SetPeer("127.0.0.1:3074",net::telemetry::State::Synced);
    const auto before=net::telemetry::Read();std::atomic<bool> done=false;
    std::thread writer([&]{for(int i=0;i<10000;++i)net::telemetry::Observe(float(i%80),1,i%100==0?1:0);done=true;});
    size_t reads=0;const auto begin=std::chrono::steady_clock::now();
    while(!done){const auto value=net::telemetry::Read();Check(value.sample_count<=120&&std::isfinite(value.smoothed_rtt_ms),"atomic telemetry snapshot");++reads;}
    writer.join();const auto value=net::telemetry::Read();Check(value.recovered_frames_count-before.recovered_frames_count==10000&&value.unrecovered_drops_count-before.unrecovered_drops_count==100,"telemetry counters");
    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();std::printf("Telemetry: concurrent read/write passed; %zu snapshots in %.3f ms (synthetic benchmark)\n",reads,ms);
}
void Preview(const char* path){
    net::telemetry::Snapshot s;s.state=net::telemetry::State::Synced;s.rtt_valid=s.redundancy_enabled=true;
    strcpy_s(s.adapter_name,"Radmin VPN");strcpy_s(s.local_ip,"26.49.12.142");strcpy_s(s.peer_endpoint,"26.102.63.39:3074");s.smoothed_rtt_ms=38.5f;s.jitter_ms=2.1f;s.recovered_frames_count=12;s.unrecovered_drops_count=1;s.sample_count=120;
    for(unsigned i=0;i<120;++i){s.rtt_graph_ring[i]=30+8*std::sin(float(i)*0.3f);s.jitter_graph_ring[i]=2+std::abs(std::sin(float(i)*0.2f))*3;s.graph_event[i]=i==75?2:i>=40&&i<=45?1:0;}
    net::hud::Surface surface;Check(surface.Render(s),"HUD render");
    BITMAPFILEHEADER f{};f.bfType=0x4D42;f.bfOffBits=sizeof(f)+sizeof(BITMAPINFOHEADER);f.bfSize=f.bfOffBits+net::hud::Width*net::hud::Height*4;
    BITMAPINFOHEADER b{};b.biSize=sizeof(b);b.biWidth=net::hud::Width;b.biHeight=-net::hud::Height;b.biPlanes=1;b.biBitCount=32;
    std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(&f),sizeof(f));file.write(reinterpret_cast<const char*>(&b),sizeof(b));file.write(static_cast<const char*>(surface.Pixels()),net::hud::Width*net::hud::Height*4);Check(bool(file),"HUD preview save");
}
}
int main(int argc,char** argv){try{Codec();Telemetry();if(argc==2)Preview(argv[1]);return 0;}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
