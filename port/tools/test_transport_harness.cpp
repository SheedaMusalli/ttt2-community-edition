#include "../src/runtime/kernel/net_transport.h"
#include <enet/enet.h>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

namespace {
constexpr enet_uint32 kMagic = 0x32545454u; // TTT2, little endian
constexpr enet_uint32 kAck = 0x4B434154u;   // TACK
constexpr enet_uint8 kInput = static_cast<enet_uint8>(net::Channel::CHANNEL_UNRELIABLE_SEQUENCED);
constexpr enet_uint8 kControl = static_cast<enet_uint8>(net::Channel::CHANNEL_RELIABLE_ORDERED);
constexpr char kToken[] = "MATCH_TOKEN_VALID_123";
constexpr char kWrong[] = "TOKEN_INVALID_999";
struct WireHandshake { enet_uint32 magic, version, player, tokenSize; char token[64]; };
struct WireAck { enet_uint32 magic, accepted; };
static_assert(sizeof(WireHandshake) == 80);
void put32(void* p, enet_uint32 v) { std::memcpy(p, &v, sizeof(v)); }

ENetPacket* handshake(const char* token, size_t size, enet_uint32 player)
{
    WireHandshake p{}; p.magic=kMagic; p.version=1; p.player=player;
    if (size > sizeof(p.token)) return nullptr;
    p.tokenSize=static_cast<enet_uint32>(size); std::memcpy(p.token,token,size);
    return enet_packet_create(&p,sizeof(p),ENET_PACKET_FLAG_RELIABLE);
}

bool run()
{
    if (enet_initialize()!=0) return false;
    ENetAddress bind{}; bind.host=ENET_HOST_ANY; bind.port=28991;
    ENetHost* server=enet_host_create(&bind,4,2,0,0);
    ENetHost* clients=enet_host_create(nullptr,2,2,0,0);
    if(!server || !clients) return false;
    ENetAddress dest{}; enet_address_set_host(&dest,"127.0.0.1"); dest.port=28991;
    ENetPeer* good=enet_host_connect(clients,&dest,2,0);
    ENetPeer* rogue=enet_host_connect(clients,&dest,2,0);
    if(!good || !rogue) return false;
    bool serverAuthed=false, clientAck=false, rogueRejected=false;
    unsigned controlCount=0, lastControl=0, inputCount=0, inputAfterDrop=0;
    auto pump=[&](enet_uint32 timeout) {
        ENetEvent e{};
        if(enet_host_service(server,&e,timeout)>0) {
            if(e.type==ENET_EVENT_TYPE_RECEIVE) {
                if(e.peer->data==nullptr) {
                    bool ok=false;
                    if(e.channelID==kControl && e.packet->dataLength==sizeof(WireHandshake)) {
                        WireHandshake p{}; std::memcpy(&p,e.packet->data,sizeof(p));
                        const std::string expectedPlayer="42", actualPlayer=std::to_string(p.player);
                        net::Handshake got{p.token,p.tokenSize,actualPlayer.data(),actualPlayer.size()};
                        net::Handshake expected{kToken,sizeof(kToken)-1,expectedPlayer.data(),expectedPlayer.size()};
                        ok=p.magic==kMagic && p.version==1 && p.tokenSize<=sizeof(p.token) && net::ValidateHandshake(got,expected);
                    }
                    if(!ok) { rogueRejected=true; enet_peer_disconnect_now(e.peer,2); }
                    else { e.peer->data=reinterpret_cast<void*>(uintptr_t(1)); serverAuthed=true;
                        WireAck ack{kAck,1}; enet_peer_send(e.peer,kControl,enet_packet_create(&ack,sizeof(ack),ENET_PACKET_FLAG_RELIABLE)); }
                } else if(e.peer->data==reinterpret_cast<void*>(uintptr_t(1))) {
                    if(e.channelID==kControl && e.packet->dataLength==4) { enet_uint32 n; std::memcpy(&n,e.packet->data,4); assert(n==lastControl++); ++controlCount; }
                    if(e.channelID==kInput && e.packet->dataLength==4) { enet_uint32 n; std::memcpy(&n,e.packet->data,4); ++inputCount; if(n>=20) ++inputAfterDrop; }
                }
                enet_packet_destroy(e.packet);
            } else if(e.type==ENET_EVENT_TYPE_DISCONNECT && e.peer==rogue) rogueRejected=true;
        }
        if(enet_host_service(clients,&e,0)>0) {
            if(e.type==ENET_EVENT_TYPE_RECEIVE && e.peer==good && e.channelID==kControl && e.packet->dataLength==sizeof(WireAck)) {
                WireAck ack{}; std::memcpy(&ack,e.packet->data,sizeof(ack)); clientAck=ack.magic==kAck && ack.accepted==1;
                if(clientAck) good->data=reinterpret_cast<void*>(uintptr_t(1));
            }
            enet_packet_destroy(e.packet);
        }
    };
    for(int i=0;i<2000 && (!clientAck || !rogueRejected);++i) {
        if(good->state==ENET_PEER_STATE_CONNECTED && !clientAck && good->channels[1].outgoingReliableSequenceNumber==0)
            enet_peer_send(good,kControl,handshake(kToken,sizeof(kToken)-1,42));
        if(rogue->state==ENET_PEER_STATE_CONNECTED && !rogueRejected && rogue->channels[1].outgoingReliableSequenceNumber==0)
            enet_peer_send(rogue,kControl,handshake(kWrong,sizeof(kWrong)-1,99));
        pump(1);
    }
    if(!(clientAck && rogueRejected && serverAuthed && good->data)) {
        std::cerr << "handshake state ack="<<clientAck<<" rogue-rejected="<<rogueRejected
                  <<" server-auth="<<serverAuthed<<" client-auth="<<(good->data!=nullptr)<<"\n";
        return false;
    }
    enet_peer_throttle_configure(good,500,2,2);
    for(enet_uint32 i=0;i<100;++i) {
        // Emulate a lossy input source by dropping every other old frame before send.
        if(i%2==0) enet_peer_send(good,kInput,enet_packet_create(&i,sizeof(i),ENET_PACKET_FLAG_UNSEQUENCED));
        if(i<20) enet_peer_send(good,kControl,enet_packet_create(&i,sizeof(i),ENET_PACKET_FLAG_RELIABLE));
    }
    enet_host_flush(clients);
    for(int i=0;i<3000 && controlCount<20;++i) pump(1);
    for(int i=0;i<100;++i) pump(1);
    const bool ok=controlCount==20 && lastControl==20 && inputCount>0 && inputCount<100 && inputAfterDrop>0;
    if(!ok) std::cerr<<"channels reliable="<<controlCount<<" next="<<lastControl<<" input="<<inputCount<<" fresh="<<inputAfterDrop<<"\n";
    enet_host_destroy(clients); enet_host_destroy(server); enet_deinitialize();
    return ok;
}
}

int main() {
    const bool ok=run();
    if(!ok) { std::cerr<<"ENet transport harness failed\n"; return 1; }
    std::cout<<"ENet authentication, rogue rejection, unreliable input and reliable ordering passed\n";
    return 0;
}
