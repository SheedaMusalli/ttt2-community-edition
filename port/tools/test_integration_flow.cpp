#include "http_client.h"
#include "../src/runtime/kernel/net_transport.h"
#include "../third_party/enet/include/enet/enet.h"
#include <cstring>
#include <iostream>
#include <string>
namespace {
constexpr enet_uint32 MAGIC=0x32545454u, ACK=0x4B434154u;
struct Hello { enet_uint32 magic,version,player,tokenLen; char token[64]; };
struct Ack { enet_uint32 magic,accepted; };
bool post(const std::wstring& u,const std::wstring& t,const std::string& b,std::string& r){unsigned s=0;std::wstring e;if(!test_http::PostJson(u,t,b,r,s,e)){std::wcerr<<L"HTTP error "<<s<<L" "<<e<<L"\n";return false;}return true;}
bool enetFlow(const std::string& token){if(token.empty()||token.size()>64||enet_initialize())return false;ENetAddress a{};a.host=ENET_HOST_ANY;a.port=28992;auto* sh=enet_host_create(&a,1,2,0,0);auto* ch=enet_host_create(nullptr,1,2,0,0);if(!sh||!ch)return false;ENetAddress to{};enet_address_set_host(&to,"127.0.0.1");to.port=28992;auto* peer=enet_host_connect(ch,&to,2,0);if(!peer)return false;bool sent=false,auth=false,ack=false;ENetEvent e{};
 for(int i=0;i<3000&&!ack;++i){if(peer->state==ENET_PEER_STATE_CONNECTED&&!sent){Hello h{};h.magic=MAGIC;h.version=1;h.player=2;h.tokenLen=(enet_uint32)token.size();memcpy(h.token,token.data(),token.size());if(enet_peer_send(peer,1,enet_packet_create(&h,sizeof h,ENET_PACKET_FLAG_RELIABLE)))return false;sent=true;}
 if(enet_host_service(sh,&e,1)>0&&e.type==ENET_EVENT_TYPE_RECEIVE){bool ok=false;if(e.channelID==1&&e.packet->dataLength==sizeof(Hello)){Hello h{};memcpy(&h,e.packet->data,sizeof h);std::string pid=std::to_string(h.player),expected="2";net::Handshake got{h.token,h.tokenLen,pid.data(),pid.size()},want{token.data(),token.size(),expected.data(),expected.size()};ok=h.magic==MAGIC&&h.version==1&&h.tokenLen<=64&&net::ValidateHandshake(got,want);}enet_packet_destroy(e.packet);if(!ok)return false;auth=true;Ack x{ACK,1};enet_peer_send(e.peer,1,enet_packet_create(&x,sizeof x,ENET_PACKET_FLAG_RELIABLE));}
 if(enet_host_service(ch,&e,0)>0&&e.type==ENET_EVENT_TYPE_RECEIVE){Ack x{};if(e.channelID==1&&e.packet->dataLength==sizeof x){memcpy(&x,e.packet->data,sizeof x);ack=x.magic==ACK&&x.accepted==1;}enet_packet_destroy(e.packet);}}
 enet_host_destroy(ch);enet_host_destroy(sh);enet_deinitialize();return auth&&ack;}
}
int main(int argc,char**argv){if(argc!=4){std::cerr<<"usage: test_integration_flow <base-url> <alice-token> <bob-token>\n";return 2;}std::wstring base(argv[1],argv[1]+strlen(argv[1])),aToken(argv[2],argv[2]+strlen(argv[2])),bToken(argv[3],argv[3]+strlen(argv[3]));std::string a,b,id,ta,tb,r;
 if(!post(base+L"/v1/queue",aToken,"{\"build\":\"harness-v1\",\"region\":\"local\"}",a)||!post(base+L"/v1/queue",bToken,"{\"build\":\"harness-v1\",\"region\":\"local\"}",b)){std::cerr<<"queue failed\n";return 1;}
 if(!test_http::JsonString(b,"id",id)||!test_http::JsonString(b,"match_token",tb)){std::cerr<<"reservation response missing ID/token: "<<b<<"\n";return 1;}
 bool reserved=false; for(int i=0;i<20&&!reserved;++i){if(!post(base+L"/v1/status",aToken,"{}",a))return 1;std::string found;if(test_http::JsonString(a,"id",found)&&found==id)reserved=test_http::JsonString(a,"match_token",ta);if(!reserved)Sleep(50);}
 if(!reserved||ta!=tb){std::cerr<<"shared reservation token mismatch A="<<a<<" B="<<b<<"\n";return 1;}
 if(!post(base+L"/v1/ready",aToken,"{\"match_id\":\""+id+"\"}",r)||!post(base+L"/v1/ready",bToken,"{\"match_id\":\""+id+"\"}",r)){std::cerr<<"ready failed\n";return 1;}if(!enetFlow(ta)){std::cerr<<"ENet handoff failed\n";return 1;}
 const std::string report="{\"match_id\":\""+id+"\",\"winner\":\"alice\"}";if(!post(base+L"/v1/report",aToken,report,r)||!post(base+L"/v1/report",bToken,report,r)){std::cerr<<"report failed\n";return 1;}std::string state;if(!test_http::JsonString(r,"state",state)||state!="completed"){std::cerr<<"result not completed: "<<r<<"\n";return 1;}std::cout<<"HTTP matchmaking -> shared token -> ENet authentication -> ranked result passed\n";return 0;}
