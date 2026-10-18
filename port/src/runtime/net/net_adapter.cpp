#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "net_adapter.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cwctype>
namespace net {
namespace {
std::string Ip(uint32_t ip){return std::to_string(ip>>24)+"."+std::to_string((ip>>16)&255)+"."+std::to_string((ip>>8)&255)+"."+std::to_string(ip&255);}
}
bool VirtualLanEnabled(){const char* e=std::getenv("TTT2_NET");return e&&(!std::strcmp(e,"hamachi")||!std::strcmp(e,"lan"));}
bool RedundancyEnabled(){const char* e=std::getenv("TTT2_NET_ROLLBACK");return VirtualLanEnabled()&&(!e||std::strcmp(e,"0"));}
std::vector<Adapter> DetectAdapters(const char* requested){
    ULONG bytes=16384;std::vector<unsigned char> buffer(bytes);
    const ULONG flags=GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc=GetAdaptersAddresses(AF_INET,flags,nullptr,reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),&bytes);
    if(rc==ERROR_BUFFER_OVERFLOW){buffer.resize(bytes);rc=GetAdaptersAddresses(AF_INET,flags,nullptr,reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),&bytes);}
    std::vector<Adapter> result;if(rc!=NO_ERROR)return result;
    for(auto* a=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());a;a=a->Next){
        if(a->OperStatus!=IfOperStatusUp)continue;
        std::wstring label=std::wstring(a->FriendlyName?a->FriendlyName:L"")+L" "+(a->Description?a->Description:L"");
        std::transform(label.begin(),label.end(),label.begin(),[](wchar_t c){return wchar_t(std::towlower(c));});
        auto has=[&](const wchar_t* v){return label.find(v)!=std::wstring::npos;};
        for(auto* u=a->FirstUnicastAddress;u;u=u->Next){
            if(!u->Address.lpSockaddr||u->Address.lpSockaddr->sa_family!=AF_INET)continue;
            const auto host=ntohl(reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr);
            const auto ip=Ip(host);const bool explicitMatch=requested&&*requested&&ip==requested;
            if(!host||((a->IfType==IF_TYPE_SOFTWARE_LOOPBACK||(host>>24)==127)&&!explicitMatch))continue;
            Adapter v;v.ip=ip;v.ip_host=host;v.name="LAN";v.priority=6;
            if(has(L"hamachi")||(host>>24)==25){v.name="Hamachi";v.priority=2;}
            else if(has(L"radmin")||has(L"famatech")||(host>>24)==26){v.name="Radmin VPN";v.priority=3;}
            else if(has(L"tailscale")||(host&0xFFC00000u)==0x64400000u){v.name="Tailscale";v.priority=4;}
            else if(has(L"zerotier")){v.name="ZeroTier";v.priority=5;}
            else if(has(L"wireguard")||has(L"wintun")){v.name="WireGuard";v.priority=5;}
            else if(has(L"openvpn")||has(L"tap-windows")){v.name="OpenVPN";v.priority=5;}
            else if((host>>24)==10||(host&0xFFF00000u)==0xAC100000u)v.priority=5;
            else if((host&0xFFFF0000u)!=0xC0A80000u&&!explicitMatch)continue;
            if(explicitMatch)v.priority=1;
            const unsigned prefix=(std::min)(unsigned(u->OnLinkPrefixLength),32u);
            v.mask_host=prefix?(0xFFFFFFFFu<<(32-prefix)):0;v.broadcast=Ip(host|~v.mask_host);
            result.push_back(v);
        }
    }
    std::stable_sort(result.begin(),result.end(),[](const Adapter& a,const Adapter& b){return a.priority<b.priority;});return result;
}
bool SelectAdapter(Adapter& out,const char* requested){auto list=DetectAdapters(requested);if(list.empty())return false;if(requested&&*requested&&list.front().ip!=requested)return false;out=list.front();return true;}
bool ValidPeerTargets(const char* targets){
    if(!targets||!*targets)return true;
    std::string list=targets;if(list.size()>4096)return false;
    size_t begin=0;unsigned count=0;
    do{
        const auto end=list.find(',',begin);auto value=list.substr(begin,end==std::string::npos?end:end-begin);
        value.erase(std::remove_if(value.begin(),value.end(),[](char c){return c==' '||c=='\t';}),value.end());
        if(value.empty()||++count>64)return false;
        const auto colon=value.find(':');
        if(colon!=std::string::npos){const auto port=value.substr(colon+1);if(port.empty()||port.size()>5||port.find_first_not_of("0123456789")!=std::string::npos)return false;const auto n=std::strtoul(port.c_str(),nullptr,10);if(!n||n>65535)return false;value.resize(colon);}
        IN_ADDR ip{};if(InetPtonA(AF_INET,value.c_str(),&ip)!=1)return false;
        const auto host=ntohl(ip.s_addr);if(!host||host==0xFFFFFFFFu||(host>>28)==14)return false;
        if(end==std::string::npos)break;begin=end+1;
    }while(begin<=list.size());return true;
}
}
