#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef TTT2_VERSION_STR
#define TTT2_VERSION_STR "0.0.0"
#endif
#define TTT2_WIDE2(s) L##s
#define TTT2_WIDE(s) TTT2_WIDE2(s)
#include <windows.h>
#include "../runtime/net/net_adapter.h"
#include <commdlg.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <uxtheme.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr wchar_t kWindowClass[] = L"TTT2CommunityLauncher";
constexpr wchar_t kGameExe[] = L"Tekken Tag Tournament 2.exe";
constexpr wchar_t kVersion[] = L"v" TTT2_WIDE(TTT2_VERSION_STR);
constexpr wchar_t kAppFolder[] = L"TTT2Recomp";
// TTT2 community theme: dark arcade slate, tournament red, and blue tag accents.
constexpr COLORREF kBg = RGB(16, 21, 28);
constexpr COLORREF kSidebar = RGB(19, 26, 35);
constexpr COLORREF kPanel = RGB(27, 36, 47);
constexpr COLORREF kLine = RGB(43, 56, 70);
constexpr COLORREF kRed = RGB(218, 47, 60);
constexpr COLORREF kBlue = RGB(53, 158, 207);
constexpr COLORREF kCream = RGB(233, 240, 247);
constexpr COLORREF kMuted = RGB(145, 163, 181);
constexpr int kFooterHeight = 72;
constexpr int kContentMargin = 34;
constexpr int kMaxContentWidth = 1440;
constexpr std::array<int,7> kInputPollRates{250,500,750,1000,2000,4000,8000};
enum : int { ID_HOME=100, ID_GRAPHICS, ID_PERFORMANCE, ID_GAME, ID_ADVANCED, ID_CONTROLS, ID_ONLINE, ID_VPNLAN,
             ID_SAVE=180, ID_PLAY, ID_MODE=200, ID_RESOLUTION, ID_SCALE,
             ID_ASYNC=210, ID_WARM, ID_FAST, ID_HARDWARE, ID_INPUT_POLL_RATE, ID_DRAW_DIAGNOSTICS,
             ID_SAVE_PATH=220, ID_BROWSE, ID_OPEN_SAVES,
             ID_OPEN_INSTALL=230, ID_OPEN_LOG, ID_VERIFY, ID_SAVE_BACKUP=240, ID_OPEN_CONFIG, ID_SAVE_RESTORE,
             ID_CLOSE_ON_LAUNCH=250, ID_OVERLAY_FIRST=260, ID_KEY_FIRST=300, ID_RANKED_URL=340, ID_RANKED_TOKEN,
             ID_RANKED_BUILD, ID_RANKED_REGION, ID_RANKED_HEALTH, ID_RANKED_PROFILE,
             ID_RANKED_QUEUE, ID_RANKED_STATUS, ID_RANKED_CANCEL, ID_RANKED_RESULT, ID_RANKED_REGISTER,
             ID_KEY2_FIRST=420, ID_PLAYER2_ENABLED=450, ID_CONTROL_PLAYER1=451, ID_CONTROL_PLAYER2=452,
             ID_RANKED_ACCEPT=453, ID_RANKED_DECLINE=454, ID_CHALLENGE_TIMER=455, ID_STATUS_TIMER=456,
             ID_COOLDOWN_TIMER=457, ID_CONSOLE_LOGS=458, ID_OVERLAY_GRAPH=459, ID_OVERLAY_POS=460, ID_NET_POS=461, ID_PAD_ASSIGNMENT=467, ID_NET_MODE=470, ID_NET_LOCAL_IP, ID_NET_PEER_IP, ID_NET_HUD, ID_NET_RECOVERY, ID_NET_REFRESH };
enum Tab { Home, Graphics, Performance, Game, Advanced, Controls, Online, VpnLan };
struct Settings {
    int mode = 0;
    int resolution = 0;
    int scale = 0;
    bool asyncShaders = true;
    bool warmShaders = true;
    bool fastShaders = true;
    int inputPollRateHz = 1000;
    bool drawDiagnostics = false;
    bool consoleLogs = false;
    bool hardware = true;
    bool closeOnLaunch = true;
    std::array<bool,8> overlay{true,true,false,false,false,false,false,false};
    bool overlayEnabled = true;
    bool overlayGraph = false;
    int overlayPosition=0,netPosition=0;
    int overlayToggleKey = VK_F10;
    std::wstring saveRoot;
    std::array<int,24> keys{VK_UP,VK_DOWN,VK_LEFT,VK_RIGHT,'A','S','Z','X','Q','W','E','R','D','F',VK_RETURN,VK_BACK};
    std::array<int,24> keys2{'I','K','J','L','U','O','N','M','Y','H','7','8','5','6',VK_NUMPAD0,VK_DECIMAL};
    bool player2KeyboardEnabled = false;
    int padAssignment = 1;
    int netMode=0; bool netHud=true, netRecovery=true;
    std::wstring netLocalIp,netPeerIps;
};
HWND g_window = nullptr;
Tab g_tab = Home;
Settings g_settings;
HFONT g_bodyFont = nullptr, g_headingFont = nullptr, g_sectionFont = nullptr, g_smallFont = nullptr;
HBRUSH g_darkBrush = nullptr, g_panelBrush = nullptr;
std::vector<HWND> g_page;
struct PageItem { HWND hwnd; RECT bounds; int windowHeight; };
std::vector<PageItem> g_pageItems;
std::array<HWND,8> g_nav{};
HWND g_status = nullptr, g_play = nullptr;
HWND g_challengePanel = nullptr, g_challengeTitle = nullptr, g_challengeOpponent = nullptr;
HWND g_challengeRating = nullptr, g_challengeDisconnect = nullptr, g_challengeTime = nullptr;
HWND g_challengeProgress = nullptr, g_challengeAccept = nullptr, g_challengeDecline = nullptr;
HANDLE g_gameProcess = nullptr;
std::atomic<bool> g_rankedBusy{false};
bool g_registrationValidation = false;
bool g_rankedSearching = false, g_acceptSubmitted = false;
bool g_closeAfterDecline = false;
bool g_capturingOverlayKey = false;
std::wstring g_pendingMatchId;
std::chrono::steady_clock::time_point g_acceptDeadline{};
std::chrono::steady_clock::time_point g_lastQueueRenew{};
std::chrono::steady_clock::time_point g_queueCooldownUntil{};
bool g_queueLeaseRenew = true;
int g_controlPlayer = 0;
int g_capturingKey = -1;
int g_contentX = kContentMargin, g_contentWidth = 1032, g_scrollOffset = 0, g_pageBottom = 0;
int g_viewportTop = 136, g_viewportBottom = 648;
constexpr UINT WM_RANKED_REGISTER_COMPLETE = WM_APP + 7;

struct RankedRegistrationResult { std::wstring response; };

void UpdateContentGeometry(int clientWidth) {
    g_contentWidth = (std::min)(kMaxContentWidth, (std::max)(500, clientWidth - 2*kContentMargin));
    g_contentX = (std::max)(kContentMargin, (clientWidth - g_contentWidth) / 2);
}

void ReflowPage(HWND hwnd) {
    RECT client{}; GetClientRect(hwnd, &client);
    g_viewportBottom = (std::max)(g_viewportTop + 80, int(client.bottom) - kFooterHeight - 10);
    const int maxOffset = (std::max)(0, g_pageBottom - g_viewportBottom);
    g_scrollOffset = std::clamp(g_scrollOffset, 0, maxOffset);
    ShowScrollBar(hwnd, SB_VERT, FALSE);
    for (const auto& item:g_pageItems) {
        RECT r=item.bounds;
        const int height=r.bottom-r.top;
        r.top-=g_scrollOffset; r.bottom=r.top+height;
        // A native dropdown combo's HWND height is its expanded height. The
        // collapsed field still occupies only the logical layout height.
        MoveWindow(item.hwnd,r.left,r.top,r.right-r.left,item.windowHeight,TRUE);
        const bool visible=r.top>=g_viewportTop && r.bottom<=g_viewportBottom;
        ShowWindow(item.hwnd,visible?SW_SHOW:SW_HIDE);
    }
}

