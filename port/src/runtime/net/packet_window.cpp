#include "packet_window.h"
#include <algorithm>
#include <cstring>
namespace net {
namespace {
void Put16(std::vector<uint8_t>& b,uint16_t v){b.push_back(uint8_t(v>>8));b.push_back(uint8_t(v));}
void Put32(std::vector<uint8_t>& b,uint32_t v){Put16(b,uint16_t(v>>16));Put16(b,uint16_t(v));}
uint16_t Get16(const uint8_t* b){return uint16_t((uint16_t(b[0])<<8)|b[1]);}
uint32_t Get32(const uint8_t* b){return (uint32_t(Get16(b))<<16)|Get16(b+2);}
}
bool PacketWindow::HasMagic(const uint8_t* b,size_t n){return b&&n>=4&&std::memcmp(b,"TRBK",4)==0;}
std::vector<uint8_t> PacketWindow::Encode(const uint8_t* data,size_t size,uint64_t now){
    if(!data||size>kMaxRecoveryPayload)return {};
    sent_.emplace_back(data,data+size);if(sent_.size()>kRedundancyWindow)sent_.pop_front();
    timestamps_.push_back(now);while(timestamps_.size()>128)timestamps_.pop_front();
    std::vector<uint8_t> out;out.reserve(kRecoveryHeader+sent_.size()*(size+2));
    out.insert(out.end(),{'T','R','B','K',1,uint8_t(sent_.size())});Put16(out,haveEcho_?1:0);
    Put32(out,epoch_);Put32(out,nextSend_++);Put32(out,haveRemote_?nextReceive_-1:0);
    Put16(out,uint16_t(now));Put16(out,remoteTick_);
    // No valid simulation boundary is available. Do not invent frame advantage from packet counts.
    Put16(out,0);Put16(out,0);
    for(const auto& p:sent_){Put16(out,uint16_t(p.size()));out.insert(out.end(),p.begin(),p.end());}
    return out;
}
RecoveryResult PacketWindow::Decode(const uint8_t* data,size_t size,uint64_t now){
    RecoveryResult out;
    if(size<kRecoveryHeader||!HasMagic(data,size)||data[4]!=1||data[5]<1||data[5]>4||Get16(data+6)>1||Get16(data+24)||Get16(data+26))return out;
    const uint32_t epoch=Get32(data+8),latest=Get32(data+12);const size_t count=data[5];
    if(!epoch)return out;
    size_t at=kRecoveryHeader;std::array<size_t,4> offsets{},lengths{};
    for(size_t i=0;i<count;++i){if(at+2>size)return out;const auto length=Get16(data+at);at+=2;if(length>512||length>size-at)return out;offsets[i]=at;lengths[i]=length;at+=length;}
    if(at!=size)return out;
    // Validate before changing epoch or delivery state: malformed restarts must not retire a live stream.
    const bool restarting=!haveRemote_||epoch!=remoteEpoch_;
    const uint32_t expected=restarting?0:nextReceive_;
    const uint32_t earliest=latest-uint32_t(count-1);
    if(int32_t(latest-expected)>=0&&int32_t(earliest-expected)>0&&earliest-expected>1'000'000)return out;
    if(haveRemote_&&epoch!=remoteEpoch_){if(std::find(retired_.begin(),retired_.end(),epoch)!=retired_.end())return out;retired_[retiredIndex_++%4]=remoteEpoch_;haveRemote_=false;}
    if(!haveRemote_){remoteEpoch_=epoch;nextReceive_=0;haveRemote_=true;}
    out.valid=true;
    // Preserve ordering across duplicate/reordered bundles. An older bundle never rewinds delivery.
    if(int32_t(latest-nextReceive_)<0)return out;
    uint32_t first=latest-uint32_t(count-1);
    if(int32_t(first-nextReceive_)>0){const uint32_t gap=first-nextReceive_;if(gap>1'000'000){out.valid=false;return out;}out.lost=gap;nextReceive_=first;}
    for(size_t i=0;i<count;++i){const uint32_t seq=first+uint32_t(i);if(int32_t(seq-nextReceive_)<0)continue;
        if(seq!=nextReceive_)continue;
        out.payloads.emplace_back(data+offsets[i],data+offsets[i]+lengths[i]);++nextReceive_;
        if(seq!=latest)++out.recovered;
    }
    remoteTick_=Get16(data+20);haveEcho_=true;
    if(Get16(data+6)&1){const uint16_t echo=Get16(data+22);
        for(auto it=timestamps_.rbegin();it!=timestamps_.rend();++it){
            if(uint16_t(*it)==echo&&*it>lastEchoSample_&&now>=*it&&now-*it<=10000){out.rtt_valid=true;out.rtt_ms=float(now-*it);lastEchoSample_=*it;break;}
        }
    }
    return out;
}
}
