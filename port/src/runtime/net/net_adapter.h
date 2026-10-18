#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace net {
struct Adapter { std::string name,ip,broadcast; uint32_t ip_host=0, mask_host=0; int priority=99; };
std::vector<Adapter> DetectAdapters(const char* overrideIp=nullptr);
bool SelectAdapter(Adapter& out, const char* overrideIp=nullptr);
bool VirtualLanEnabled();
bool RedundancyEnabled();
bool ValidPeerTargets(const char* targets);
}