LRESULT CALLBACK WheelForwardSubclass(HWND control, UINT msg, WPARAM wp, LPARAM lp,
                                      UINT_PTR subclassId, DWORD_PTR reference) {
    if (msg == WM_MOUSEWHEEL && GetParent(control)) {
        PostMessageW(GetParent(control), WM_MOUSEWHEEL, wp, lp);
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(control, WheelForwardSubclass, subclassId);
    return DefSubclassProc(control, msg, wp, lp);
}

const wchar_t* ControlNames[24]{L"D-pad Up",L"D-pad Down",L"D-pad Left",L"D-pad Right",
    L"A",L"B",L"X",L"Y",L"LB",L"RB",L"LT",L"RT",L"LS click",L"RS click",L"Start",L"Back",
    L"LS Up",L"LS Down",L"LS Left",L"LS Right",L"RS Up",L"RS Down",L"RS Left",L"RS Right"};

std::wstring KeyName(int vk) {
    if(vk>='A'&&vk<='Z')return std::wstring(1,wchar_t(vk));
    if(vk>='0'&&vk<='9')return std::wstring(1,wchar_t(vk));
    if(vk>=VK_NUMPAD0&&vk<=VK_NUMPAD9)return L"NUM "+std::to_wstring(vk-VK_NUMPAD0);
    if(vk>=VK_F1&&vk<=VK_F24)return L"F"+std::to_wstring(vk-VK_F1+1);
    switch(vk){case 0:return L"UNBOUND";case VK_UP:return L"UP";case VK_DOWN:return L"DOWN";case VK_LEFT:return L"LEFT";case VK_RIGHT:return L"RIGHT";case VK_SPACE:return L"SPACE";case VK_RETURN:return L"ENTER";case VK_BACK:return L"BACKSPACE";case VK_TAB:return L"TAB";case VK_LSHIFT:return L"L SHIFT";case VK_RSHIFT:return L"R SHIFT";case VK_LCONTROL:return L"L CTRL";case VK_RCONTROL:return L"R CTRL";case VK_LMENU:return L"L ALT";case VK_RMENU:return L"R ALT";case VK_DECIMAL:return L"NUM .";case VK_MULTIPLY:return L"NUM *";case VK_ADD:return L"NUM +";case VK_SUBTRACT:return L"NUM -";case VK_DIVIDE:return L"NUM /";case VK_OEM_MINUS:return L"-";case VK_OEM_PLUS:return L"=";case VK_OEM_COMMA:return L",";case VK_OEM_PERIOD:return L".";case VK_OEM_1:return L";";case VK_OEM_2:return L"/";case VK_OEM_3:return L"`";case VK_OEM_4:return L"[";case VK_OEM_5:return L"\\";case VK_OEM_6:return L"]";case VK_OEM_7:return L"'";default:return L"KEY "+std::to_wstring(vk);}
}

constexpr int ID_OVERLAY_ENABLE = 268;
constexpr int ID_OVERLAY_HOTKEY = 269;

struct EnvCaseLess {
    bool operator()(const std::wstring& a,const std::wstring& b) const { return _wcsicmp(a.c_str(),b.c_str())<0; }
};

std::vector<wchar_t> ChildEnvironment(const std::map<std::wstring,std::wstring,EnvCaseLess>& overrides) {
    std::map<std::wstring,std::wstring,EnvCaseLess> values;
    LPWCH block=GetEnvironmentStringsW();
    if(block){for(const wchar_t* p=block;*p;p+=wcslen(p)+1){const wchar_t* separator=wcschr(p+(*p==L'='?1:0),L'=');if(!separator)continue;values[std::wstring(p,size_t(separator-p))]=separator+1;}FreeEnvironmentStringsW(block);}
    for(const auto& entry:overrides){if(entry.second.empty())values.erase(entry.first);else values[entry.first]=entry.second;}
    std::vector<wchar_t> result;
    for(const auto& entry:values){result.insert(result.end(),entry.first.begin(),entry.first.end());result.push_back(L'=');result.insert(result.end(),entry.second.begin(),entry.second.end());result.push_back(L'\0');}
    result.push_back(L'\0');return result;
}

int CapturedVirtualKey(UINT message, WPARAM key, LPARAM data) {
    int vk=static_cast<int>(key);
    const bool extended=(data&(1LL<<24))!=0;
    if(vk==VK_SHIFT)vk=static_cast<int>(MapVirtualKeyW((data>>16)&0xff,MAPVK_VSC_TO_VK_EX));
    else if(vk==VK_CONTROL)vk=extended?VK_RCONTROL:VK_LCONTROL;
    else if(vk==VK_MENU)vk=extended?VK_RMENU:VK_LMENU;
    if(message==WM_SYSKEYDOWN && vk==VK_F4)return 0;
    return vk;
}

LRESULT CALLBACK KeyCaptureSubclass(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR) {
    if(msg==WM_GETDLGCODE)return DefSubclassProc(hwnd,msg,wp,lp)|DLGC_WANTALLKEYS;
    if((msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN)&&g_capturingOverlayKey&&int(id)==ID_OVERLAY_HOTKEY){
        if(lp&(1LL<<30))return 0;
        const int vk=CapturedVirtualKey(msg,wp,lp);
        if(vk==VK_ESCAPE){g_capturingOverlayKey=false;SetWindowTextW(hwnd,KeyName(g_settings.overlayToggleKey).c_str());SetWindowTextW(g_status,L"Overlay hotkey assignment cancelled.");InvalidateRect(hwnd,nullptr,TRUE);return 0;}
        const bool invalid=!vk||vk==VK_LWIN||vk==VK_RWIN||vk==VK_APPS||vk==VK_SNAPSHOT||vk==VK_SCROLL||vk==VK_PAUSE||vk==VK_NUMLOCK||vk==VK_CAPITAL||vk==VK_SHIFT||vk==VK_CONTROL||vk==VK_MENU||vk==VK_LBUTTON||vk==VK_RBUTTON||vk==VK_MBUTTON;
        if(!invalid){g_settings.overlayToggleKey=vk;g_capturingOverlayKey=false;SetWindowTextW(hwnd,KeyName(vk).c_str());SetWindowTextW(g_status,L"Overlay toggle key assigned. Save to apply on next launch.");InvalidateRect(hwnd,nullptr,TRUE);return 0;}
        return 0;
    }
    if((msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN)&&g_capturingKey>=0){
        if(lp&(1LL<<30))return 0;
        const int vk=CapturedVirtualKey(msg,wp,lp);
        if(!vk)return DefSubclassProc(hwnd,msg,wp,lp);
        if(vk==VK_ESCAPE){const int index=g_capturingKey;const auto& map=g_controlPlayer==0?g_settings.keys:g_settings.keys2;SetWindowTextW(hwnd,KeyName(map[index]).c_str());g_capturingKey=-1;SetWindowTextW(g_status,L"Key assignment cancelled.");InvalidateRect(hwnd,nullptr,TRUE);return 0;}
        const bool invalid=!vk||vk==VK_LWIN||vk==VK_RWIN||vk==VK_APPS||vk==VK_SNAPSHOT||vk==VK_SCROLL||vk==VK_PAUSE||vk==VK_NUMLOCK||vk==VK_CAPITAL||vk==VK_SHIFT||vk==VK_CONTROL||vk==VK_MENU||vk==VK_LBUTTON||vk==VK_RBUTTON||vk==VK_MBUTTON;
        if(!invalid){const int index=g_capturingKey;auto& map=g_controlPlayer==0?g_settings.keys:g_settings.keys2;for(size_t i=0;i<map.size();++i)if(int(i)!=index&&map[i]==vk)map[i]=map[index];map[index]=vk;SetWindowTextW(hwnd,KeyName(vk).c_str());g_capturingKey=-1;SetWindowTextW(g_status,L"Key assigned. Save to apply on next launch.");InvalidateRect(hwnd,nullptr,TRUE);return 0;}
        return 0;
    }
    if(msg==WM_NCDESTROY)RemoveWindowSubclass(hwnd,KeyCaptureSubclass,id);
    return DefSubclassProc(hwnd,msg,wp,lp);
}

std::wstring LocalRoot() {
    wchar_t path[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    std::wstring root = n && n < MAX_PATH ? path : L".";
    root += L"\\TTT2Recomp";
    CreateDirectoryW(root.c_str(), nullptr);
    return root;
}
std::wstring RuntimeIni() { return LocalRoot() + L"\\settings.ini"; }
std::wstring LauncherIni() { return LocalRoot() + L"\\launcher.ini"; }
std::wstring InstallDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring result(path);
    const size_t slash = result.find_last_of(L"\\/");
    if (slash != std::wstring::npos) result.resize(slash);
    return result;
}
int IniInt(const wchar_t* section, const wchar_t* key, int fallback, const std::wstring& path) {
    return GetPrivateProfileIntW(section, key, fallback, path.c_str());
}
bool IniBool(const wchar_t* key, bool fallback) {
    return IniInt(L"Performance", key, fallback ? 1 : 0, LauncherIni()) != 0;
}
void LoadSettings() {
    const auto runtime = RuntimeIni();
    g_settings.mode = std::clamp(IniInt(L"Display", L"Mode", 0, runtime), 0, 2);
    wchar_t res[32]{};
    GetPrivateProfileStringW(L"Display", L"Resolution", L"1280x720", res, 32, runtime.c_str());
    const std::array<const wchar_t*,5> resolutions{L"1280x720",L"1600x900",L"1920x1080",L"2560x1440",L"3840x2160"};
    g_settings.resolution = 0;
    for (size_t i=0;i<resolutions.size();++i) if (_wcsicmp(res,resolutions[i])==0) g_settings.resolution=int(i);
    const auto cfg = LauncherIni();
    wchar_t scale[24]{};
    GetPrivateProfileStringW(L"Graphics", L"RenderScale", L"auto", scale, 24, cfg.c_str());
    const std::array<const wchar_t*,6> scales{L"auto",L"1.0",L"1.25",L"1.5",L"2.0",L"3.0"};
    g_settings.scale = 0;
    for(size_t i=0;i<scales.size();++i) if (_wcsicmp(scale,scales[i])==0) g_settings.scale=int(i);
    g_settings.asyncShaders = IniBool(L"AsyncShaders", true);
    g_settings.warmShaders = IniBool(L"WarmShaders", true);
    g_settings.fastShaders = IniBool(L"FastShaders", true);
    g_settings.inputPollRateHz = IniInt(L"Performance", L"InputPollHz", 1000, cfg.c_str());
    g_settings.drawDiagnostics = IniInt(L"Performance", L"DrawDiagnostics", 0, cfg.c_str()) != 0;
    g_settings.consoleLogs = IniInt(L"Performance", L"ConsoleLogs", 0, cfg.c_str()) != 0;
    if (std::find(kInputPollRates.begin(), kInputPollRates.end(), g_settings.inputPollRateHz) == kInputPollRates.end())
        g_settings.inputPollRateHz = 1000;
    g_settings.hardware = IniInt(L"Game", L"HardwareRenderer", 1, cfg.c_str()) != 0;
    g_settings.closeOnLaunch = IniInt(L"Launcher", L"CloseOnLaunch", 1, cfg.c_str()) != 0;
    const wchar_t* overlayKeys[8]{L"FPS",L"FrameTime",L"CPUUsage",L"GPUUsage",L"SystemRAM",L"GPUMemory",L"CPUTemperature",L"GPUTemperature"};
    for(int i=0;i<8;++i)g_settings.overlay[i]=IniInt(L"Overlay",overlayKeys[i],i<2?1:0,cfg.c_str())!=0;
    g_settings.overlayEnabled=IniInt(L"Overlay",L"Enabled",1,cfg.c_str())!=0;
    g_settings.overlayGraph=IniInt(L"Overlay",L"Graph",0,cfg.c_str())!=0;
    g_settings.overlayPosition=std::clamp(IniInt(L"Overlay",L"Position",0,cfg.c_str()),0,3);
    g_settings.netPosition=std::clamp(IniInt(L"Netplay",L"HudPosition",3,cfg.c_str()),0,3);
    g_settings.overlayToggleKey=IniInt(L"Overlay",L"ToggleKey",VK_F10,cfg.c_str());
    wchar_t saveRoot[MAX_PATH]{};
    GetPrivateProfileStringW(L"Game", L"SaveRoot", L"", saveRoot, MAX_PATH, cfg.c_str());
    g_settings.saveRoot = saveRoot;
    const wchar_t* keyNames[24]{L"Up",L"Down",L"Left",L"Right",L"A",L"B",L"X",L"Y",L"LB",L"RB",L"LT",L"RT",L"LS",L"RS",L"Start",L"Back",L"LSUp",L"LSDown",L"LSLeft",L"LSRight",L"RSUp",L"RSDown",L"RSLeft",L"RSRight"};
    for(int i=0;i<24;++i){g_settings.keys[i]=IniInt(L"Keyboard",keyNames[i],g_settings.keys[i],runtime);g_settings.keys2[i]=IniInt(L"Keyboard2",keyNames[i],g_settings.keys2[i],runtime);}
    g_settings.player2KeyboardEnabled=IniInt(L"Controls",L"Player2Keyboard",0,cfg.c_str())!=0;
    g_settings.padAssignment=std::clamp(IniInt(L"Controls",L"PadAssignMode",1,cfg.c_str()),0,3);
    g_settings.netMode=std::clamp(IniInt(L"Netplay",L"Mode",0,cfg),0,2);
    g_settings.netHud=IniInt(L"Netplay",L"Hud",1,cfg)!=0;
    g_settings.netRecovery=IniInt(L"Netplay",L"PacketRecovery",1,cfg)!=0;
    wchar_t localIp[64]{},peerIps[512]{};
    GetPrivateProfileStringW(L"Netplay",L"LocalIPv4",L"",localIp,64,cfg.c_str());
    GetPrivateProfileStringW(L"Netplay",L"PeerIPv4",L"",peerIps,512,cfg.c_str());
    g_settings.netLocalIp=localIp;g_settings.netPeerIps=peerIps;
    // Drop settings left by the removed private-network discovery page.
    WritePrivateProfileStringW(L"VPN", nullptr, nullptr, cfg.c_str());
}
void SaveSettings() {
    const auto runtime = RuntimeIni();
    wchar_t number[24]{};
    swprintf_s(number, L"%d", g_settings.mode);
    WritePrivateProfileStringW(L"Display", L"Mode", number, runtime.c_str());
    const std::array<const wchar_t*,5> resolutions{L"1280x720",L"1600x900",L"1920x1080",L"2560x1440",L"3840x2160"};
    WritePrivateProfileStringW(L"Display", L"Resolution", resolutions[g_settings.resolution], runtime.c_str());
    const auto cfg = LauncherIni();
    const std::array<const wchar_t*,6> scales{L"auto",L"1.0",L"1.25",L"1.5",L"2.0",L"3.0"};
    WritePrivateProfileStringW(L"Graphics", L"RenderScale", scales[g_settings.scale], cfg.c_str());
    auto put = [&](const wchar_t* section,const wchar_t* key,bool value){WritePrivateProfileStringW(section,key,value?L"1":L"0",cfg.c_str());};
    put(L"Performance",L"AsyncShaders",g_settings.asyncShaders);
    put(L"Performance",L"WarmShaders",g_settings.warmShaders);
    put(L"Performance",L"FastShaders",g_settings.fastShaders);
    swprintf_s(number, L"%d", g_settings.inputPollRateHz);
    WritePrivateProfileStringW(L"Performance", L"InputPollHz", number, cfg.c_str());
    put(L"Performance",L"DrawDiagnostics",g_settings.drawDiagnostics);
    put(L"Performance",L"ConsoleLogs",g_settings.consoleLogs);
    put(L"Game",L"HardwareRenderer",g_settings.hardware);
    put(L"Launcher",L"CloseOnLaunch",g_settings.closeOnLaunch);
    const wchar_t* overlayKeys[8]{L"FPS",L"FrameTime",L"CPUUsage",L"GPUUsage",L"SystemRAM",L"GPUMemory",L"CPUTemperature",L"GPUTemperature"};
    for(int i=0;i<8;++i){wchar_t value[2]{g_settings.overlay[i]?L'1':L'0',L'\0'};WritePrivateProfileStringW(L"Overlay",overlayKeys[i],value,cfg.c_str());}
    put(L"Overlay",L"Enabled",g_settings.overlayEnabled);put(L"Overlay",L"Graph",g_settings.overlayGraph);
    swprintf_s(number,L"%d",g_settings.overlayPosition);WritePrivateProfileStringW(L"Overlay",L"Position",number,cfg.c_str());
    swprintf_s(number,L"%d",g_settings.netPosition);WritePrivateProfileStringW(L"Netplay",L"HudPosition",number,cfg.c_str());
    swprintf_s(number,L"%d",g_settings.overlayToggleKey);WritePrivateProfileStringW(L"Overlay",L"ToggleKey",number,cfg.c_str());
    WritePrivateProfileStringW(L"Game",L"SaveRoot",g_settings.saveRoot.c_str(),cfg.c_str());
    const wchar_t* keyNames[24]{L"Up",L"Down",L"Left",L"Right",L"A",L"B",L"X",L"Y",L"LB",L"RB",L"LT",L"RT",L"LS",L"RS",L"Start",L"Back",L"LSUp",L"LSDown",L"LSLeft",L"LSRight",L"RSUp",L"RSDown",L"RSLeft",L"RSRight"};
    for(int i=0;i<24;++i){swprintf_s(number,L"0x%02X",unsigned(g_settings.keys[i]));WritePrivateProfileStringW(L"Keyboard",keyNames[i],number,runtime.c_str());swprintf_s(number,L"0x%02X",unsigned(g_settings.keys2[i]));WritePrivateProfileStringW(L"Keyboard2",keyNames[i],number,runtime.c_str());}
    swprintf_s(number,L"%d",g_settings.netMode);WritePrivateProfileStringW(L"Netplay",L"Mode",number,cfg.c_str());
    put(L"Netplay",L"Hud",g_settings.netHud);put(L"Netplay",L"PacketRecovery",g_settings.netRecovery);
    WritePrivateProfileStringW(L"Netplay",L"LocalIPv4",g_settings.netLocalIp.c_str(),cfg.c_str());
    WritePrivateProfileStringW(L"Netplay",L"PeerIPv4",g_settings.netPeerIps.c_str(),cfg.c_str());
    put(L"Controls",L"Player2Keyboard",g_settings.player2KeyboardEnabled);
    swprintf_s(number,L"%d",g_settings.padAssignment);WritePrivateProfileStringW(L"Controls",L"PadAssignMode",number,cfg.c_str());
}
std::wstring ControlText(int id) {
    HWND h=GetDlgItem(g_window,id);const int n=h?GetWindowTextLengthW(h):0;
    std::wstring value(size_t(std::max(n,0))+1,L'\0');if(h)GetWindowTextW(h,value.data(),int(value.size()));
    value.resize(wcslen(value.c_str()));return value;
}
std::wstring LoadRankedValue(const wchar_t* key,const wchar_t* fallback) {
    wchar_t value[2048]{};const auto path=LauncherIni();GetPrivateProfileStringW(L"Ranked",key,fallback,value,2048,path.c_str());return value;
}
void SaveRankedSettings() {
    if(!GetDlgItem(g_window,ID_RANKED_URL))return;
    const auto path=LauncherIni();
    WritePrivateProfileStringW(L"Ranked",L"Url",ControlText(ID_RANKED_URL).c_str(),path.c_str());
    WritePrivateProfileStringW(L"Ranked",L"Build",ControlText(ID_RANKED_BUILD).c_str(),path.c_str());
    WritePrivateProfileStringW(L"Ranked",L"Region",ControlText(ID_RANKED_REGION).c_str(),path.c_str());
    const std::wstring token=ControlText(ID_RANKED_TOKEN);
    if(token.empty()){WritePrivateProfileStringW(L"Ranked",L"TokenDPAPI",nullptr,path.c_str());return;}
    DATA_BLOB input{DWORD(token.size()*sizeof(wchar_t)),reinterpret_cast<BYTE*>(const_cast<wchar_t*>(token.data()))},output{};
    if(CryptProtectData(&input,L"TTT2 ranked service token",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output)){
        static constexpr wchar_t hex[]=L"0123456789ABCDEF";std::wstring encoded;encoded.reserve(output.cbData*2);
        for(DWORD i=0;i<output.cbData;++i){encoded.push_back(hex[output.pbData[i]>>4]);encoded.push_back(hex[output.pbData[i]&15]);}
        WritePrivateProfileStringW(L"Ranked",L"TokenDPAPI",encoded.c_str(),path.c_str());LocalFree(output.pbData);
    }
}
std::wstring LoadRankedToken() {
    const auto encoded=LoadRankedValue(L"TokenDPAPI",L"");if(encoded.empty()||encoded.size()%2)return L"";
    std::vector<BYTE> bytes(encoded.size()/2);auto nibble=[](wchar_t c)->int{if(c>=L'0'&&c<=L'9')return int(c-L'0');if(c>=L'A'&&c<=L'F')return int(c-L'A'+10);if(c>=L'a'&&c<=L'f')return int(c-L'a'+10);return -1;};
    for(size_t i=0;i<bytes.size();++i){const int a=nibble(encoded[i*2]),b=nibble(encoded[i*2+1]);if(a<0||b<0)return L"";bytes[i]=BYTE((a<<4)|b);}
    DATA_BLOB input{DWORD(bytes.size()),bytes.data()},output{};std::wstring token;
    if(CryptUnprotectData(&input,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output)){
        token.assign(reinterpret_cast<wchar_t*>(output.pbData),output.cbData/sizeof(wchar_t));LocalFree(output.pbData);
    }
    return token;
}
std::string Utf8(const std::wstring& value) {
    if(value.empty())return {};const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),int(value.size()),nullptr,0,nullptr,nullptr);
    if(n<=0)return {};std::string result(size_t(n),'\0');WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),int(value.size()),result.data(),n,nullptr,nullptr);return result;
}
std::string JsonString(const std::wstring& value) {
    const auto utf8=Utf8(value);std::string out="\"";for(unsigned char c:utf8){if(c=='\\'||c=='\"'){out.push_back('\\');out.push_back(char(c));}else if(c<0x20){static constexpr char hex[]="0123456789abcdef";out+="\\u00";out.push_back(hex[c>>4]);out.push_back(hex[c&15]);}else out.push_back(char(c));}out.push_back('\"');return out;
}
std::wstring FromUtf8(const std::string& value) {
    const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),nullptr,0);if(n<=0)return L"";
    std::wstring result(size_t(n),L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),result.data(),n);return result;
}
std::string RankedBody(const std::wstring& response) {
    const size_t at=response.find(L"\r\n");return at==std::wstring::npos?std::string{}:Utf8(response.substr(at+2));
}
std::string JsonField(const std::string& json,const char* key) {
    const std::string marker=std::string("\"")+key+"\"";size_t at=json.find(marker);
    if(at==std::string::npos||(at=json.find(':',at+marker.size()))==std::string::npos)return {};
    ++at;while(at<json.size()&&(json[at]==' '||json[at]=='\t'||json[at]=='\r'||json[at]=='\n'))++at;
    if(at>=json.size())return {};
    if(json[at]=='\"'){
        std::string out;for(++at;at<json.size();++at){if(json[at]=='\"')return out;if(json[at]=='\\'&&at+1<json.size())++at;out.push_back(json[at]);}return {};
    }
    size_t end=at;while(end<json.size()&&json[end]!=','&&json[end]!='}'&&json[end]!=']')++end;
    while(end>at&&(json[end-1]==' '||json[end-1]=='\r'||json[end-1]=='\n'))--end;
    return json.substr(at,end-at);
}
double JsonNumber(const std::string& json,const char* key,double fallback=0.0) {
    const std::string text=JsonField(json,key);if(text.empty())return fallback;
    char* end=nullptr;const double result=std::strtod(text.c_str(),&end);return end&&*end=='\0'?result:fallback;
}
std::string RedactMatchToken(std::string body) {
    for(const char* field:{"match_token","session_key","session_key_bytes","opponent_endpoint"}){
        const std::string key=std::string("\"")+field+"\"";size_t at=body.find(key);if(at==std::string::npos)continue;
        at=body.find(':',at+key.size());if(at==std::string::npos)continue;at=body.find('\"',at+1);if(at==std::string::npos)continue;
        const size_t end=body.find('\"',at+1);if(end!=std::string::npos)body.replace(at+1,end-at-1,"[hidden]");
    }
    return body;
}
std::wstring SafeRankedDisplay(const std::wstring& response) {
    const size_t split=response.find(L"\r\n");
    return split==std::wstring::npos?response:response.substr(0,split+2)+FromUtf8(RedactMatchToken(RankedBody(response)));
}
std::wstring RankedRequest(const std::wstring& base,const std::wstring& bearer,const std::wstring& route,const std::string& payload) {
    URL_COMPONENTSW parts{};parts.dwStructSize=sizeof(parts);parts.dwSchemeLength=DWORD(-1);parts.dwHostNameLength=DWORD(-1);parts.dwUrlPathLength=DWORD(-1);parts.dwExtraInfoLength=DWORD(-1);
    if(!WinHttpCrackUrl(base.c_str(),0,0,&parts))return L"Invalid server URL.";
    if((parts.dwUrlPathLength>1)||(parts.dwExtraInfoLength>0)||parts.dwUserNameLength||parts.dwPasswordLength)return L"Enter the service root URL only, without a path, query, or user information.";
    const bool https=parts.nScheme==INTERNET_SCHEME_HTTPS;std::wstring host(parts.lpszHostName,parts.dwHostNameLength);
    const bool local=_wcsicmp(host.c_str(),L"localhost")==0||_wcsicmp(host.c_str(),L"127.0.0.1")==0||_wcsicmp(host.c_str(),L"::1")==0;
    if(!https&&!local)return L"Use HTTPS for remote ranked servers. Plain HTTP is allowed only on this PC.";
    std::wstring path=route;HINTERNET session=WinHttpOpen(L"TTT2 Community Launcher/1.0",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!session)return L"Could not initialize Windows HTTP.";
    WinHttpSetTimeouts(session,3000,3000,4000,4000);
    HINTERNET connection=WinHttpConnect(session,host.c_str(),parts.nPort,0);if(!connection){WinHttpCloseHandle(session);return L"Could not connect to the ranked server.";}
    HINTERNET request=WinHttpOpenRequest(connection,payload.empty()?L"GET":L"POST",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,https?WINHTTP_FLAG_SECURE:0);
    if(!request){WinHttpCloseHandle(connection);WinHttpCloseHandle(session);return L"Could not create the server request.";}
    std::wstring headers=L"Accept: application/json\r\n";if(!bearer.empty())headers+=L"Authorization: Bearer "+bearer+L"\r\n";
    if(!payload.empty())headers+=L"Content-Type: application/json; charset=utf-8\r\n";
    const BOOL sent=WinHttpSendRequest(request,headers.c_str(),DWORD(headers.size()),payload.empty()?WINHTTP_NO_REQUEST_DATA:(LPVOID)payload.data(),DWORD(payload.size()),DWORD(payload.size()),0);
    BOOL received=sent&&WinHttpReceiveResponse(request,nullptr);DWORD status=0,size=sizeof(status);
    if(received)WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX);
    std::string body;
    if(received){DWORD available=0;while(WinHttpQueryDataAvailable(request,&available)&&available){if(body.size()+available>16384){body="Response exceeded the 16 KB limit.";break;}const size_t offset=body.size();body.resize(offset+available);DWORD read=0;if(!WinHttpReadData(request,body.data()+offset,available,&read)){body="Could not read the server response.";break;}body.resize(offset+read);if(!read)break;}}
    WinHttpCloseHandle(request);WinHttpCloseHandle(connection);WinHttpCloseHandle(session);
    if(!received)return L"Request failed or timed out. Check the URL and server.";
    return L"HTTP "+std::to_wstring(status)+L"\r\n"+FromUtf8(body);
}
std::string NewPlayerId() {
    wchar_t username[128]{};
    DWORD length=GetEnvironmentVariableW(L"USERNAME",username,static_cast<DWORD>(std::size(username)));
    std::string prefix;
    if(length>0&&length<std::size(username)) {
        const std::string utf8=Utf8(username);
        for(unsigned char c:utf8)if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')){
            prefix.push_back(char(c>='A'&&c<='Z'?c+('a'-'A'):c));if(prefix.size()==32)break;
        }
    }
    if(prefix.empty())prefix="player";
    std::array<UCHAR,4> random{};
    if(BCryptGenRandom(nullptr,random.data(),static_cast<ULONG>(random.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)!=0)
        return {};
    static constexpr char hex[]="0123456789abcdef";
    std::string id=prefix+"-";
    for(UCHAR byte:random){id.push_back(hex[byte>>4]);id.push_back(hex[byte&15]);}
    return id;
}
std::wstring RegistrationToken(const std::wstring& response) {
    const size_t marker=response.find(L"\"token\"");
    if(marker==std::wstring::npos)return {};
    const size_t colon=response.find(L':',marker+7);
    if(colon==std::wstring::npos)return {};
    const size_t quote=response.find(L'\"',colon+1);
    if(quote==std::wstring::npos)return {};
    const size_t end=response.find(L'\"',quote+1);
    if(end==std::wstring::npos)return {};
    const std::wstring token=response.substr(quote+1,end-quote-1);
    if(token.size()<32||token.size()>128)return {};
    for(wchar_t c:token)if(!((c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9')||c==L'_'||c==L'-'))return {};
    return token;
}
void StartRankedRegistration() {
    if(g_rankedBusy.exchange(true))return;
    const std::wstring base=ControlText(ID_RANKED_URL);
    const std::string player=NewPlayerId();
    if(player.empty()){g_rankedBusy=false;SetWindowTextW(g_status,L"Secure token generation failed.");return;}
    SaveRankedSettings();
    for(HWND h:{GetDlgItem(g_window,ID_RANKED_HEALTH),GetDlgItem(g_window,ID_RANKED_REGISTER),GetDlgItem(g_window,ID_RANKED_PROFILE),GetDlgItem(g_window,ID_RANKED_QUEUE),GetDlgItem(g_window,ID_RANKED_STATUS),GetDlgItem(g_window,ID_RANKED_CANCEL)})if(h)EnableWindow(h,FALSE);
    SetWindowTextW(GetDlgItem(g_window,ID_RANKED_RESULT),L"Creating a new player identity...");
    SetWindowTextW(g_status,L"Registering account with ranked service");
    const HWND target=g_window;
    const std::string payload=std::string("{\"player\":\"")+player+"\"}";
    std::thread([target,base,payload]{auto* result=new RankedRegistrationResult{RankedRequest(base,L"",L"/v1/register",payload)};if(!PostMessageW(target,WM_RANKED_REGISTER_COMPLETE,0,reinterpret_cast<LPARAM>(result)))delete result;}).detach();
}
void LockRankedChallenge(bool locked) {
    for(HWND h:g_nav)if(h)EnableWindow(h,!locked);
    if(g_play)EnableWindow(g_play,!locked);
    if(HWND save=GetDlgItem(g_window,ID_SAVE))EnableWindow(save,!locked);
    for(HWND h:g_page)if(h)EnableWindow(h,!locked);
    for(int id:{ID_RANKED_HEALTH,ID_RANKED_REGISTER,ID_RANKED_PROFILE,ID_RANKED_QUEUE,ID_RANKED_STATUS,ID_RANKED_CANCEL})
        if(HWND h=GetDlgItem(g_window,id))EnableWindow(h,!locked);
    if(g_challengeAccept)EnableWindow(g_challengeAccept,locked&&!g_acceptSubmitted);
    if(g_challengeDecline)EnableWindow(g_challengeDecline,locked&&!g_acceptSubmitted);
}
void UpdateQueueCooldown() {
    const double remaining=std::max(0.0,std::chrono::duration<double>(g_queueCooldownUntil-std::chrono::steady_clock::now()).count());
    if(HWND queue=GetDlgItem(g_window,ID_RANKED_QUEUE))EnableWindow(queue,remaining<=0.0);
    if(remaining>0.0)SetTimer(g_window,ID_COOLDOWN_TIMER,1000,nullptr);
    else KillTimer(g_window,ID_COOLDOWN_TIMER);
}
void CloseChallenge() {
    KillTimer(g_window,ID_CHALLENGE_TIMER);
    for(HWND* h:{&g_challengePanel,&g_challengeTitle,&g_challengeOpponent,&g_challengeRating,
                 &g_challengeDisconnect,&g_challengeTime,&g_challengeProgress,&g_challengeAccept,&g_challengeDecline})
        if(*h){DestroyWindow(*h);*h=nullptr;}
    g_pendingMatchId.clear();g_acceptSubmitted=false;LockRankedChallenge(false);
}
HWND ChallengeStatic(const wchar_t* text,int x,int y,int w,int h,HFONT font) {
    HWND control=CreateWindowExW(0,L"STATIC",text,WS_CHILD|WS_VISIBLE|SS_LEFT|SS_NOPREFIX,
        x,y,w,h,g_window,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(control&&font)SendMessageW(control,WM_SETFONT,(WPARAM)font,TRUE);
    return control;
}
void LayoutChallenge() {
    if(!g_challengePanel)return;
    RECT client{};GetClientRect(g_window,&client);
    const int width=(std::min)(520,(std::max)(420,int(client.right)-80)),height=310;
    const int x=(client.right-width)/2,y=(g_viewportTop+g_viewportBottom-height)/2;
    const int left=x+28,inner=width-56;
    MoveWindow(g_challengePanel,x,y,width,height,TRUE);
    MoveWindow(g_challengeTitle,left,y+22,inner,34,TRUE);
    MoveWindow(g_challengeOpponent,left,y+78,inner,25,TRUE);
    MoveWindow(g_challengeRating,left,y+108,inner,24,TRUE);
    MoveWindow(g_challengeDisconnect,left,y+136,inner,24,TRUE);
    MoveWindow(g_challengeTime,left,y+174,inner,26,TRUE);
    MoveWindow(g_challengeProgress,left,y+207,inner,18,TRUE);
    MoveWindow(g_challengeAccept,left,y+246,inner/2-8,40,TRUE);
    MoveWindow(g_challengeDecline,left+inner/2+8,y+246,inner/2-8,40,TRUE);
}
void ShowChallenge(const std::string& body,bool updateDeadline) {
    const std::string matchId=JsonField(body,"match_id");
    if(matchId.empty())return;
    const double seconds=(std::max)(0.0,(std::min)(12.0,JsonNumber(body,"time_remaining",12.0)));
    if(!g_challengePanel){
        RECT client{};GetClientRect(g_window,&client);
        const int width=(std::min)(520,(std::max)(420,int(client.right)-80)),height=310;
        const int x=(client.right-width)/2,y=(g_viewportTop+g_viewportBottom-height)/2;
        g_challengePanel=CreateWindowExW(WS_EX_CLIENTEDGE,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_NOTIFY,
            x,y,width,height,g_window,nullptr,GetModuleHandleW(nullptr),nullptr);
        const int left=x+28,inner=width-56;
        g_challengeTitle=ChallengeStatic(L"CHALLENGER FOUND!",left,y+22,inner,34,g_headingFont);
        g_challengeOpponent=ChallengeStatic(L"Opponent",left,y+78,inner,25,g_bodyFont);
        g_challengeRating=ChallengeStatic(L"Rating",left,y+108,inner,24,g_bodyFont);
        g_challengeDisconnect=ChallengeStatic(L"Disconnection rate",left,y+136,inner,24,g_bodyFont);
        g_challengeTime=ChallengeStatic(L"Time remaining",left,y+174,inner,26,g_sectionFont);
        g_challengeProgress=CreateWindowExW(0,PROGRESS_CLASSW,L"",WS_CHILD|WS_VISIBLE|PBS_SMOOTH,
            left,y+207,inner,18,g_window,(HMENU)(INT_PTR)0,GetModuleHandleW(nullptr),nullptr);
        if(g_challengeProgress){SendMessageW(g_challengeProgress,PBM_SETRANGE32,0,1200);SendMessageW(g_challengeProgress,PBM_SETBARCOLOR,0,kRed);}
        g_challengeAccept=CreateWindowExW(0,L"BUTTON",L"ACCEPT MATCH",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW|WS_TABSTOP,
            left,y+246,inner/2-8,40,g_window,(HMENU)(INT_PTR)ID_RANKED_ACCEPT,GetModuleHandleW(nullptr),nullptr);
        g_challengeDecline=CreateWindowExW(0,L"BUTTON",L"DECLINE",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW|WS_TABSTOP,
            left+inner/2+8,y+246,inner/2-8,40,g_window,(HMENU)(INT_PTR)ID_RANKED_DECLINE,GetModuleHandleW(nullptr),nullptr);
        for(HWND control:{g_challengeAccept,g_challengeDecline})if(control)SendMessageW(control,WM_SETFONT,(WPARAM)g_bodyFont,TRUE);
        LockRankedChallenge(true);
        SetTimer(g_window,ID_CHALLENGE_TIMER,50,nullptr);
    }
    LayoutChallenge();
    g_pendingMatchId=FromUtf8(matchId);
    if(updateDeadline)g_acceptDeadline=std::chrono::steady_clock::now()+std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
    std::string playerId;
    const size_t opponentAt=body.find("\"opponent\"");
    if(opponentAt!=std::string::npos)playerId=JsonField(body.substr(opponentAt),"player_id");
    std::string rating="—",disconnect="—";
    if(opponentAt!=std::string::npos){const std::string section=body.substr(opponentAt);const std::string r=JsonField(section,"rating"),d=JsonField(section,"disconnect_rate");if(!r.empty())rating=r;if(!d.empty()){char* end=nullptr;double value=std::strtod(d.c_str(),&end);if(end&&*end=='\0')disconnect=std::to_string(int(value*100+0.5))+"%";}}
    SetWindowTextW(g_challengeOpponent,(L"Opponent: "+FromUtf8(playerId)).c_str());
    SetWindowTextW(g_challengeRating,(L"Rating: "+FromUtf8(rating)).c_str());
    SetWindowTextW(g_challengeDisconnect,(L"Disconnection rate: "+FromUtf8(disconnect)).c_str());
    InvalidateRect(g_window,nullptr,FALSE);
}
void StartRankedAsync(const std::wstring& route,const std::string& payload,int action) {
    if(g_rankedBusy.exchange(true))return;
    const std::wstring base=ControlText(ID_RANKED_URL),token=ControlText(ID_RANKED_TOKEN);
    SaveRankedSettings();
    for(int id:{ID_RANKED_HEALTH,ID_RANKED_REGISTER,ID_RANKED_PROFILE,ID_RANKED_QUEUE,ID_RANKED_STATUS,ID_RANKED_CANCEL})
        if(HWND h=GetDlgItem(g_window,id))EnableWindow(h,FALSE);
    SetWindowTextW(GetDlgItem(g_window,ID_RANKED_RESULT),L"Contacting ranked service...");
    const HWND target=g_window;
    std::thread([target,base,token,route,payload,action]{
        auto* result=new std::wstring(RankedRequest(base,token,route,payload));
        if(!PostMessageW(target,WM_APP+6,WPARAM(action),LPARAM(result)))delete result;
    }).detach();
}
void StartRankedRequest(int action) {
    const std::wstring build=ControlText(ID_RANKED_BUILD),region=ControlText(ID_RANKED_REGION);
    std::wstring route;std::string payload;
    switch(action){case ID_RANKED_HEALTH:route=L"/healthz";break;
    case ID_RANKED_PROFILE:route=L"/v1/profile";payload="{}";break;
    case ID_RANKED_QUEUE:route=L"/v1/queue";payload="{\"build\":"+JsonString(build)+",\"region\":"+JsonString(region)+"}";g_rankedSearching=true;g_queueLeaseRenew=true;g_lastQueueRenew=std::chrono::steady_clock::now();SetTimer(g_window,ID_STATUS_TIMER,1000,nullptr);break;
    case ID_RANKED_STATUS:route=L"/v1/status";payload="{}";break;
    case ID_RANKED_CANCEL:route=L"/v1/cancel";payload="{}";g_rankedSearching=false;KillTimer(g_window,ID_STATUS_TIMER);break;default:return;}
    SetWindowTextW(g_status,L"Ranked service request in progress");
    StartRankedAsync(route,payload,action);
}
HWND Add(int id, const wchar_t* cls, const wchar_t* text, DWORD style, int x,int y,int w,int h,
         int layoutHeight=0) {
    style|=WS_CLIPSIBLINGS;
    if (wcscmp(cls,L"BUTTON")==0 && id!=0) style=(style&~BS_TYPEMASK)|BS_OWNERDRAW|WS_TABSTOP;
    HWND control=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,g_window,(HMENU)(INT_PTR)id,
        GetModuleHandleW(nullptr),nullptr);
    if(control){
        SendMessageW(control,WM_SETFONT,(WPARAM)g_bodyFont,TRUE);
        const bool combo=wcscmp(cls,L"COMBOBOX")==0;
        if(wcscmp(cls,L"EDIT")==0||combo)SetWindowTheme(control,L"DarkMode_Explorer",nullptr);
        // Keep native combo mouse handling intact. The scroll-forward subclass is
        // for ordinary page controls only; combo dropdowns own their pointer input.
        if(!combo)SetWindowSubclass(control,WheelForwardSubclass,0,0);
        const int logicalHeight=layoutHeight>0?layoutHeight:h;
        g_page.push_back(control);g_pageItems.push_back({control,RECT{x,y,x+w,y+logicalHeight},h});g_pageBottom=(std::max)(g_pageBottom,y+logicalHeight);
    }
    return control;
}
void Label(const wchar_t* text,int x,int y,int w,int h,bool muted=false) {
    HWND c=Add(0,L"STATIC",text,SS_LEFT|SS_NOPREFIX,x,y,w,h);
    if(c) SetPropW(c,L"ttt2.muted",(HANDLE)(INT_PTR)(muted?1:0));
}
void Select(HWND combo,int index) { if(combo) SendMessageW(combo,CB_SETCURSEL,index,0); }
HWND Combo(int id,int x,int y,int width,const std::vector<std::wstring>& items,int selected) {
    constexpr int fieldHeight=32;
    constexpr int listRowHeight=26;
    const int visibleRows=(std::max)(1,(int)(std::min)(items.size(),size_t(8)));
    const int expandedHeight=fieldHeight+visibleRows*listRowHeight+4;
    HWND c=Add(id,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL,x,y,width,expandedHeight,fieldHeight);
    for(const auto& item:items) SendMessageW(c,CB_ADDSTRING,0,(LPARAM)item.c_str());
    Select(c,selected);
    return c;
}
HWND Check(int id,const wchar_t* text,int x,int y,bool checked,int width=0) {
    (void)checked;
    return Add(id,L"BUTTON",text,BS_OWNERDRAW|WS_TABSTOP,x,y,width>0?width:(std::max)(160,g_contentX+g_contentWidth-x-2),28);
}
void DestroyPage() { for(HWND c:g_page) if(IsWindow(c))DestroyWindow(c);g_page.clear();g_pageItems.clear();g_pageBottom=0; }
void Heading(const wchar_t* title,const wchar_t* subtitle) {
    HWND h=Add(0,L"STATIC",title,SS_LEFT|SS_NOPREFIX,g_contentX,145,g_contentWidth,34);
    if(h){SendMessageW(h,WM_SETFONT,(WPARAM)g_headingFont,TRUE);SetPropW(h,L"ttt2.title",(HANDLE)1);}
    Label(subtitle,g_contentX+1,181,g_contentWidth,24,true);
}
void SectionTitle(const wchar_t* title,int y) {
    HWND h=Add(0,L"STATIC",title,SS_LEFT|SS_NOPREFIX,g_contentX,y,g_contentWidth,24);
    if(h) SendMessageW(h,WM_SETFONT,(WPARAM)g_sectionFont,TRUE);
    HWND line=Add(900+y,L"STATIC",L"",SS_OWNERDRAW,g_contentX,y+27,g_contentWidth,1);
    (void)line;
}
void Field(const wchar_t* title,int y,int id,const std::vector<std::wstring>& options,int selected) {
    Label(title,g_contentX+2,y+5,205,24);
    Combo(id,g_contentX+210,y,(std::min)(330,g_contentWidth-220),options,selected);
}
void BuildPage() {
    g_capturingKey=-1;
    g_capturingOverlayKey=false;
    if(g_tab==Online)SaveRankedSettings();
    DestroyPage();
    switch(g_tab) {
    case Home: {
        HWND title=Add(0,L"STATIC",L"READY TO PLAY?",SS_LEFT|SS_NOPREFIX,g_contentX,145,(std::min)(810,g_contentWidth),42);
        if(title) SendMessageW(title,WM_SETFONT,(WPARAM)g_headingFont,TRUE);
        Label(L"TEKKEN TAG TOURNAMENT 2  /  COMMUNITY PC EDITION",g_contentX+2,189,(std::min)(806,g_contentWidth),26,true);
        SectionTitle(L"DISPLAY",252);
        Label(L"Window mode",g_contentX+2,310,190,24);
        Combo(ID_MODE,g_contentX+210,304,250,{L"Windowed",L"Borderless",L"Fullscreen"},g_settings.mode);
        Label(L"Resolution",g_contentX+500,310,120,24);
        Combo(ID_RESOLUTION,g_contentX+620,304,(std::min)(220,g_contentWidth-630),{L"1280 x 720",L"1600 x 900",L"1920 x 1080",L"2560 x 1440",L"3840 x 2160"},g_settings.resolution);
        Label(L"Your changes are stored on this PC and applied when the game starts.",g_contentX+2,354,800,44,true);
        SectionTitle(L"QUICK ACCESS",430);
        Add(ID_OPEN_INSTALL,L"BUTTON",L"Open game folder",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+2,474,170,36);
        Add(ID_OPEN_LOG,L"BUTTON",L"View latest log",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+188,474,170,36);
        Add(ID_OPEN_CONFIG,L"BUTTON",L"Open settings",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+374,474,170,36);
        break;
    }
    case Graphics:
        Heading(L"GRAPHICS",L"Set the display and rendering options for your screen.");
        SectionTitle(L"DISPLAY",214);
        Field(L"Window mode",250,ID_MODE,{L"Windowed",L"Borderless",L"Fullscreen"},g_settings.mode);
        Field(L"Output resolution",294,ID_RESOLUTION,{L"1280 x 720",L"1600 x 900",L"1920 x 1080",L"2560 x 1440",L"3840 x 2160"},g_settings.resolution);
        Field(L"Internal render scale",338,ID_SCALE,{L"Auto (follows resolution)",L"1.0x",L"1.25x",L"1.5x",L"2.0x",L"3.0x"},g_settings.scale);
        Label(L"Changes apply when you next launch the game.",g_contentX+2,386,700,25,true);
        break;
    case Performance:
        Heading(L"PERFORMANCE",L"Choose how shader work is scheduled.");
        SectionTitle(L"SHADER COMPILATION",214);
        Check(ID_ASYNC,L"Compile new shaders in the background",g_contentX+2,248,g_settings.asyncShaders);
        Check(ID_WARM,L"Prepare saved shaders while the game starts",g_contentX+2,280,g_settings.warmShaders);
        Check(ID_FAST,L"Use fast shader compilation during a match",g_contentX+2,312,g_settings.fastShaders);
        Label(L"Background work reduces some first-use stalls; unseen shaders still compile when encountered.",g_contentX+26,344,850,24,true);
        SectionTitle(L"CONTROLLER INPUT POLLING",378);
        Field(L"Input polling rate",414,ID_INPUT_POLL_RATE,
              {L"250 Hz",L"500 Hz",L"750 Hz",L"1000 Hz",L"2000 Hz",L"4000 Hz",L"8000 Hz"},
              static_cast<int>(std::find(kInputPollRates.begin(), kInputPollRates.end(), g_settings.inputPollRateHz) - kInputPollRates.begin()));
        Label(L"Rates above 1000 Hz are capped by Windows/XInput to prevent excess CPU use; applies next launch.",
              g_contentX+2,456,840,36,true);
        Check(ID_DRAW_DIAGNOSTICS,L"Collect detailed draw timings in the runtime log",g_contentX+2,500,g_settings.drawDiagnostics);
        Label(L"Diagnostic mode adds overhead and may lower FPS. Enable briefly while reproducing a slowdown.",
              g_contentX+26,530,900,26,true);
        break;
    case Game: {
        Heading(L"GAME",L"Game data, local saves and graphics device.");
        SectionTitle(L"RENDERER",214);
        Check(ID_HARDWARE,L"Use the Direct3D 11 hardware renderer (recommended)",g_contentX+2,253,g_settings.hardware);
        Label(L"Turn this off only to troubleshoot graphics compatibility.",g_contentX+26,283,760,25,true);
        SectionTitle(L"SAVE LOCATION",332);
        Label(L"Save folder",g_contentX+2,380,180,24);
        HWND edit=Add(ID_SAVE_PATH,L"EDIT",g_settings.saveRoot.c_str(),WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,g_contentX+210,372,400,28);
        if(edit) SendMessageW(edit,WM_SETFONT,(WPARAM)g_bodyFont,TRUE);
        Add(ID_BROWSE,L"BUTTON",L"Browse...",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+622,372,95,28);
        Add(ID_OPEN_SAVES,L"BUTTON",L"Open",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+727,372,78,28);
        Label(L"Leave blank to use the default folder under %LOCALAPPDATA%\\TTT2Recomp\\saves.",
              g_contentX+210,410,600,42,true);
        Label(L"Profiles and game saves use this location. The folder can be changed before launch.",
              g_contentX+2,467,795,42,true);
        Add(ID_SAVE_BACKUP,L"BUTTON",L"Back up saves",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+2,519,150,34);
        Add(ID_SAVE_RESTORE,L"BUTTON",L"Restore latest backup",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+168,519,190,34);
        break;
    }
    case Advanced: {
        Heading(L"ADVANCED",L"Launcher options, performance overlay and quick access to files.");
        SectionTitle(L"LAUNCHER",214);
        Check(ID_CLOSE_ON_LAUNCH,L"Close this window after the game starts",g_contentX+2,253,g_settings.closeOnLaunch,360);
        Check(ID_CONSOLE_LOGS,L"Show runtime console logs",g_contentX+390,253,g_settings.consoleLogs,360);
        Label(L"The console shows live messages; the log file continues saving in the background.",g_contentX+26,283,820,25,true);
        SectionTitle(L"FILES",332);
        Add(ID_OPEN_INSTALL,L"BUTTON",L"Open game folder",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+2,374,170,36);
        Add(ID_OPEN_LOG,L"BUTTON",L"View latest log",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+188,374,150,36);
        Add(ID_VERIFY,L"BUTTON",L"Check install",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+354,374,150,36);
        Add(ID_OPEN_CONFIG,L"BUTTON",L"Open settings folder",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+520,374,190,36);
        Label(L"Log file: %LOCALAPPDATA%\\TTT2Recomp\\log.txt",g_contentX+2,422,790,28,true);
        SectionTitle(L"PERFORMANCE OVERLAY",450);
        Label(L"Choose the live metrics shown over the game. Temperature sensors may report unavailable on unsupported hardware.",g_contentX+2,484,850,36,true);
        Add(ID_OVERLAY_ENABLE,L"BUTTON",L"Show performance overlay",BS_OWNERDRAW|WS_TABSTOP,g_contentX+2,526,260,26);
        Label(L"Toggle key",g_contentX+280,526,90,26);
        HWND overlayKey=Add(ID_OVERLAY_HOTKEY,L"BUTTON",KeyName(g_settings.overlayToggleKey).c_str(),BS_OWNERDRAW|WS_TABSTOP,g_contentX+370,526,180,26);
        if(overlayKey)SetWindowSubclass(overlayKey,KeyCaptureSubclass,ID_OVERLAY_HOTKEY,0);
        {
            const wchar_t* overlayNames[8]{L"Frames per second",L"Frame-time graph",L"CPU usage",L"GPU usage",L"System RAM",L"GPU memory",L"CPU temperature",L"GPU temperature"};
            const int columnWidth=(g_contentWidth-24)/2;
            for(int i=0;i<8;++i){const int col=i/4,row=i%4,x=g_contentX+2+col*(columnWidth+20),y=560+row*28;Add(ID_OVERLAY_FIRST+i,L"BUTTON",overlayNames[i],BS_OWNERDRAW|WS_TABSTOP,x,y,columnWidth,26);}
        Check(ID_OVERLAY_GRAPH,L"Show frame-time graph",g_contentX+2,682,g_settings.overlayGraph,280);
        Label(L"Overlay position",g_contentX+330,682,140,24);
        Combo(ID_OVERLAY_POS,g_contentX+470,678,260,{L"Top left",L"Top right",L"Bottom left",L"Bottom right"},g_settings.overlayPosition);
        }
        break;
    }
    case Controls: {
        Heading(L"CONTROLS",L"Click a binding and press the key you want. Changes apply on the next launch.");
        Add(ID_CONTROL_PLAYER1,L"BUTTON",L"PLAYER 1",BS_OWNERDRAW|WS_TABSTOP,g_contentX+2,210,150,34);
        Add(ID_CONTROL_PLAYER2,L"BUTTON",L"PLAYER 2",BS_OWNERDRAW|WS_TABSTOP,g_contentX+162,210,150,34);
        Check(ID_PLAYER2_ENABLED,L"Enable Player 2 keyboard",g_contentX+340,214,g_settings.player2KeyboardEnabled);
        Label(L"Device assignment",g_contentX+2,258,150,26);
        HWND assignment=Combo(ID_PAD_ASSIGNMENT,g_contentX+162,254,(std::min)(490,g_contentWidth-170),
            {L"Auto (first active device is Player 1)",L"Keyboard Player 1 / Controller Player 2",
             L"Controller Player 1 / Keyboard Player 2",L"Share Player 1"},g_settings.padAssignment);
        if(assignment)SendMessageW(assignment,CB_SETCURSEL,g_settings.padAssignment,0);
        SectionTitle(g_controlPlayer==0?L"PLAYER 1 BINDINGS":L"PLAYER 2 BINDINGS",294);
        {
            auto& map=g_controlPlayer==0?g_settings.keys:g_settings.keys2;
            for(int i=0;i<24;++i){
                const int columnWidth=(g_contentWidth-24)/2;
                const int col=i/12,row=i%12,x=g_contentX+2+col*(columnWidth+20),y=330+row*28;
                Label(ControlNames[i],x+2,y+4,112,20);
                const int id=(g_controlPlayer==0?ID_KEY_FIRST:ID_KEY2_FIRST)+i;
                HWND key=Add(id,L"BUTTON",KeyName(map[i]).c_str(),BS_OWNERDRAW|WS_TABSTOP,x+118,y,(std::max)(180,columnWidth-130),26);
                if(key)SetWindowSubclass(key,KeyCaptureSubclass,static_cast<UINT_PTR>(id),0);
            }
        }
        break;
    }
    case VpnLan: {
        Heading(L"VPN LAN & OVERLAYS",L"Configure private network play and compact telemetry panels.");
        SectionTitle(L"VIRTUAL LAN / VPN DIRECT PLAY",214);
        Label(L"Network transport",g_contentX+2,251,170,24);
        Combo(ID_NET_MODE,g_contentX+180,247,500,{L"Ranked coordinator",L"Virtual LAN / VPN (Radmin, Hamachi, Tailscale, LAN)",L"Offline / local only"},g_settings.netMode);
        Label(L"Local IPv4 override",g_contentX+2,291,170,24);
        Add(ID_NET_LOCAL_IP,L"EDIT",g_settings.netLocalIp.c_str(),WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,g_contentX+180,287,260,28);
        Add(ID_NET_REFRESH,L"BUTTON",L"REFRESH ADAPTERS",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+458,287,175,28);
        net::Adapter adapter;std::wstring detected=L"No active supported IPv4 interface found";
        const auto overrideIp=Utf8(g_settings.netLocalIp);
        if(net::SelectAdapter(adapter,overrideIp.c_str()))detected=L"Interface: "+FromUtf8(adapter.name)+L" - "+FromUtf8(adapter.ip)+L" (interface up; VPN membership is checked in its app)";
        Label(detected.c_str(),g_contentX+180,320,g_contentWidth-190,38,true);
        Label(L"Friend / Host IPv4",g_contentX+2,365,170,24);
        Add(ID_NET_PEER_IP,L"EDIT",g_settings.netPeerIps.c_str(),WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,g_contentX+180,361,600,28);
        Label(L"Comma-separated IPv4 targets; optional :discovery-port. Blank uses broadcast. Leave local override blank to auto-detect.",g_contentX+180,394,g_contentWidth-190,40,true);
        Check(ID_NET_RECOVERY,L"4-packet redundancy (small UDP datagrams)",g_contentX+2,441,g_settings.netRecovery,580);
        Label(L"Netplay overlay position",g_contentX+2,510,190,24);
        Combo(ID_NET_POS,g_contentX+200,506,280,{L"Top left",L"Top right",L"Bottom left",L"Bottom right"},g_settings.netPosition);
        Check(ID_NET_HUD,L"Show Netplay telemetry overlay (F2)",g_contentX+2,548,g_settings.netHud,460);
        break;
    }
    case Online: {
        Heading(L"ONLINE",L"Configure your ranked service account and find community matches.");
        SectionTitle(L"RANKED SERVICE",520);
        Label(L"Service URL",g_contentX+2,559,175,24);
        Add(ID_RANKED_URL,L"EDIT",LoadRankedValue(L"Url",L"http://127.0.0.1:8765").c_str(),WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,g_contentX+180,555,(std::min)(630,g_contentWidth-190),28);
        Label(L"Account token",g_contentX+2,597,175,24);
        std::wstring secret=LoadRankedToken();
        const int tokenWidth=(std::max)(220,(std::min)(500,g_contentWidth-370));
        Add(ID_RANKED_TOKEN,L"EDIT",secret.c_str(),WS_BORDER|ES_AUTOHSCROLL|ES_PASSWORD|WS_TABSTOP,g_contentX+180,593,tokenWidth,32);
        Add(ID_RANKED_REGISTER,L"BUTTON",L"GENERATE TOKEN",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+192+tokenWidth,593,160,32);
        Label(L"Token is saved encrypted for this Windows account.",g_contentX+180,626,(std::min)(680,g_contentWidth-190),26,true);
        Label(L"Build",g_contentX+2,662,175,24);
        Add(ID_RANKED_BUILD,L"EDIT",LoadRankedValue(L"Build",L"ttt2-community").c_str(),WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,g_contentX+180,658,(std::min)(300,(g_contentWidth-210)/2),28);
        Label(L"Region",g_contentX+500,662,75,24);
        Add(ID_RANKED_REGION,L"EDIT",LoadRankedValue(L"Region",L"global").c_str(),WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,g_contentX+575,658,(std::min)(235,(std::max)(120,g_contentWidth-585)),28);
        SectionTitle(L"MATCHMAKING",706);
        Add(ID_RANKED_HEALTH,L"BUTTON",L"CHECK SERVER",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+2,748,145,34);
        Add(ID_RANKED_PROFILE,L"BUTTON",L"MY PROFILE",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+160,748,135,34);
        Add(ID_RANKED_QUEUE,L"BUTTON",L"FIND RANKED",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+308,748,145,34);
        Add(ID_RANKED_STATUS,L"BUTTON",L"UPDATE STATUS",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+466,748,145,34);
        Add(ID_RANKED_CANCEL,L"BUTTON",L"CANCEL SEARCH",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+624,748,155,34);
        Label(L"Service responses (reservation secrets are hidden):",g_contentX+2,790,700,24,true);
        Add(ID_RANKED_RESULT,L"EDIT",L"Server connection has not been checked.",WS_BORDER|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_TABSTOP,g_contentX+2,818,(std::min)(810,g_contentWidth-4),120);
        break;
    }
    }
    ReflowPage(g_window);
    UpdateQueueCooldown();
    RedrawWindow(g_window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);
}
void ReadControls() {
    auto sel=[&](int id,int old){HWND h=GetDlgItem(g_window,id);int n=(int)SendMessageW(h,CB_GETCURSEL,0,0);return n<0?old:n;};
    if(GetDlgItem(g_window,ID_MODE)) g_settings.mode=std::clamp(sel(ID_MODE,g_settings.mode),0,2);
    if(GetDlgItem(g_window,ID_RESOLUTION)) g_settings.resolution=std::clamp(sel(ID_RESOLUTION,g_settings.resolution),0,4);
    if(GetDlgItem(g_window,ID_SCALE)) g_settings.scale=std::clamp(sel(ID_SCALE,g_settings.scale),0,5);
    if(GetDlgItem(g_window,ID_INPUT_POLL_RATE)) {
        g_settings.inputPollRateHz=kInputPollRates[std::clamp(sel(ID_INPUT_POLL_RATE,3),0,static_cast<int>(kInputPollRates.size()-1))];
    }
    if(GetDlgItem(g_window,ID_NET_POS))g_settings.netPosition=std::clamp(sel(ID_NET_POS,g_settings.netPosition),0,3);
    if(GetDlgItem(g_window,ID_OVERLAY_POS))g_settings.overlayPosition=std::clamp(sel(ID_OVERLAY_POS,g_settings.overlayPosition),0,3);
    if(GetDlgItem(g_window,ID_NET_MODE))g_settings.netMode=std::clamp(sel(ID_NET_MODE,g_settings.netMode),0,2);
    if(GetDlgItem(g_window,ID_NET_LOCAL_IP))g_settings.netLocalIp=ControlText(ID_NET_LOCAL_IP);
    if(GetDlgItem(g_window,ID_NET_PEER_IP))g_settings.netPeerIps=ControlText(ID_NET_PEER_IP);
    if(GetDlgItem(g_window,ID_PAD_ASSIGNMENT)) g_settings.padAssignment=std::clamp(sel(ID_PAD_ASSIGNMENT,g_settings.padAssignment),0,3);
    if(HWND edit=GetDlgItem(g_window,ID_SAVE_PATH)){wchar_t path[MAX_PATH]{};GetWindowTextW(edit,path,MAX_PATH);g_settings.saveRoot=path;}
}
void SetEnv(const wchar_t* name,const wchar_t* value) { SetEnvironmentVariableW(name,value&&*value?value:nullptr); }
void OpenPath(const std::wstring& path) { ShellExecuteW(g_window,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL); }
void UseDarkTitleBar(HWND hwnd) {
    const BOOL enabled = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &enabled, sizeof(enabled));
    DwmSetWindowAttribute(hwnd, 19, &enabled, sizeof(enabled));
}
std::filesystem::path SaveDirectory() {
    std::filesystem::path root=g_settings.saveRoot.empty()?std::filesystem::path(LocalRoot()+L"\\saves"):std::filesystem::path(g_settings.saveRoot);
    return root;
}
bool CopySaveTree(const std::filesystem::path& from,const std::filesystem::path& to) {
    std::error_code ec;if(!std::filesystem::exists(from,ec))return false;
    std::filesystem::create_directories(to,ec);if(ec)return false;
    for(std::filesystem::recursive_directory_iterator it(from,std::filesystem::directory_options::skip_permission_denied,ec),end;it!=end&&!ec;it.increment(ec)){
        const auto status=it->symlink_status(ec);if(ec||std::filesystem::is_symlink(status))return false;
        const auto rel=std::filesystem::relative(it->path(),from,ec);if(ec)return false;
        const auto dest=to/rel;
        if(std::filesystem::is_directory(status))std::filesystem::create_directories(dest,ec);
        else if(std::filesystem::is_regular_file(status)){std::filesystem::create_directories(dest.parent_path(),ec);if(!ec)std::filesystem::copy_file(it->path(),dest,std::filesystem::copy_options::overwrite_existing,ec);}
        if(ec)return false;
    }
    return !ec;
}
std::filesystem::path LatestBackup() {
    const auto backups=std::filesystem::path(LocalRoot()+L"\\save-backups");std::error_code ec;
    if(!std::filesystem::exists(backups,ec))return {};
    std::filesystem::path latest;for(const auto& e:std::filesystem::directory_iterator(backups,ec))if(e.is_directory()&&e.path().filename().wstring()>latest.filename().wstring())latest=e.path();
    return latest;
}
std::wstring Timestamp() { SYSTEMTIME t{};GetLocalTime(&t);wchar_t s[32]{};swprintf_s(s,L"%04u%02u%02u-%02u%02u%02u",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);return s; }
void LaunchGame(const std::string* confirmedReservation=nullptr) {
    ReadControls();SaveSettings();
    if(!confirmedReservation&&g_settings.netMode==1){
        net::Adapter adapter;const auto local=Utf8(g_settings.netLocalIp),peers=Utf8(g_settings.netPeerIps);
        if(!net::SelectAdapter(adapter,local.c_str())||!net::ValidPeerTargets(peers.c_str())){
            MessageBoxW(g_window,L"Select an active local IPv4 address (or leave it blank for automatic detection). Peer targets must be comma-separated IPv4 addresses, optionally followed by :discovery-port.",L"Check Virtual LAN settings",MB_ICONERROR|MB_OK);return;
        }
    }
    const std::wstring exe=InstallDir()+L"\\"+kGameExe;
    const std::wstring image=InstallDir()+L"\\game\\tekken.img";
    if(GetFileAttributesW(exe.c_str())==INVALID_FILE_ATTRIBUTES||GetFileAttributesW(image.c_str())==INVALID_FILE_ATTRIBUTES){
        MessageBoxW(g_window,L"The game files are missing. Run the installer again or choose the installed game folder.",
                    L"TTT2 Community Edition",MB_ICONERROR|MB_OK);return;
    }
    const std::array<const wchar_t*,6> scales{L"",L"1.0",L"1.25",L"1.5",L"2.0",L"3.0"};
    std::map<std::wstring,std::wstring,EnvCaseLess> env;
    env[L"TTT2_RENDER_SCALE"]=scales[g_settings.scale];
    env[L"TTT2_SHADER_ASYNC"]=g_settings.asyncShaders?L"1":L"0";
    env[L"TTT2_SHADER_WARM"]=g_settings.warmShaders?L"1":L"0";
    env[L"TTT2_SHADER_FAST"]=g_settings.fastShaders?L"1":L"0";
    env[L"TTT2_GPU_HW"]=g_settings.hardware?L"1":L"0";
    unsigned overlayMask=0;for(unsigned i=0;i<g_settings.overlay.size();++i)if(g_settings.overlay[i])overlayMask|=1u<<i;
    wchar_t overlayValue[24]{},hotkey[24]{};swprintf_s(overlayValue,L"%u",overlayMask);swprintf_s(hotkey,L"%d",g_settings.overlayToggleKey);
    env[L"TTT2_OVERLAY_MASK"]=overlayValue;env[L"TTT2_FRAMETIME"]=g_settings.overlay[1]?L"1":L"";env[L"TTT2_OVERLAY_GRAPH"]=g_settings.overlayGraph?L"1":L"0";
    env[L"TTT2_GPU_PROFILE"]=g_settings.drawDiagnostics?L"1":L"";
    env[L"TTT2_CONSOLE_LOGS"]=g_settings.consoleLogs?L"1":L"";
    env[L"TTT2_OVERLAY_ENABLED"]=g_settings.overlayEnabled?L"1":L"0";env[L"TTT2_OVERLAY_TOGGLE_KEY"]=hotkey;
    env[L"TTT2_SAVE_ROOT"]=g_settings.saveRoot;
    env[L"TTT2_NET_HUD"]=g_settings.netHud?L"1":L"0";wchar_t np[8]{};swprintf_s(np,L"%d",g_settings.netPosition);env[L"TTT2_NET_HUD_POSITION"]=np;wchar_t op[8]{};swprintf_s(op,L"%d",g_settings.overlayPosition);env[L"TTT2_OVERLAY_POSITION"]=op;
    env[L"TTT2_NET"]=g_settings.netMode==1?L"hamachi":g_settings.netMode==2?L"loopback":L"ipv4";
    env[L"TTT2_LOCAL_IP"]=g_settings.netMode==1?g_settings.netLocalIp:L"";
    env[L"TTT2_PEER_IP"]=g_settings.netMode==1?g_settings.netPeerIps:L"";
    env[L"TTT2_NET_ROLLBACK"]=g_settings.netMode==1&&g_settings.netRecovery?L"1":L"0";

    for(const wchar_t* name:{L"TTT2_RANKED_RUNTIME",L"TTT2_RANKED_CONFIRMED",L"TTT2_RANKED_MATCH_ID",L"TTT2_RANKED_SESSION_KEY",L"TTT2_RANKED_ROLE",L"TTT2_RANKED_PEER_ENDPOINT",L"TTT2_RANKED_RELAY_ENDPOINT",L"TTT2_RANKED_RELAY_TOKEN",L"TTT2_RANKED_MATCH_TOKEN",L"TTT2_RANKED_PLAYER",L"TTT2_RANKED_PEER_ID"})env[name]=L"";
    if(confirmedReservation){
        env[L"TTT2_NET"]=L"ipv4";env[L"TTT2_LOCAL_IP"]=L"";env[L"TTT2_PEER_IP"]=L"";env[L"TTT2_NET_ROLLBACK"]=L"0";
        std::string matchId=JsonField(*confirmedReservation,"match_id");if(matchId.empty())matchId=JsonField(*confirmedReservation,"id");
        std::string sessionKey=JsonField(*confirmedReservation,"session_key_bytes");if(sessionKey.empty())sessionKey=JsonField(*confirmedReservation,"session_key");
        const std::string role=JsonField(*confirmedReservation,"role"),endpoint=JsonField(*confirmedReservation,"opponent_endpoint");
        if(matchId.size()!=32||sessionKey.size()!=32||(role!="host"&&role!="guest")||endpoint.empty()){
            MessageBoxW(g_window,L"The match is confirmed, but the service did not return a complete session reservation.",L"Ranked match",MB_ICONERROR|MB_OK);return;
        }
        env[L"TTT2_RANKED_RUNTIME"]=L"1";env[L"TTT2_RANKED_CONFIRMED"]=L"1";
        env[L"TTT2_RANKED_MATCH_ID"]=FromUtf8(matchId);env[L"TTT2_RANKED_SESSION_KEY"]=FromUtf8(sessionKey);
        env[L"TTT2_RANKED_ROLE"]=FromUtf8(role);env[L"TTT2_RANKED_PEER_ENDPOINT"]=FromUtf8(endpoint);
        env[L"TTT2_RANKED_MATCH_TOKEN"]=FromUtf8(JsonField(*confirmedReservation,"match_token"));
        const std::string localPlayer=JsonField(*confirmedReservation,"player_a");
        const std::string otherPlayer=JsonField(*confirmedReservation,"player_b");
        const bool isHost=role=="host";
        env[L"TTT2_RANKED_PLAYER"]=FromUtf8(isHost?localPlayer:otherPlayer);
        env[L"TTT2_RANKED_PEER_ID"]=FromUtf8(isHost?otherPlayer:localPlayer);
        const std::string relayEndpoint=JsonField(*confirmedReservation,"relay_endpoint"),relayToken=JsonField(*confirmedReservation,"relay_token");
        if(!relayEndpoint.empty()&&!relayToken.empty()){env[L"TTT2_RANKED_RELAY_ENDPOINT"]=FromUtf8(relayEndpoint);env[L"TTT2_RANKED_RELAY_TOKEN"]=FromUtf8(relayToken);}
    }
    auto environment=ChildEnvironment(env);
    std::wstring command=L"\""+exe+L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(),command.end());mutableCommand.push_back(L'\0');
    STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
    if(!CreateProcessW(exe.c_str(),mutableCommand.data(),nullptr,nullptr,FALSE,CREATE_UNICODE_ENVIRONMENT,environment.data(),InstallDir().c_str(),&si,&pi)){
        MessageBoxW(g_window,L"Windows could not start the game. Check the game folder and permissions.",
                    L"TTT2 Community Edition",MB_ICONERROR|MB_OK);return;
    }
    CloseHandle(pi.hThread);
    if(g_settings.closeOnLaunch){CloseHandle(pi.hProcess);PostQuitMessage(0);return;}
    if(g_gameProcess)CloseHandle(g_gameProcess);
    g_gameProcess=pi.hProcess;EnableWindow(g_play,FALSE);
    SetWindowTextW(g_status,confirmedReservation?L"Ranked match launched.":L"Game is running");
    HANDLE waitHandle=nullptr;
    if(DuplicateHandle(GetCurrentProcess(),g_gameProcess,GetCurrentProcess(),&waitHandle,0,FALSE,DUPLICATE_SAME_ACCESS))
        std::thread([handle=waitHandle]{WaitForSingleObject(handle,INFINITE);CloseHandle(handle);PostMessageW(g_window,WM_APP+5,0,0);}).detach();
}
void ChooseSaveFolder() {
    BROWSEINFOW b{};b.hwndOwner=g_window;b.lpszTitle=L"Choose a save folder";b.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE id=SHBrowseForFolderW(&b);if(!id)return;
    wchar_t path[MAX_PATH]{};if(SHGetPathFromIDListW(id,path))SetWindowTextW(GetDlgItem(g_window,ID_SAVE_PATH),path);
    CoTaskMemFree(id);
}
LRESULT CALLBACK Proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg){
    case WM_CREATE: {
        g_window=hwnd;
        UseDarkTitleBar(hwnd);
        g_bodyFont=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        g_headingFont=CreateFontW(-27,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI Variable Display");
        g_sectionFont=CreateFontW(-16,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        g_smallFont=CreateFontW(-13,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        g_darkBrush=CreateSolidBrush(kBg);g_panelBrush=CreateSolidBrush(kPanel);
        LoadSettings();
        const std::array<const wchar_t*,8> names{L"OVERVIEW",L"DISPLAY",L"PERFORMANCE",L"SAVES",L"CONTROLS",L"ONLINE",L"VPN LAN",L"ABOUT"};
        const std::array<int,8> ids{ID_HOME,ID_GRAPHICS,ID_PERFORMANCE,ID_GAME,ID_CONTROLS,ID_ONLINE,ID_VPNLAN,ID_ADVANCED};
        for(size_t i=0;i<names.size();++i){g_nav[i]=CreateWindowExW(0,L"BUTTON",names[i],WS_CHILD|WS_VISIBLE|BS_OWNERDRAW|WS_TABSTOP,0,0,0,0,hwnd,(HMENU)(INT_PTR)ids[i],GetModuleHandleW(nullptr),nullptr);}
        g_status=CreateWindowExW(0,L"STATIC",L"Ready / settings are saved on this PC",WS_CHILD|WS_VISIBLE|SS_LEFT,34,0,520,26,hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
        SendMessageW(g_status,WM_SETFONT,(WPARAM)g_smallFont,TRUE);
        HWND save=CreateWindowExW(0,L"BUTTON",L"SAVE",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW|WS_TABSTOP,0,0,0,0,hwnd,(HMENU)ID_SAVE,GetModuleHandleW(nullptr),nullptr);
        g_play=CreateWindowExW(0,L"BUTTON",L"LAUNCH GAME",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW|WS_TABSTOP,0,0,0,0,hwnd,(HMENU)ID_PLAY,GetModuleHandleW(nullptr),nullptr);
        (void)save;BuildPage();return 0;
    }
    case WM_SIZE: {
        const int w=LOWORD(lp),h=HIWORD(lp),footer=h-kFooterHeight;
        UpdateContentGeometry(w);
        const int navWidth=(w-68)/8;
        for(size_t i=0;i<g_nav.size();++i)MoveWindow(g_nav[i],34+int(i)*navWidth,80,navWidth-8,38,TRUE);
        MoveWindow(g_status,34,footer+24,460,26,TRUE);
        MoveWindow(GetDlgItem(hwnd,ID_SAVE),w-330,footer+18,120,38,TRUE);
        MoveWindow(g_play,w-190,footer+15,156,44,TRUE);
        ReadControls();BuildPage();
        if(g_challengePanel){LockRankedChallenge(true);LayoutChallenge();}
        return 0;
    }
    case WM_TIMER: {
        if(wp==ID_STATUS_TIMER){
            if(g_rankedSearching&&!g_rankedBusy.load()){
                const auto now=std::chrono::steady_clock::now();
                const int operation=g_queueLeaseRenew&&now-g_lastQueueRenew>=std::chrono::seconds(10)?ID_RANKED_QUEUE:ID_RANKED_STATUS;
                StartRankedRequest(operation);
            }
            return 0;
        }
        if(wp==ID_COOLDOWN_TIMER){UpdateQueueCooldown();return 0;}
        if(wp==ID_CHALLENGE_TIMER&&g_challengePanel){
            const double remaining=std::max(0.0,std::chrono::duration<double>(g_acceptDeadline-std::chrono::steady_clock::now()).count());
            wchar_t label[80]{};swprintf_s(label,L"Time remaining: %.1f seconds",remaining);
            if(g_challengeTime)SetWindowTextW(g_challengeTime,label);
            if(g_challengeProgress)SendMessageW(g_challengeProgress,PBM_SETPOS,WPARAM(remaining*100.0),0);
            if(remaining<=0.0&&!g_acceptSubmitted&&!g_rankedBusy){
                g_acceptSubmitted=true;LockRankedChallenge(true);
                SetWindowTextW(g_challengeAccept,L"DECLINING...");
                StartRankedAsync(L"/v1/matches/"+g_pendingMatchId+L"/decline","{}",ID_RANKED_DECLINE);
            }
            InvalidateRect(g_window,nullptr,FALSE);return 0;
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        const int maxOffset=(std::max)(0,g_pageBottom-g_viewportBottom);
        const int delta=GET_WHEEL_DELTA_WPARAM(wp);
        g_scrollOffset=std::clamp(g_scrollOffset-(delta/WHEEL_DELTA)*48,0,maxOffset);
        ReflowPage(hwnd);return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO si{sizeof(si),SIF_ALL};GetScrollInfo(hwnd,SB_VERT,&si);
        const int maxOffset=(std::max)(0,g_pageBottom-g_viewportBottom);
        switch(LOWORD(wp)) {
        case SB_LINEUP: si.nPos-=36;break;
        case SB_LINEDOWN: si.nPos+=36;break;
        case SB_PAGEUP: si.nPos-=int(si.nPage);break;
        case SB_PAGEDOWN: si.nPos+=int(si.nPage);break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: si.nPos=si.nTrackPos;break;
        case SB_TOP: si.nPos=0;break;
        case SB_BOTTOM: si.nPos=maxOffset;break;
        default:return 0;
        }
        g_scrollOffset=std::clamp(si.nPos,0,maxOffset);ReflowPage(hwnd);return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};HDC target=BeginPaint(hwnd,&paint);
        RECT bounds{};GetClientRect(hwnd,&bounds);
        HDC buffer=CreateCompatibleDC(target);
        HBITMAP bitmap=CreateCompatibleBitmap(target,(std::max)(1L,bounds.right),(std::max)(1L,bounds.bottom));
        if(buffer&&bitmap){HGDIOBJ oldBitmap=SelectObject(buffer,bitmap);
            HBRUSH bg=CreateSolidBrush(kBg);FillRect(buffer,&bounds,bg);DeleteObject(bg);
            RECT top{0,0,bounds.right,136};HBRUSH sb=CreateSolidBrush(kSidebar);FillRect(buffer,&top,sb);DeleteObject(sb);
            RECT foot{0,bounds.bottom-kFooterHeight,bounds.right,bounds.bottom};HBRUSH fb=CreateSolidBrush(kSidebar);FillRect(buffer,&foot,fb);DeleteObject(fb);
            HPEN pen=CreatePen(PS_SOLID,1,kLine);auto oldPen=SelectObject(buffer,pen);MoveToEx(buffer,0,foot.top,nullptr);LineTo(buffer,bounds.right,foot.top);SelectObject(buffer,oldPen);DeleteObject(pen);
            SetBkMode(buffer,TRANSPARENT);SetTextColor(buffer,kCream);
            HFONT logo=CreateFontW(-22,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI Variable Display");
            auto oldFont=SelectObject(buffer,logo);RECT lr{34,12,500,44};DrawTextW(buffer,L"TEKKEN TAG TOURNAMENT 2",-1,&lr,DT_LEFT|DT_TOP|DT_NOCLIP);SelectObject(buffer,oldFont);DeleteObject(logo);
            HFONT sub=CreateFontW(-11,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
            oldFont=SelectObject(buffer,sub);SetTextColor(buffer,kMuted);RECT sr{36,47,390,66};DrawTextW(buffer,L"COMMUNITY EDITION  /  SHEEDA MUSALLI",-1,&sr,DT_LEFT|DT_TOP|DT_NOCLIP);SelectObject(buffer,oldFont);DeleteObject(sub);
            HPEN streak=CreatePen(PS_SOLID,3,kRed);HGDIOBJ oldStreak=SelectObject(buffer,streak);MoveToEx(buffer,bounds.right-142,12,nullptr);LineTo(buffer,bounds.right-70,70);SelectObject(buffer,oldStreak);DeleteObject(streak);
            streak=CreatePen(PS_SOLID,2,kBlue);oldStreak=SelectObject(buffer,streak);MoveToEx(buffer,bounds.right-112,10,nullptr);LineTo(buffer,bounds.right-42,68);SelectObject(buffer,oldStreak);DeleteObject(streak);
            BitBlt(target,paint.rcPaint.left,paint.rcPaint.top,paint.rcPaint.right-paint.rcPaint.left,paint.rcPaint.bottom-paint.rcPaint.top,buffer,paint.rcPaint.left,paint.rcPaint.top,SRCCOPY);
            SelectObject(buffer,oldBitmap);
        }
        else FillRect(target,&paint.rcPaint,(HBRUSH)GetStockObject(BLACK_BRUSH));
        if(bitmap)DeleteObject(bitmap);if(buffer)DeleteDC(buffer);EndPaint(hwnd,&paint);return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc=(HDC)wp;HWND c=(HWND)lp;SetBkMode(dc,OPAQUE);
        if(c==g_challengePanel){SetBkColor(dc,kPanel);return (LRESULT)g_panelBrush;}
        if(c==g_challengeTitle||c==g_challengeOpponent||c==g_challengeRating||c==g_challengeDisconnect||c==g_challengeTime){
            SetBkMode(dc,OPAQUE);SetBkColor(dc,kPanel);SetTextColor(dc,c==g_challengeTitle?kRed:kCream);return (LRESULT)g_panelBrush;
        }
        SetBkMode(dc,OPAQUE);SetBkColor(dc,c==g_status?kSidebar:kBg);
        if((INT_PTR)GetPropW(c,L"ttt2.muted")){SetTextColor(dc,kMuted);return (LRESULT)g_darkBrush;}
        wchar_t text[64]{};GetWindowTextW(c,text,64);
        if(c==g_status){SetTextColor(dc,kMuted);return (LRESULT)g_darkBrush;}
        if((INT_PTR)GetPropW(c,L"ttt2.title")){SetTextColor(dc,kCream);return (LRESULT)g_darkBrush;}
        if(wcsstr(text,L"GRAPHICS")||wcsstr(text,L"PERFORMANCE")||wcsstr(text,L"GAME")||wcsstr(text,L"ADVANCED")||wcsstr(text,L"HOME")||wcsstr(text,L"CONTROLS")||wcsstr(text,L"ONLINE"))SetTextColor(dc,kRed);else SetTextColor(dc,kCream);
        return (LRESULT)g_darkBrush;
    }
    case WM_CTLCOLORBTN: {HDC dc=(HDC)wp;SetTextColor(dc,kCream);SetBkColor(dc,kPanel);return (LRESULT)g_panelBrush;}
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: {HDC dc=(HDC)wp;SetTextColor(dc,kCream);SetBkColor(dc,kPanel);return (LRESULT)g_panelBrush;}
    case WM_DRAWITEM: {
        auto* d=(DRAWITEMSTRUCT*)lp;const int id=(int)d->CtlID;
        if(d->CtlType==ODT_STATIC){HPEN p=CreatePen(PS_SOLID,1,kLine);auto old=(HPEN)SelectObject(d->hDC,p);MoveToEx(d->hDC,d->rcItem.left,d->rcItem.top,nullptr);LineTo(d->hDC,d->rcItem.right,d->rcItem.top);SelectObject(d->hDC,old);DeleteObject(p);return TRUE;}
        const bool keyBinding=(id>=ID_KEY_FIRST&&id<ID_KEY_FIRST+24)||(id>=ID_KEY2_FIRST&&id<ID_KEY2_FIRST+24)||id==ID_OVERLAY_HOTKEY;
        if(keyBinding){const bool waiting=id==ID_OVERLAY_HOTKEY?g_capturingOverlayKey:g_capturingKey==((id>=ID_KEY2_FIRST)?id-ID_KEY2_FIRST:id-ID_KEY_FIRST);HBRUSH fill=CreateSolidBrush(waiting?RGB(53,47,43):kPanel);FillRect(d->hDC,&d->rcItem,fill);DeleteObject(fill);HPEN edge=CreatePen(PS_SOLID,(d->itemState&ODS_FOCUS)?2:1,waiting?kRed:kLine);HGDIOBJ oldPen=SelectObject(d->hDC,edge);HGDIOBJ oldBrush=SelectObject(d->hDC,GetStockObject(NULL_BRUSH));RoundRect(d->hDC,d->rcItem.left,d->rcItem.top,d->rcItem.right,d->rcItem.bottom,5,5);SelectObject(d->hDC,oldBrush);SelectObject(d->hDC,oldPen);DeleteObject(edge);RECT text=d->rcItem;text.left+=10;text.right-=28;SetBkMode(d->hDC,TRANSPARENT);SetTextColor(d->hDC,waiting?kRed:kCream);wchar_t value[80]{};GetWindowTextW(d->hwndItem,value,80);DrawTextW(d->hDC,value,-1,&text,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);const int cx=d->rcItem.right-15,cy=(d->rcItem.top+d->rcItem.bottom)/2;HPEN glyph=CreatePen(PS_SOLID,2,kMuted);oldPen=SelectObject(d->hDC,glyph);MoveToEx(d->hDC,cx-3,cy-4,nullptr);LineTo(d->hDC,cx+1,cy);LineTo(d->hDC,cx-3,cy+4);SelectObject(d->hDC,oldPen);DeleteObject(glyph);return TRUE;}
        const bool overlayCheck=id>=ID_OVERLAY_FIRST&&id<ID_OVERLAY_FIRST+8;
        const bool checkbox=id==ID_ASYNC||id==ID_WARM||id==ID_FAST||id==ID_DRAW_DIAGNOSTICS||id==ID_CONSOLE_LOGS||id==ID_HARDWARE||id==ID_CLOSE_ON_LAUNCH||id==ID_PLAYER2_ENABLED||id==ID_OVERLAY_ENABLE||id==ID_NET_HUD||id==ID_NET_RECOVERY||id==ID_OVERLAY_GRAPH||overlayCheck;
        if(checkbox){const bool checked=id==ID_NET_HUD?g_settings.netHud:id==ID_NET_RECOVERY?g_settings.netRecovery:id==ID_OVERLAY_GRAPH?g_settings.overlayGraph:id==ID_PLAYER2_ENABLED?g_settings.player2KeyboardEnabled:id==ID_OVERLAY_ENABLE?g_settings.overlayEnabled:overlayCheck?g_settings.overlay[id-ID_OVERLAY_FIRST]:id==ID_ASYNC?g_settings.asyncShaders:id==ID_WARM?g_settings.warmShaders:id==ID_FAST?g_settings.fastShaders:id==ID_DRAW_DIAGNOSTICS?g_settings.drawDiagnostics:id==ID_CONSOLE_LOGS?g_settings.consoleLogs:id==ID_HARDWARE?g_settings.hardware:g_settings.closeOnLaunch;FillRect(d->hDC,&d->rcItem,g_darkBrush);RECT box{d->rcItem.left,d->rcItem.top+6,d->rcItem.left+16,d->rcItem.top+22};HBRUSH b=CreateSolidBrush(checked?kRed:kPanel);FillRect(d->hDC,&box,b);DeleteObject(b);HBRUSH edge=CreateSolidBrush(kLine);FrameRect(d->hDC,&box,edge);DeleteObject(edge);if(checked){HPEN p=CreatePen(PS_SOLID,2,kCream);auto old=(HPEN)SelectObject(d->hDC,p);MoveToEx(d->hDC,box.left+3,box.top+8,nullptr);LineTo(d->hDC,box.left+7,box.top+12);LineTo(d->hDC,box.left+13,box.top+4);SelectObject(d->hDC,old);DeleteObject(p);}RECT tx=d->rcItem;tx.left+=25;SetBkMode(d->hDC,TRANSPARENT);SetTextColor(d->hDC,kCream);wchar_t label[128]{};GetWindowTextW(d->hwndItem,label,128);DrawTextW(d->hDC,label,-1,&tx,DT_LEFT|DT_VCENTER|DT_SINGLELINE);return TRUE;}
        int pageIndex=-1;switch(id){case ID_HOME:pageIndex=0;break;case ID_GRAPHICS:pageIndex=1;break;case ID_PERFORMANCE:pageIndex=2;break;case ID_GAME:pageIndex=3;break;case ID_ADVANCED:pageIndex=4;break;case ID_CONTROLS:pageIndex=5;break;case ID_ONLINE:pageIndex=6;break;}const bool active=pageIndex>=0&&int(g_tab)==pageIndex;
        const bool play=id==ID_PLAY;const bool challengeAccept=id==ID_RANKED_ACCEPT;const bool challengeDecline=id==ID_RANKED_DECLINE;const bool controlTab=id==ID_CONTROL_PLAYER1||id==ID_CONTROL_PLAYER2;const bool selectedControl=id==ID_CONTROL_PLAYER1?g_controlPlayer==0:id==ID_CONTROL_PLAYER2&&g_controlPlayer==1;const bool primary=play||challengeAccept;const COLORREF fill=challengeAccept?RGB(39,119,95):((active||(controlTab&&selectedControl))?RGB(47,34,39):kPanel);HBRUSH brush=CreateSolidBrush(fill);
        HPEN p=CreatePen(PS_SOLID,1,primary?kRed:kLine);auto oldPen=(HPEN)SelectObject(d->hDC,p);auto oldBrush=(HBRUSH)SelectObject(d->hDC,brush);RoundRect(d->hDC,d->rcItem.left,d->rcItem.top,d->rcItem.right,d->rcItem.bottom,8,8);SelectObject(d->hDC,oldBrush);SelectObject(d->hDC,oldPen);DeleteObject(p);DeleteObject(brush);
        SetBkMode(d->hDC,TRANSPARENT);SetTextColor(d->hDC,(active||play||challengeAccept||challengeDecline||(controlTab&&selectedControl))?kCream:kMuted);
        HFONT f=(play||id==ID_SAVE)?g_bodyFont:g_smallFont;auto old=(HFONT)SelectObject(d->hDC,f);
        wchar_t text[128]{};GetWindowTextW(d->hwndItem,text,128);DrawTextW(d->hDC,text,-1,&d->rcItem,DT_CENTER|DT_VCENTER|DT_SINGLELINE);SelectObject(d->hDC,old);
        if(d->itemState&ODS_FOCUS){RECT r=d->rcItem;InflateRect(&r,-4,-4);DrawFocusRect(d->hDC,&r);}return TRUE;
    }
    case WM_COMMAND: {
        const int id=LOWORD(wp);
        if(id>=ID_HOME&&id<=ID_VPNLAN){ReadControls();if(g_tab==Online)SaveRankedSettings();g_tab=(Tab)(id-ID_HOME);g_scrollOffset=0;for(size_t i=0;i<g_nav.size();++i)InvalidateRect(g_nav[i],nullptr,TRUE);BuildPage();return 0;}
        if(id==ID_SAVE){ReadControls();SaveSettings();if(g_tab==Online)SaveRankedSettings();SetWindowTextW(g_status,L"Settings saved");return 0;}
        if(id==ID_PLAY){LaunchGame();return 0;}
        if(id==ID_NET_HUD||id==ID_NET_RECOVERY||id==ID_OVERLAY_GRAPH){if(id==ID_NET_HUD)g_settings.netHud=!g_settings.netHud;else if(id==ID_NET_RECOVERY)g_settings.netRecovery=!g_settings.netRecovery;else g_settings.overlayGraph=!g_settings.overlayGraph;InvalidateRect(GetDlgItem(hwnd,id),nullptr,TRUE);return 0;}
        if(id==ID_NET_REFRESH){ReadControls();BuildPage();return 0;}
        if(id==ID_NET_MODE&&HIWORD(wp)==CBN_SELCHANGE){ReadControls();return 0;}

        if(id==ID_CONTROL_PLAYER1||id==ID_CONTROL_PLAYER2){g_capturingKey=-1;g_controlPlayer=id==ID_CONTROL_PLAYER1?0:1;g_scrollOffset=0;BuildPage();return 0;}
        if(id==ID_PLAYER2_ENABLED){g_settings.player2KeyboardEnabled=!g_settings.player2KeyboardEnabled;InvalidateRect(GetDlgItem(hwnd,id),nullptr,TRUE);return 0;}
        if(id==ID_PAD_ASSIGNMENT&&HIWORD(wp)==CBN_SELCHANGE){g_settings.padAssignment=std::clamp(int(SendMessageW(GetDlgItem(hwnd,id),CB_GETCURSEL,0,0)),0,3);SetWindowTextW(g_status,L"Device assignment changed. Save before launching.");return 0;}
        if(id==ID_OVERLAY_ENABLE){g_settings.overlayEnabled=!g_settings.overlayEnabled;InvalidateRect(GetDlgItem(hwnd,id),nullptr,TRUE);return 0;}
        if(id==ID_OVERLAY_HOTKEY){g_capturingOverlayKey=true;SetWindowTextW(GetDlgItem(hwnd,id),L"PRESS A KEY...");SetFocus(GetDlgItem(hwnd,id));InvalidateRect(GetDlgItem(hwnd,id),nullptr,TRUE);SetWindowTextW(g_status,L"Press a key to toggle the performance overlay. Escape cancels.");return 0;}
        if((id>=ID_KEY_FIRST&&id<ID_KEY_FIRST+24)||(id>=ID_KEY2_FIRST&&id<ID_KEY2_FIRST+24)){g_controlPlayer=id>=ID_KEY2_FIRST?1:0;g_capturingKey=id-(g_controlPlayer?ID_KEY2_FIRST:ID_KEY_FIRST);SetWindowTextW(GetDlgItem(hwnd,id),L"PRESS A KEY...");SetFocus(GetDlgItem(hwnd,id));InvalidateRect(GetDlgItem(hwnd,id),nullptr,TRUE);SetWindowTextW(g_status,L"Press a key for this binding. Escape is reserved.");return 0;}
        if(id==ID_RANKED_ACCEPT||id==ID_RANKED_DECLINE){
            if(!g_challengePanel||g_pendingMatchId.empty()||g_acceptSubmitted)return 0;
            g_acceptSubmitted=true;LockRankedChallenge(true);
            if(id==ID_RANKED_ACCEPT){SetWindowTextW(g_challengeAccept,L"WAITING FOR OPPONENT...");SetWindowTextW(g_status,L"Accepted. Waiting for the other player.");
                StartRankedAsync(L"/v1/matches/"+g_pendingMatchId+L"/accept","{}",ID_RANKED_ACCEPT);
            }else{SetWindowTextW(g_challengeDecline,L"DECLINING...");
                StartRankedAsync(L"/v1/matches/"+g_pendingMatchId+L"/decline","{}",ID_RANKED_DECLINE);}
            return 0;
        }
        if(id==ID_RANKED_REGISTER){StartRankedRegistration();return 0;}
        if(id==ID_RANKED_HEALTH||id==ID_RANKED_PROFILE||id==ID_RANKED_QUEUE||id==ID_RANKED_STATUS||id==ID_RANKED_CANCEL){StartRankedRequest(id);return 0;}
        if(id==ID_BROWSE){ChooseSaveFolder();return 0;}
        if(id==ID_OPEN_SAVES){ReadControls();OpenPath(g_settings.saveRoot.empty()?LocalRoot()+L"\\saves":g_settings.saveRoot);return 0;}
        if(id==ID_OPEN_INSTALL){OpenPath(InstallDir());return 0;}
        if(id==ID_OPEN_LOG){OpenPath(LocalRoot()+L"\\log.txt");return 0;}
        if(id==ID_OPEN_CONFIG){OpenPath(LocalRoot());return 0;}
        if(id==ID_SAVE_BACKUP){ReadControls();const auto dst=std::filesystem::path(LocalRoot()+L"\\save-backups\\"+Timestamp());const bool ok=CopySaveTree(SaveDirectory(),dst);MessageBoxW(hwnd,ok?L"Save backup created.":L"No save files were found, or the backup could not be written.",L"Save backup",ok?MB_ICONINFORMATION:MB_ICONWARNING);return 0;}
        if(id==ID_SAVE_RESTORE){ReadControls();const auto src=LatestBackup();if(src.empty()){MessageBoxW(hwnd,L"There are no save backups to restore.",L"Restore saves",MB_ICONINFORMATION);return 0;}if(MessageBoxW(hwnd,L"Restore the latest backup over the current save files? A safety copy of current files will be made first when they exist.",L"Restore saves",MB_ICONWARNING|MB_YESNO)!=IDYES)return 0;const auto current=SaveDirectory();std::error_code ec;const bool hasCurrent=std::filesystem::exists(current,ec);const auto safety=std::filesystem::path(LocalRoot()+L"\\save-backups\\before-restore-"+Timestamp());const bool safe=!hasCurrent||CopySaveTree(current,safety);const bool ok=safe&&CopySaveTree(src,current);MessageBoxW(hwnd,ok?L"The latest save backup was restored.":L"Restore failed. Your safety backup may be available in the save-backups folder.",L"Restore saves",ok?MB_ICONINFORMATION:MB_ICONERROR);return 0;}
        if(id==ID_VERIFY){const auto exe=InstallDir()+L"\\"+kGameExe;const auto image=InstallDir()+L"\\game\\tekken.img";const bool ok=GetFileAttributesW(exe.c_str())!=INVALID_FILE_ATTRIBUTES&&GetFileAttributesW(image.c_str())!=INVALID_FILE_ATTRIBUTES;MessageBoxW(hwnd,ok?L"The game executable and installed image were found.":L"One or more required game files are missing. Run the installer again.",L"Check install",ok?MB_ICONINFORMATION:MB_ICONWARNING);return 0;}
        if(id==ID_MODE||id==ID_RESOLUTION||id==ID_SCALE||id==ID_INPUT_POLL_RATE){
            if(HIWORD(wp)==CBN_SELCHANGE)ReadControls();
            return 0;
        }
        if(id>=ID_OVERLAY_FIRST&&id<ID_OVERLAY_FIRST+8){g_settings.overlay[id-ID_OVERLAY_FIRST]=!g_settings.overlay[id-ID_OVERLAY_FIRST];InvalidateRect(GetDlgItem(hwnd,id),nullptr,TRUE);return 0;}
        if(id==ID_ASYNC||id==ID_WARM||id==ID_FAST||id==ID_DRAW_DIAGNOSTICS||id==ID_CONSOLE_LOGS||id==ID_HARDWARE||id==ID_CLOSE_ON_LAUNCH){if(id==ID_ASYNC)g_settings.asyncShaders=!g_settings.asyncShaders;else if(id==ID_WARM)g_settings.warmShaders=!g_settings.warmShaders;else if(id==ID_FAST)g_settings.fastShaders=!g_settings.fastShaders;else if(id==ID_DRAW_DIAGNOSTICS)g_settings.drawDiagnostics=!g_settings.drawDiagnostics;else if(id==ID_CONSOLE_LOGS)g_settings.consoleLogs=!g_settings.consoleLogs;else if(id==ID_HARDWARE)g_settings.hardware=!g_settings.hardware;else g_settings.closeOnLaunch=!g_settings.closeOnLaunch;InvalidateRect(GetDlgItem(hwnd,id),nullptr,TRUE);return 0;}
        break;
    }
    case WM_APP+6: {
        std::unique_ptr<std::wstring> result(reinterpret_cast<std::wstring*>(lp));
        const int action=int(wp);const std::string body=RankedBody(*result),state=JsonField(body,"state");
        if(GetDlgItem(hwnd,ID_RANKED_RESULT)){const std::wstring display=SafeRankedDisplay(*result);SetWindowTextW(GetDlgItem(hwnd,ID_RANKED_RESULT),display.c_str());}
        if(!g_challengePanel)for(int id:{ID_RANKED_HEALTH,ID_RANKED_REGISTER,ID_RANKED_PROFILE,ID_RANKED_QUEUE,ID_RANKED_STATUS,ID_RANKED_CANCEL})if(HWND h=GetDlgItem(hwnd,id))EnableWindow(h,TRUE);
        const bool validating=g_registrationValidation;g_registrationValidation=false;
        if(validating){SetWindowTextW(g_status,result->rfind(L"HTTP 200",0)==0?L"Token successfully generated and saved!":L"Token saved, but server validation failed.");g_rankedBusy=false;return 0;}
        if(action==ID_RANKED_ACCEPT&&state.empty()){
            g_acceptSubmitted=false;LockRankedChallenge(true);
            SetWindowTextW(g_status,L"Acceptance could not be confirmed. Checking status; you can retry.");
            g_rankedBusy=false;return 0;
        }
        if(action==ID_RANKED_DECLINE&&state!="declined"){
            if(g_closeAfterDecline){g_closeAfterDecline=false;g_rankedBusy=false;DestroyWindow(hwnd);return 0;}
            g_acceptSubmitted=false;LockRankedChallenge(true);
            SetWindowTextW(g_status,L"Could not confirm the decline. Checking the match status...");
            g_rankedBusy=false;return 0;
        }
        if(action==ID_RANKED_DECLINE){
            g_queueCooldownUntil=std::chrono::steady_clock::now()+std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(JsonNumber(body,"cooldown",30.0)));
            CloseChallenge();g_rankedSearching=false;KillTimer(hwnd,ID_STATUS_TIMER);
            UpdateQueueCooldown();
            SetWindowTextW(g_status,L"Match declined. 30-second matchmaking cooldown active.");
            if(g_closeAfterDecline){g_closeAfterDecline=false;g_rankedBusy=false;DestroyWindow(hwnd);return 0;}
        }else if(state=="pending_acceptance"){
            g_queueLeaseRenew=false;
            ShowChallenge(body,!g_challengePanel);
            SetWindowTextW(g_status,g_acceptSubmitted?L"Accepted. Waiting for the other player.":L"Challenger found. Accept or decline before the timer expires.");
        }else if(state=="confirmed"){
            g_queueLeaseRenew=false;
            CloseChallenge();g_rankedSearching=false;KillTimer(hwnd,ID_STATUS_TIMER);
            if(JsonField(body,"session_key_bytes").empty()||JsonField(body,"opponent_endpoint").empty()||JsonField(body,"role").empty()){
                SetWindowTextW(g_status,L"Match confirmed. Retrieving session details...");
                g_rankedBusy=false;StartRankedAsync(L"/v1/status","{}",ID_RANKED_STATUS);return 0;
            }
            SetWindowTextW(g_status,L"Match confirmed! Launching Tekken Tag Tournament 2...");
            g_rankedBusy=false;LaunchGame(&body);return 0;
        }else if(state=="queued"){
            g_queueLeaseRenew=true;
            if(g_challengePanel){CloseChallenge();SetWindowTextW(g_status,L"Opponent declined or timed out. You are back in the queue.");}
            else if(action==ID_RANKED_QUEUE)SetWindowTextW(g_status,L"Searching for an opponent...");
        }else if(state=="cooldown"){
            g_queueLeaseRenew=false;
            g_queueCooldownUntil=std::chrono::steady_clock::now()+std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(JsonNumber(body,"time_remaining",30.0)));
            CloseChallenge();g_rankedSearching=false;KillTimer(hwnd,ID_STATUS_TIMER);
            UpdateQueueCooldown();
            SetWindowTextW(g_status,L"Match declined or expired. A short queue cooldown is active.");
        }else if(state=="idle"&&action==ID_RANKED_STATUS){
            g_queueLeaseRenew=false;
            g_rankedSearching=false;KillTimer(hwnd,ID_STATUS_TIMER);SetWindowTextW(g_status,L"No active search or match.");
        }else if(action==ID_RANKED_QUEUE&&state.empty()){
            g_rankedSearching=false;KillTimer(hwnd,ID_STATUS_TIMER);SetWindowTextW(g_status,L"Ranked queue request failed. See the service response.");
        }else if(action==ID_RANKED_STATUS&&state.empty()){
            g_rankedSearching=false;KillTimer(hwnd,ID_STATUS_TIMER);SetWindowTextW(g_status,L"Ranked status request failed. See the service response.");
        }else SetWindowTextW(g_status,L"Ranked service response received");
        if(g_closeAfterDecline&&!g_challengePanel){g_closeAfterDecline=false;g_rankedBusy=false;DestroyWindow(hwnd);return 0;}
        if(g_closeAfterDecline&&action!=ID_RANKED_DECLINE&&g_challengePanel){
            g_rankedBusy=false;g_acceptSubmitted=true;
            StartRankedAsync(L"/v1/matches/"+g_pendingMatchId+L"/decline","{}",ID_RANKED_DECLINE);return 0;
        }
        if(g_closeAfterDecline&&action==ID_RANKED_DECLINE){g_closeAfterDecline=false;g_rankedBusy=false;DestroyWindow(hwnd);return 0;}
        g_rankedBusy=false;return 0;
    }
    case WM_RANKED_REGISTER_COMPLETE: {
        std::unique_ptr<RankedRegistrationResult> result(reinterpret_cast<RankedRegistrationResult*>(lp));
        const std::wstring token=RegistrationToken(result->response);
        if(token.empty()){
            if(GetDlgItem(hwnd,ID_RANKED_RESULT))SetWindowTextW(GetDlgItem(hwnd,ID_RANKED_RESULT),result->response.c_str());
            for(int id:{ID_RANKED_HEALTH,ID_RANKED_REGISTER,ID_RANKED_PROFILE,ID_RANKED_QUEUE,ID_RANKED_STATUS,ID_RANKED_CANCEL})if(HWND h=GetDlgItem(hwnd,id))EnableWindow(h,TRUE);
            SetWindowTextW(g_status,L"Token registration failed. Check the service status.");g_rankedBusy=false;return 0;
        }
        SetWindowTextW(GetDlgItem(hwnd,ID_RANKED_TOKEN),token.c_str());
        SaveRankedSettings();
        g_registrationValidation=true;g_rankedBusy=false;
        StartRankedRequest(ID_RANKED_PROFILE);
        return 0;
    }
    case WM_APP+5: if(g_gameProcess){CloseHandle(g_gameProcess);g_gameProcess=nullptr;EnableWindow(g_play,TRUE);SetWindowTextW(g_status,L"Game closed");}return 0;
    case WM_CLOSE:
        if(g_challengePanel){
            g_closeAfterDecline=true;
            if(!g_rankedBusy){g_acceptSubmitted=true;LockRankedChallenge(true);
                StartRankedAsync(L"/v1/matches/"+g_pendingMatchId+L"/decline","{}",ID_RANKED_DECLINE);}
            return 0;
        }
        DestroyWindow(hwnd);return 0;
    case WM_GETMINMAXINFO: {auto* m=(MINMAXINFO*)lp;m->ptMinTrackSize.x=900;m->ptMinTrackSize.y=620;return 0;}
    case WM_DESTROY:
        if(g_tab==Online)SaveRankedSettings();
        if(g_gameProcess)CloseHandle(g_gameProcess);
        if(g_bodyFont)DeleteObject(g_bodyFont);if(g_headingFont)DeleteObject(g_headingFont);if(g_sectionFont)DeleteObject(g_sectionFont);if(g_smallFont)DeleteObject(g_smallFont);
        if(g_darkBrush)DeleteObject(g_darkBrush);if(g_panelBrush)DeleteObject(g_panelBrush);
        PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show) {
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
    WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=Proc;wc.hInstance=instance;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));
    wc.hIconSm=wc.hIcon;
    wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);wc.lpszClassName=kWindowClass;
    if(!RegisterClassExW(&wc))return 1;
    HWND hwnd=CreateWindowExW(0,kWindowClass,L"Tekken Tag Tournament 2 | Community Edition",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
        CW_USEDEFAULT,CW_USEDEFAULT,1120,760,nullptr,nullptr,instance,nullptr);
    if(!hwnd)return 2;
    ShowWindow(hwnd,show);UpdateWindow(hwnd);
    MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    return (int)msg.wParam;
}
