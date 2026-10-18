#pragma once
#include "net_adapter.h"
#include <cstddef>
#include <cstdint>
#include <vector>
namespace net::lan {
using Receiver=void(*)(uint64_t peer,const uint8_t* bytes,size_t size);
bool Start(const Adapter& adapter,uint64_t peer,uint16_t gamePort);
void SetDatagramReceiver(Receiver receiver);
bool Broadcast(const uint8_t* data,size_t size);
void Publish(uint64_t id,const std::vector<uint8_t>& data);
void Remove(uint64_t id);
std::vector<std::vector<uint8_t>> List(unsigned waitMs=350);
void Query();
}
