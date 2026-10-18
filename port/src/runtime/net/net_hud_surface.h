#pragma once
#include <windows.h>
#include "net_telemetry.h"
#include <algorithm>
#include <cstdio>
namespace net::hud {
constexpr int Width=420,Height=86;
// Cached GDI text rasterization into a CPU bitmap; uploaded as one native D3D11 quad.
// No topmost window, GPU readback, per-frame font creation, or network locking.
class Surface {
    HDC dc_=nullptr;HBITMAP bitmap_=nullptr;HGDIOBJ oldBitmap_=nullptr,oldFont_=nullptr;
    HFONT font_=nullptr;HBRUSH background_=nullptr,graphBackground_=nullptr,accent_=nullptr;
    HPEN pens_[4]{};void* pixels_=nullptr;
public:
    Surface(){
        dc_=CreateCompatibleDC(nullptr);BITMAPINFO b{};b.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);b.bmiHeader.biWidth=Width;b.bmiHeader.biHeight=-Height;b.bmiHeader.biPlanes=1;b.bmiHeader.biBitCount=32;b.bmiHeader.biCompression=BI_RGB;
        bitmap_=CreateDIBSection(dc_,&b,DIB_RGB_COLORS,&pixels_,nullptr,0);if(!dc_||!bitmap_)return;
        oldBitmap_=SelectObject(dc_,bitmap_);font_=CreateFontA(14,0,0,0,FW_MEDIUM,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,FIXED_PITCH,"Consolas");oldFont_=SelectObject(dc_,font_);
        background_=CreateSolidBrush(RGB(12,17,25));graphBackground_=CreateSolidBrush(RGB(22,30,40));accent_=CreateSolidBrush(RGB(32,224,160));
        pens_[0]=CreatePen(PS_SOLID,2,RGB(32,224,160));pens_[1]=CreatePen(PS_SOLID,2,RGB(240,176,32));pens_[2]=CreatePen(PS_SOLID,2,RGB(240,64,64));pens_[3]=CreatePen(PS_SOLID,1,RGB(112,160,190));
    }
    ~Surface(){if(dc_){if(oldFont_)SelectObject(dc_,oldFont_);if(oldBitmap_)SelectObject(dc_,oldBitmap_);}if(font_)DeleteObject(font_);if(bitmap_)DeleteObject(bitmap_);if(dc_)DeleteDC(dc_);if(background_)DeleteObject(background_);if(graphBackground_)DeleteObject(graphBackground_);if(accent_)DeleteObject(accent_);for(auto p:pens_)if(p)DeleteObject(p);}
    Surface(const Surface&)=delete;Surface& operator=(const Surface&)=delete;
    const void* Pixels()const{return pixels_;}
    bool Render(const telemetry::Snapshot& s){
        if(!pixels_)return false;RECT area{0,0,Width,Height};FillRect(dc_,&area,background_);RECT bar{0,0,3,Height};FillRect(dc_,&bar,accent_);SetBkMode(dc_,TRANSPARENT);
        auto text=[&](int y,const char* value,COLORREF color=RGB(220,234,243)){SetTextColor(dc_,color);RECT line{12,y,Width-12,y+18};DrawTextA(dc_,value,-1,&line,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);};
        char line[256]{};std::snprintf(line,sizeof(line),"NETPLAY %s %s",s.adapter_name[0]?s.adapter_name:"LAN",s.local_ip[0]?s.local_ip:"N/A");text(5,line);
        const char* state=s.state==telemetry::State::Synced?"SYNCED":s.state==telemetry::State::Connected?"CONNECTED":s.state==telemetry::State::Searching?"SEARCHING":"IDLE";
        if(s.rtt_valid)std::snprintf(line,sizeof(line),"PING %.0f ms  JIT +/-%.0f ms  REC %llu  LOST %llu  %s",s.smoothed_rtt_ms,s.jitter_ms,(unsigned long long)s.recovered_frames_count,(unsigned long long)s.unrecovered_drops_count,state);else std::snprintf(line,sizeof(line),"PING --  JIT --  REC %llu  LOST %llu  %s",(unsigned long long)s.recovered_frames_count,(unsigned long long)s.unrecovered_drops_count,state);text(29,line);
        if(s.simulation_sync_valid){std::snprintf(line,sizeof(line),"RIFT %+.2f  PACE %+d us",s.frame_rift_delta,s.pacing_adjust_us);text(52,line);}
        const int x0=12,y0=59,w=Width-24,h=22;RECT graph{x0,y0,x0+w,y0+h};FillRect(dc_,&graph,graphBackground_);
        float scale=60;for(uint32_t i=0;i<s.sample_count;++i)scale=(std::max)(scale,s.rtt_graph_ring[i]+s.jitter_graph_ring[i]);scale*=1.15f;
        auto y=[&](float v){return y0+h-2-int(std::clamp(v/scale,0.0f,1.0f)*float(h-4));};
        const auto previous=SelectObject(dc_,pens_[0]);
        for(uint32_t i=1;i<s.sample_count;++i){const int a=x0+int((i-1)*(w-1)/119),b=x0+int(i*(w-1)/119);const int color=s.graph_event[i]==2?2:s.graph_event[i]==1||s.rtt_graph_ring[i]>=60?1:0;
            SelectObject(dc_,pens_[color]);MoveToEx(dc_,a,y(s.rtt_graph_ring[i-1]),nullptr);LineTo(dc_,b,y(s.rtt_graph_ring[i]));
            SelectObject(dc_,pens_[3]);MoveToEx(dc_,a,y(s.jitter_graph_ring[i-1]),nullptr);LineTo(dc_,b,y(s.jitter_graph_ring[i]));
        }
        SelectObject(dc_,previous);GdiFlush();return true;
    }
};
}
