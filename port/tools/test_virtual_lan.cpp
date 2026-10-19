#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "net_transport.h"
#include "steam_net.h"
#include "matchmaking.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
namespace {
std::atomic<bool> ack{false},broadcastAck{false},removed{false},finished{false};
bool host=false;
constexpr uint64_t id=0x1122334455667788ull;
void Receive(uint64_t peer,net::Channel,const uint8_t* data,size_t size){
    const std::string message(reinterpret_cast<const char*>(data),size);
    if(host){if(message=="hello")net::Send(peer,net::Channel::Reliable,"ack",3);
        else if(message=="beacon")net::Send(peer,net::Channel::Reliable,"broadcast-ack",13);
        else if(message=="remove"){mm::Remove(id);net::Send(peer,net::Channel::Reliable,"removed",7);}
        else if(message=="done"){net::Send(peer,net::Channel::Reliable,"finished",8);finished=true;}}
    else{if(message=="ack")ack=true;else if(message=="broadcast-ack")broadcastAck=true;else if(message=="removed")removed=true;else if(message=="finished")finished=true;}
}
template<class F>bool Wait(F predicate){const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(8);while(std::chrono::steady_clock::now()<until){if(predicate())return true;std::this_thread::sleep_for(std::chrono::milliseconds(10));}return predicate();}
}
namespace steamnet {bool Start(Receiver){return false;}uint64_t LocalId(){return 0;}bool Send(uint64_t,bool,const void*,size_t){return false;}void PublishLobby(uint64_t,const std::vector<uint8_t>&){}void RemoveLobby(uint64_t){}std::vector<std::vector<uint8_t>> ListLobbies(int,int){return {};}bool TakeJoinRequest(std::vector<uint8_t>&){return false;}bool OpenInviteDialog(uint64_t){return false;}}
int main(int argc,char** argv){
    host=argc==2&&std::strcmp(argv[1],"host")==0;if(!net::Start(Receive))return 1;
    if(host){mm::SessionRecord r;r.id=id;r.hostPeer=net::LocalPeer();r.hostXuid=111;r.maxPublic=2;r.filledPublic=1;mm::Publish(r);std::puts("READY");std::fflush(stdout);if(!Wait([]{return finished.load();}))return 2;std::this_thread::sleep_for(std::chrono::milliseconds(100));ExitProcess(0);}
    uint64_t peer=0;if(!Wait([&]{for(const auto& r:mm::List(4))if(r.id==id&&r.hostPeer!=net::LocalPeer()){peer=r.hostPeer;return r.maxPublic==2&&r.filledPublic==1;}return false;}))return 3;
    if(!net::Send(peer,net::Channel::Reliable,"hello",5)||!Wait([]{return ack.load();}))return 4;
    if(!net::BroadcastDatagram("beacon",6)||!Wait([]{return broadcastAck.load();}))return 5;
    if(!net::Send(peer,net::Channel::Reliable,"remove",6)||!Wait([]{return removed.load();}))return 6;
    if(!Wait([]{mm::SessionRecord r;return !mm::Find(id,r);}))return 7;
    if(!net::Send(peer,net::Channel::Reliable,"done",4)||!Wait([]{return finished.load();}))return 8;
    std::puts("LAN session discovery -> peer registration -> reliable handshake -> UDP broadcast fan-out -> lobby withdrawal passed");std::fflush(stdout);ExitProcess(0);
}
