#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include "virtual_lan.h"
#include "net_telemetry.h"
#include "../kernel/net_transport.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
namespace net::lan {
namespace {
constexpr uint16_t kPort=29876;constexpr size_t kHeader=20;
SOCKET sock=INVALID_SOCKET;std::atomic<bool> started{false};std::atomic<Receiver> receiver{nullptr};
uint64_t self=0;uint16_t gamePort=0,discoveryPort=kPort;std::vector<sockaddr_in> targets;
std::mutex mutex;std::condition_variable changed;
struct Record {std::vector<uint8_t> data;std::chrono::steady_clock::time_point seen;};
std::map<uint64_t,std::vector<uint8_t>> owned;std::map<std::pair<uint64_t,uint64_t>,Record> remote;
void Put64(uint8_t* out,uint64_t v){for(int i=7;i>=0;--i){out[i]=uint8_t(v);v>>=8;}}
uint64_t Get64(const uint8_t* p){uint64_t v=0;for(int i=0;i<8;++i)v=(v<<8)|p[i];return v;}
uint16_t ReadPort(const char* text,uint16_t fallback){if(!text||!*text)return fallback;char* e=nullptr;const auto v=std::strtoul(text,&e,10);return e&&!*e&&v&&v<=65535?uint16_t(v):0;}
std::vector<uint8_t> Wire(uint8_t type,const uint8_t* data,size_t size){
    if(size>60000)return {};std::vector<uint8_t> out(kHeader+size);std::memcpy(out.data(),"TLAN",4);out[4]=1;out[5]=type;
    Put64(out.data()+8,self);out[16]=uint8_t(gamePort>>8);out[17]=uint8_t(gamePort);out[18]=uint8_t(size>>8);out[19]=uint8_t(size);
    if(size)std::memcpy(out.data()+kHeader,data,size);return out;
}
bool Send(uint8_t type,const uint8_t* data,size_t size,const sockaddr_in* destination=nullptr){
    const auto wire=Wire(type,data,size);if(wire.empty())return false;bool ok=false;
    auto send=[&](const sockaddr_in& to){if(sendto(sock,reinterpret_cast<const char*>(wire.data()),int(wire.size()),0,reinterpret_cast<const sockaddr*>(&to),sizeof(to))==int(wire.size()))ok=true;};
    if(destination)send(*destination);else for(const auto& target:targets)send(target);return ok;
}
void Run(){
    uint64_t nextBeat=0;std::vector<uint8_t> buffer(65507);
    for(;;){
        if(telemetry::NowMs()>=nextBeat){Query();std::vector<std::vector<uint8_t>> records;{std::lock_guard l(mutex);for(const auto& v:owned)records.push_back(v.second);}for(const auto& v:records)Send(2,v.data(),v.size());nextBeat=telemetry::NowMs()+1000;}
        fd_set reads;FD_ZERO(&reads);FD_SET(sock,&reads);timeval timeout{0,30000};if(select(0,&reads,nullptr,nullptr,&timeout)<=0)continue;
        for(unsigned n=0;n<32;++n){sockaddr_in source{};int sourceSize=sizeof(source);const int size=recvfrom(sock,reinterpret_cast<char*>(buffer.data()),int(buffer.size()),0,reinterpret_cast<sockaddr*>(&source),&sourceSize);if(size<0)break;
            if(size<int(kHeader)||std::memcmp(buffer.data(),"TLAN",4)||buffer[4]!=1||buffer[6]||buffer[7])continue;
            const uint64_t peer=Get64(buffer.data()+8);const auto port=uint16_t((uint16_t(buffer[16])<<8)|buffer[17]);const auto length=size_t((uint16_t(buffer[18])<<8)|buffer[19]);
            if(!peer||peer==self||!port||length!=size-kHeader)continue;
            char ip[16]{};if(!InetNtopA(AF_INET,&source.sin_addr,ip,sizeof(ip))||!net::RegisterPeer(peer,ip,port))continue;
            const auto* data=buffer.data()+kHeader;
            if(buffer[5]==1&&length==0){std::vector<std::vector<uint8_t>> list;{std::lock_guard l(mutex);for(const auto& v:owned)list.push_back(v.second);}for(const auto& v:list)Send(2,v.data(),v.size(),&source);}
            else if(buffer[5]==2&&length>=20&&std::memcmp(data,"SES2",4)==0){
                uint64_t sessionId=0;for(int i=7;i>=0;--i)sessionId=(sessionId<<8)|data[4+i];
                uint64_t recordPeer=0;for(int i=7;i>=0;--i)recordPeer=(recordPeer<<8)|data[12+i];if(recordPeer!=peer)continue;std::lock_guard l(mutex);if(remote.size()<128||remote.count({peer,sessionId}))remote[{peer,sessionId}]={std::vector<uint8_t>(data,data+length),std::chrono::steady_clock::now()};changed.notify_all();}
            else if(buffer[5]==3){if(auto fn=receiver.load())fn(peer,data,length);}
            else if(buffer[5]==4&&length==8){std::lock_guard l(mutex);remote.erase({peer,Get64(data)});changed.notify_all();}
        }
    }
}
}
bool Start(const Adapter& adapter,uint64_t peer,uint16_t port){
    if(!ValidPeerTargets(std::getenv("TTT2_PEER_IP"))){std::fprintf(stderr,"[lan] invalid TTT2_PEER_IP; expected up to 64 comma-separated IPv4[:discovery-port] targets\n");return false;}
    if(started.load())return true;self=peer;gamePort=port;discoveryPort=ReadPort(std::getenv("TTT2_DISCOVERY_PORT"),kPort);if(!discoveryPort)return false;
    sock=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(sock==INVALID_SOCKET)return false;
    BOOL yes=TRUE;int bytes=262144;setsockopt(sock,SOL_SOCKET,SO_BROADCAST,reinterpret_cast<const char*>(&yes),sizeof(yes));
    setsockopt(sock,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&yes),sizeof(yes));
    setsockopt(sock,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&bytes),sizeof(bytes));setsockopt(sock,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<const char*>(&bytes),sizeof(bytes));
    sockaddr_in local{};local.sin_family=AF_INET;local.sin_port=htons(discoveryPort);local.sin_addr.s_addr=htonl(adapter.ip_host);
    int bound=bind(sock,reinterpret_cast<const sockaddr*>(&local),sizeof(local));
    if(bound && !std::getenv("TTT2_DISCOVERY_PORT")){for(unsigned p=kPort+1;p<=kPort+99&&bound;++p){local.sin_port=htons(uint16_t(p));bound=bind(sock,reinterpret_cast<const sockaddr*>(&local),sizeof(local));if(!bound)discoveryPort=uint16_t(p);}}
    if(bound){std::fprintf(stderr,"[lan] bind %s:%u failed: %d\n",adapter.ip.c_str(),discoveryPort,WSAGetLastError());closesocket(sock);sock=INVALID_SOCKET;return false;}
    u_long nonblocking=1;ioctlsocket(sock,FIONBIO,&nonblocking);
    auto add=[&](std::string text){uint16_t p=kPort;const auto colon=text.find(':');if(colon!=std::string::npos){p=ReadPort(text.c_str()+colon+1,kPort);text.resize(colon);}sockaddr_in to{};to.sin_family=AF_INET;to.sin_port=htons(p);if(!p||InetPtonA(AF_INET,text.c_str(),&to.sin_addr)!=1)return;
        for(const auto& t:targets)if(t.sin_addr.s_addr==to.sin_addr.s_addr&&t.sin_port==to.sin_port)return;targets.push_back(to);};
    if(const char* explicitPeers=std::getenv("TTT2_PEER_IP")){std::string list=explicitPeers;size_t begin=0;while(begin<list.size()){const auto end=list.find(',',begin);auto v=list.substr(begin,end==std::string::npos?end:end-begin);v.erase(std::remove_if(v.begin(),v.end(),[](char c){return c==' '||c=='\t';}),v.end());add(v);if(end==std::string::npos)break;begin=end+1;}}
    add(adapter.broadcast);add("255.255.255.255");started=true;
    std::thread(Run).detach();std::fprintf(stderr,"[lan] discovery %s:%u; %zu fan-out targets; broadcasts require provider/firewall support\n",adapter.ip.c_str(),discoveryPort,targets.size());return true;
}
void SetDatagramReceiver(Receiver fn){receiver=fn;}
bool Broadcast(const uint8_t* data,size_t size){return started&&Send(3,data,size);}
void Query(){if(started)Send(1,nullptr,0);}
void Publish(uint64_t id,const std::vector<uint8_t>& data){{std::lock_guard l(mutex);owned[id]=data;}if(started)Send(2,data.data(),data.size());}
void Remove(uint64_t id){{std::lock_guard l(mutex);owned.erase(id);}uint8_t b[8];Put64(b,id);if(started)Send(4,b,8);}
std::vector<std::vector<uint8_t>> List(unsigned waitMs){
    Query();std::unique_lock l(mutex);auto prune=[](){const auto now=std::chrono::steady_clock::now();for(auto it=remote.begin();it!=remote.end();)if(now-it->second.seen>std::chrono::seconds(4))it=remote.erase(it);else ++it;};prune();
    if(remote.empty()&&waitMs)changed.wait_for(l,std::chrono::milliseconds(waitMs),[]{return !remote.empty();});prune();
    std::vector<std::vector<uint8_t>> out;for(const auto& v:remote)out.push_back(v.second.data);return out;
}
}
