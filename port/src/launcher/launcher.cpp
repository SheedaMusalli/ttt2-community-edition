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
#include <cwchar>
#include <filesystem>
#include <memory>
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
enum : int { ID_HOME=100, ID_GRAPHICS, ID_PERFORMANCE, ID_GAME, ID_ADVANCED, ID_CONTROLS, ID_ONLINE,
             ID_SAVE=180, ID_PLAY, ID_MODE=200, ID_RESOLUTION, ID_SCALE,
             ID_ASYNC=210, ID_WARM, ID_FAST, ID_HARDWARE, ID_INPUT_POLL_RATE,
             ID_SAVE_PATH=220, ID_BROWSE, ID_OPEN_SAVES,
             ID_OPEN_INSTALL=230, ID_OPEN_LOG, ID_VERIFY, ID_SAVE_BACKUP=240, ID_OPEN_CONFIG, ID_SAVE_RESTORE,
             ID_CLOSE_ON_LAUNCH=250, ID_OVERLAY_FIRST=260, ID_KEY_FIRST=300, ID_RANKED_URL=340, ID_RANKED_TOKEN,
             ID_RANKED_BUILD, ID_RANKED_REGION, ID_RANKED_HEALTH, ID_RANKED_PROFILE,
             ID_RANKED_QUEUE, ID_RANKED_STATUS, ID_RANKED_CANCEL, ID_RANKED_RESULT, ID_RANKED_REGISTER,
             ID_KEY2_FIRST=420, ID_PLAYER2_ENABLED=450, ID_CONTROL_PLAYER1=451, ID_CONTROL_PLAYER2=452 };
enum Tab { Home, Graphics, Performance, Game, Advanced, Controls, Online };
struct Settings {
    int mode = 0;
    int resolution = 0;
    int scale = 0;
    bool asyncShaders = true;
    bool warmShaders = true;
    bool fastShaders = true;
    int inputPollRateHz = 1000;
    bool hardware = true;
    bool closeOnLaunch = true;
    std::array<bool,8> overlay{true,true,false,false,false,false,false,false};
    std::wstring saveRoot;
    std::array<int,24> keys{VK_UP,VK_DOWN,VK_LEFT,VK_RIGHT,'A','S','Z','X','Q','W','E','R','D','F',VK_RETURN,VK_BACK};
    std::array<int,24> keys2{'I','K','J','L','U','O','N','M','Y','H','7','8','5','6',VK_NUMPAD0,VK_DECIMAL};
    bool player2KeyboardEnabled = false;
};
HWND g_window = nullptr;
Tab g_tab = Home;
Settings g_settings;
HFONT g_bodyFont = nullptr, g_headingFont = nullptr, g_sectionFont = nullptr, g_smallFont = nullptr;
HBRUSH g_darkBrush = nullptr, g_panelBrush = nullptr;
std::vector<HWND> g_page;
struct PageItem { HWND hwnd; RECT bounds; };
std::vector<PageItem> g_pageItems;
std::array<HWND,7> g_nav{};
HWND g_status = nullptr, g_play = nullptr;
HANDLE g_gameProcess = nullptr;
std::atomic<bool> g_rankedBusy{false};
bool g_registrationValidation = false;
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
        MoveWindow(item.hwnd,r.left,r.top,r.right-r.left,height,TRUE);
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
    if (g_settings.inputPollRateHz != 250 && g_settings.inputPollRateHz != 500 &&
        g_settings.inputPollRateHz != 750 && g_settings.inputPollRateHz != 1000)
        g_settings.inputPollRateHz = 1000;
    g_settings.hardware = IniInt(L"Game", L"HardwareRenderer", 1, cfg.c_str()) != 0;
    g_settings.closeOnLaunch = IniInt(L"Launcher", L"CloseOnLaunch", 1, cfg.c_str()) != 0;
    const wchar_t* overlayKeys[8]{L"FPS",L"FrameTime",L"CPUUsage",L"GPUUsage",L"SystemRAM",L"GPUMemory",L"CPUTemperature",L"GPUTemperature"};
    for(int i=0;i<8;++i)g_settings.overlay[i]=IniInt(L"Overlay",overlayKeys[i],i<2?1:0,cfg.c_str())!=0;
    wchar_t saveRoot[MAX_PATH]{};
    GetPrivateProfileStringW(L"Game", L"SaveRoot", L"", saveRoot, MAX_PATH, cfg.c_str());
    g_settings.saveRoot = saveRoot;
    const wchar_t* keyNames[24]{L"Up",L"Down",L"Left",L"Right",L"A",L"B",L"X",L"Y",L"LB",L"RB",L"LT",L"RT",L"LS",L"RS",L"Start",L"Back",L"LSUp",L"LSDown",L"LSLeft",L"LSRight",L"RSUp",L"RSDown",L"RSLeft",L"RSRight"};
    for(int i=0;i<24;++i){g_settings.keys[i]=IniInt(L"Keyboard",keyNames[i],g_settings.keys[i],runtime);g_settings.keys2[i]=IniInt(L"Keyboard2",keyNames[i],g_settings.keys2[i],runtime);}
    g_settings.player2KeyboardEnabled=IniInt(L"Controls",L"Player2Keyboard",0,cfg.c_str())!=0;
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
    put(L"Game",L"HardwareRenderer",g_settings.hardware);
    put(L"Launcher",L"CloseOnLaunch",g_settings.closeOnLaunch);
    const wchar_t* overlayKeys[8]{L"FPS",L"FrameTime",L"CPUUsage",L"GPUUsage",L"SystemRAM",L"GPUMemory",L"CPUTemperature",L"GPUTemperature"};
    for(int i=0;i<8;++i){wchar_t value[2]{g_settings.overlay[i]?L'1':L'0',L'\0'};WritePrivateProfileStringW(L"Overlay",overlayKeys[i],value,cfg.c_str());}
    WritePrivateProfileStringW(L"Game",L"SaveRoot",g_settings.saveRoot.c_str(),cfg.c_str());
    const wchar_t* keyNames[24]{L"Up",L"Down",L"Left",L"Right",L"A",L"B",L"X",L"Y",L"LB",L"RB",L"LT",L"RT",L"LS",L"RS",L"Start",L"Back",L"LSUp",L"LSDown",L"LSLeft",L"LSRight",L"RSUp",L"RSDown",L"RSLeft",L"RSRight"};
    for(int i=0;i<24;++i){swprintf_s(number,L"0x%02X",unsigned(g_settings.keys[i]));WritePrivateProfileStringW(L"Keyboard",keyNames[i],number,runtime.c_str());swprintf_s(number,L"0x%02X",unsigned(g_settings.keys2[i]));WritePrivateProfileStringW(L"Keyboard2",keyNames[i],number,runtime.c_str());}
    put(L"Controls",L"Player2Keyboard",g_settings.player2KeyboardEnabled);
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
std::string RedactMatchToken(std::string body) {
    const std::string key="\"match_token\"";size_t at=body.find(key);if(at==std::string::npos)return body;
    at=body.find(':',at+key.size());if(at==std::string::npos)return body;at=body.find('\"',at+1);if(at==std::string::npos)return body;
    const size_t end=body.find('\"',at+1);if(end!=std::string::npos)body.replace(at+1,end-at-1,"[hidden]");return body;
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
    body=RedactMatchToken(body);return L"HTTP "+std::to_wstring(status)+L"\r\n"+FromUtf8(body);
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
void StartRankedRequest(int action) {
    if(g_rankedBusy.exchange(true))return;
    const std::wstring base=ControlText(ID_RANKED_URL),token=ControlText(ID_RANKED_TOKEN),build=ControlText(ID_RANKED_BUILD),region=ControlText(ID_RANKED_REGION);
    SaveRankedSettings();
    std::wstring route;std::string payload;
    switch(action){case ID_RANKED_HEALTH:route=L"/healthz";break;
    case ID_RANKED_PROFILE:route=L"/v1/profile";payload="{}";break;
    case ID_RANKED_QUEUE:route=L"/v1/queue";payload="{\"build\":"+JsonString(build)+",\"region\":"+JsonString(region)+"}";break;
    case ID_RANKED_STATUS:route=L"/v1/status";payload="{\"build\":"+JsonString(build)+",\"region\":"+JsonString(region)+"}";break;
    case ID_RANKED_CANCEL:route=L"/v1/cancel";payload="{}";break;default:g_rankedBusy=false;return;}
    for(HWND h:{GetDlgItem(g_window,ID_RANKED_HEALTH),GetDlgItem(g_window,ID_RANKED_REGISTER),GetDlgItem(g_window,ID_RANKED_PROFILE),GetDlgItem(g_window,ID_RANKED_QUEUE),GetDlgItem(g_window,ID_RANKED_STATUS),GetDlgItem(g_window,ID_RANKED_CANCEL)})if(h)EnableWindow(h,FALSE);
    SetWindowTextW(GetDlgItem(g_window,ID_RANKED_RESULT),L"Contacting ranked service...");SetWindowTextW(g_status,L"Ranked service request in progress");
    const HWND target=g_window;
    std::thread([target,base,token,route,payload]{auto* result=new std::wstring(RankedRequest(base,token,route,payload));if(!PostMessageW(target,WM_APP+6,0,(LPARAM)result))delete result;}).detach();
}
HWND Add(int id, const wchar_t* cls, const wchar_t* text, DWORD style, int x,int y,int w,int h) {
    if (wcscmp(cls,L"BUTTON")==0 && id!=0) style=(style&~BS_TYPEMASK)|BS_OWNERDRAW|WS_TABSTOP;
    HWND control=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,g_window,(HMENU)(INT_PTR)id,
        GetModuleHandleW(nullptr),nullptr);
    if(control){SendMessageW(control,WM_SETFONT,(WPARAM)g_bodyFont,TRUE);if(wcscmp(cls,L"COMBOBOX")==0||wcscmp(cls,L"EDIT")==0)SetWindowTheme(control,L"DarkMode_Explorer",nullptr);SetWindowSubclass(control,WheelForwardSubclass,0,0);g_page.push_back(control);g_pageItems.push_back({control,RECT{x,y,x+w,y+h}});g_pageBottom=(std::max)(g_pageBottom,y+h);}
    return control;
}
void Label(const wchar_t* text,int x,int y,int w,int h,bool muted=false) {
    HWND c=Add(0,L"STATIC",text,SS_LEFT|SS_NOPREFIX,x,y,w,h);
    if(c) SetPropW(c,L"ttt2.muted",(HANDLE)(INT_PTR)(muted?1:0));
}
void Select(HWND combo,int index) { if(combo) SendMessageW(combo,CB_SETCURSEL,index,0); }
HWND Combo(int id,int x,int y,int width,const std::vector<std::wstring>& items,int selected) {
    HWND c=Add(id,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,x,y,width,32);
    for(const auto& item:items) SendMessageW(c,CB_ADDSTRING,0,(LPARAM)item.c_str());
    SendMessageW(c,CB_SETMINVISIBLE,(WPARAM)(std::min)(items.size(),size_t(8)),0);
    Select(c,selected);
    return c;
}
HWND Check(int id,const wchar_t* text,int x,int y,bool checked) {
    (void)checked;
    return Add(id,L"BUTTON",text,BS_OWNERDRAW|WS_TABSTOP,x,y,(std::max)(160,g_contentX+g_contentWidth-x-2),28);
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
              {L"250 Hz",L"500 Hz",L"750 Hz",L"1000 Hz"},
              g_settings.inputPollRateHz == 250 ? 0 : g_settings.inputPollRateHz == 500 ? 1 :
              g_settings.inputPollRateHz == 750 ? 2 : 3);
        Label(L"Samples controller and keyboard input at this rate. Higher rates respond sooner and use more CPU; applies next launch.",
              g_contentX+2,456,840,36,true);
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
    case Advanced:
        Heading(L"ADVANCED",L"Launcher options, performance overlay and quick access to files.");
        SectionTitle(L"LAUNCHER",214);
        Check(ID_CLOSE_ON_LAUNCH,L"Close this window after the game starts",g_contentX+2,253,g_settings.closeOnLaunch);
        Label(L"The game can also be started again from its install folder.",g_contentX+26,283,760,25,true);
        SectionTitle(L"FILES",332);
        Add(ID_OPEN_INSTALL,L"BUTTON",L"Open game folder",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+2,374,170,36);
        Add(ID_OPEN_LOG,L"BUTTON",L"View latest log",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+188,374,150,36);
        Add(ID_VERIFY,L"BUTTON",L"Check install",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+354,374,150,36);
        Add(ID_OPEN_CONFIG,L"BUTTON",L"Open settings folder",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+520,374,190,36);
        Label(L"Log file: %LOCALAPPDATA%\\TTT2Recomp\\log.txt",g_contentX+2,422,790,28,true);
        SectionTitle(L"PERFORMANCE OVERLAY",450);
        Label(L"Choose the live metrics shown over the game. Temperature sensors may report unavailable on unsupported hardware.",g_contentX+2,484,850,36,true);
        {
            const wchar_t* overlayNames[8]{L"Frames per second",L"Frame-time graph",L"CPU usage",L"GPU usage",L"System RAM",L"GPU memory",L"CPU temperature",L"GPU temperature"};
            const int columnWidth=(g_contentWidth-24)/2;
            for(int i=0;i<8;++i){const int col=i/4,row=i%4,x=g_contentX+2+col*(columnWidth+20),y=526+row*28;Add(ID_OVERLAY_FIRST+i,L"BUTTON",overlayNames[i],BS_OWNERDRAW|WS_TABSTOP,x,y,columnWidth,26);}
        }
        break;
    case Controls: {
        Heading(L"CONTROLS",L"Click a binding and press the key you want. Changes apply on the next launch.");
        Add(ID_CONTROL_PLAYER1,L"BUTTON",L"PLAYER 1",BS_OWNERDRAW|WS_TABSTOP,g_contentX+2,210,150,34);
        Add(ID_CONTROL_PLAYER2,L"BUTTON",L"PLAYER 2",BS_OWNERDRAW|WS_TABSTOP,g_contentX+162,210,150,34);
        Check(ID_PLAYER2_ENABLED,L"Enable Player 2 keyboard",g_contentX+340,214,g_settings.player2KeyboardEnabled);
        SectionTitle(g_controlPlayer==0?L"PLAYER 1 BINDINGS":L"PLAYER 2 BINDINGS",258);
        {
            auto& map=g_controlPlayer==0?g_settings.keys:g_settings.keys2;
            for(int i=0;i<24;++i){
                const int columnWidth=(g_contentWidth-24)/2;
                const int col=i/12,row=i%12,x=g_contentX+2+col*(columnWidth+20),y=294+row*28;
                Label(ControlNames[i],x+2,y+4,112,20);
                const int id=(g_controlPlayer==0?ID_KEY_FIRST:ID_KEY2_FIRST)+i;
                HWND key=Add(id,L"BUTTON",KeyName(map[i]).c_str(),BS_OWNERDRAW|WS_TABSTOP,x+118,y,(std::max)(180,columnWidth-130),26);
                if(key)SetWindowSubclass(key,KeyCaptureSubclass,static_cast<UINT_PTR>(id),0);
            }
        }
        break;
    }
    case Online: {
        Heading(L"RANKED SERVICE",L"Connect to a community matchmaking service. Queue reservations do not launch or connect the game yet.");
        SectionTitle(L"SERVER AND ACCOUNT",214);
        Label(L"Service URL",g_contentX+2,253,175,24);
        Add(ID_RANKED_URL,L"EDIT",LoadRankedValue(L"Url",L"http://127.0.0.1:8765").c_str(),WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,g_contentX+180,249,(std::min)(630,g_contentWidth-190),28);
        Label(L"Account token",g_contentX+2,291,175,24);
        std::wstring secret=LoadRankedToken();
        const int tokenWidth=(std::max)(220,(std::min)(500,g_contentWidth-370));
        Add(ID_RANKED_TOKEN,L"EDIT",secret.c_str(),WS_BORDER|ES_AUTOHSCROLL|ES_PASSWORD|WS_TABSTOP,g_contentX+180,287,tokenWidth,32);
        Add(ID_RANKED_REGISTER,L"BUTTON",L"GENERATE TOKEN",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+192+tokenWidth,287,160,32);
        Label(L"Use the bearer token issued by the service administrator. It is saved encrypted for this Windows account.",g_contentX+180,319,(std::min)(680,g_contentWidth-190),40,true);
        Label(L"Build",g_contentX+2,369,175,24);
        Add(ID_RANKED_BUILD,L"EDIT",LoadRankedValue(L"Build",L"ttt2-community").c_str(),WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,g_contentX+180,365,(std::min)(300,(g_contentWidth-210)/2),28);
        Label(L"Region",g_contentX+500,369,75,24);
        Add(ID_RANKED_REGION,L"EDIT",LoadRankedValue(L"Region",L"global").c_str(),WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,g_contentX+575,365,(std::min)(235,(std::max)(120,g_contentWidth-585)),28);
        SectionTitle(L"MATCHMAKING",420);
        Add(ID_RANKED_HEALTH,L"BUTTON",L"CHECK SERVER",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+2,462,145,34);
        Add(ID_RANKED_PROFILE,L"BUTTON",L"MY PROFILE",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+160,462,135,34);
        Add(ID_RANKED_QUEUE,L"BUTTON",L"FIND RANKED",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+308,462,145,34);
        Add(ID_RANKED_STATUS,L"BUTTON",L"UPDATE STATUS",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+466,462,145,34);
        Add(ID_RANKED_CANCEL,L"BUTTON",L"CANCEL SEARCH",BS_PUSHBUTTON|WS_TABSTOP,g_contentX+624,462,155,34);
        Label(L"Service responses (reservation secrets are hidden):",g_contentX+2,504,700,24,true);
        Add(ID_RANKED_RESULT,L"EDIT",L"Server connection has not been checked.",WS_BORDER|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_TABSTOP,g_contentX+2,532,(std::min)(810,g_contentWidth-4),92);
        break;
    }
    }
    ReflowPage(g_window);
    InvalidateRect(g_window,nullptr,FALSE);
}
void ReadControls() {
    auto sel=[&](int id,int old){HWND h=GetDlgItem(g_window,id);int n=(int)SendMessageW(h,CB_GETCURSEL,0,0);return n<0?old:n;};
    if(GetDlgItem(g_window,ID_MODE)) g_settings.mode=std::clamp(sel(ID_MODE,g_settings.mode),0,2);
    if(GetDlgItem(g_window,ID_RESOLUTION)) g_settings.resolution=std::clamp(sel(ID_RESOLUTION,g_settings.resolution),0,4);
    if(GetDlgItem(g_window,ID_SCALE)) g_settings.scale=std::clamp(sel(ID_SCALE,g_settings.scale),0,5);
    if(GetDlgItem(g_window,ID_INPUT_POLL_RATE)) {
        const std::array<int,4> rates{250,500,750,1000};
        g_settings.inputPollRateHz=rates[std::clamp(sel(ID_INPUT_POLL_RATE,3),0,3)];
    }
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
void LaunchGame() {
    ReadControls();SaveSettings();
    const std::wstring exe=InstallDir()+L"\\"+kGameExe;
    const std::wstring image=InstallDir()+L"\\game\\tekken.img";
    if(GetFileAttributesW(exe.c_str())==INVALID_FILE_ATTRIBUTES||GetFileAttributesW(image.c_str())==INVALID_FILE_ATTRIBUTES){
        MessageBoxW(g_window,L"The game files are missing. Run the installer again or choose the installed game folder.",
                    L"TTT2 Community Edition",MB_ICONERROR|MB_OK);return;
    }
    const std::array<const wchar_t*,6> scales{L"",L"1.0",L"1.25",L"1.5",L"2.0",L"3.0"};
    SetEnv(L"TTT2_RENDER_SCALE",scales[g_settings.scale]);
    SetEnv(L"TTT2_SHADER_ASYNC",g_settings.asyncShaders?L"1":L"0");
    SetEnv(L"TTT2_SHADER_WARM",g_settings.warmShaders?L"1":L"0");
    SetEnv(L"TTT2_SHADER_FAST",g_settings.fastShaders?L"1":L"0");
    SetEnv(L"TTT2_GPU_HW",g_settings.hardware?L"1":L"0");
    unsigned overlayMask=0;for(unsigned i=0;i<g_settings.overlay.size();++i)if(g_settings.overlay[i])overlayMask|=1u<<i;
    wchar_t overlayValue[24]{};swprintf_s(overlayValue,L"%u",overlayMask);SetEnv(L"TTT2_OVERLAY_MASK",overlayValue);
    SetEnv(L"TTT2_FRAMETIME",g_settings.overlay[1]?L"1":nullptr);
    SetEnv(L"TTT2_SAVE_ROOT",g_settings.saveRoot.c_str());
    std::wstring command=L"\""+exe+L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(),command.end());mutableCommand.push_back(L'\0');
    STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
    if(!CreateProcessW(exe.c_str(),mutableCommand.data(),nullptr,nullptr,FALSE,0,nullptr,InstallDir().c_str(),&si,&pi)){
        MessageBoxW(g_window,L"Windows could not start the game. Check the game folder and permissions.",
                    L"TTT2 Community Edition",MB_ICONERROR|MB_OK);return;
    }
    CloseHandle(pi.hThread);
    if(g_settings.closeOnLaunch){CloseHandle(pi.hProcess);PostQuitMessage(0);return;}
    if(g_gameProcess)CloseHandle(g_gameProcess);
    g_gameProcess=pi.hProcess;EnableWindow(g_play,FALSE);
    SetWindowTextW(g_status,L"Game is running");
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
        const std::array<const wchar_t*,7> names{L"OVERVIEW",L"DISPLAY",L"PERFORMANCE",L"SAVES",L"CONTROLS",L"ONLINE",L"ABOUT"};
        const std::array<int,7> ids{ID_HOME,ID_GRAPHICS,ID_PERFORMANCE,ID_GAME,ID_CONTROLS,ID_ONLINE,ID_ADVANCED};
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
        const int navWidth=(w-68)/7;
        for(size_t i=0;i<g_nav.size();++i)MoveWindow(g_nav[i],34+int(i)*navWidth,80,navWidth-8,38,TRUE);
        MoveWindow(g_status,34,footer+24,460,26,TRUE);
        MoveWindow(GetDlgItem(hwnd,ID_SAVE),w-330,footer+18,120,38,TRUE);
        MoveWindow(g_play,w-190,footer+15,156,44,TRUE);
        ReadControls();BuildPage();return 0;
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
        HDC dc=(HDC)wp;HWND c=(HWND)lp;SetBkMode(dc,TRANSPARENT);
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
        const bool keyBinding=(id>=ID_KEY_FIRST&&id<ID_KEY_FIRST+24)||(id>=ID_KEY2_FIRST&&id<ID_KEY2_FIRST+24);
        if(keyBinding){const bool waiting=g_capturingKey==((id>=ID_KEY2_FIRST)?id-ID_KEY2_FIRST:id-ID_KEY_FIRST);HBRUSH fill=CreateSolidBrush(waiting?RGB(53,47,43):kPanel);FillRect(d->hDC,&d->rcItem,fill);DeleteObject(fill);HPEN edge=CreatePen(PS_SOLID,(d->itemState&ODS_FOCUS)?2:1,waiting?kRed:kLine);HGDIOBJ oldPen=SelectObject(d->hDC,edge);HGDIOBJ oldBrush=SelectObject(d->hDC,GetStockObject(NULL_BRUSH));RoundRect(d->hDC,d->rcItem.left,d->rcItem.top,d->rcItem.right,d->rcItem.bottom,5,5);SelectObject(d->hDC,oldBrush);SelectObject(d->hDC,oldPen);DeleteObject(edge);RECT text=d->rcItem;text.left+=10;text.right-=28;SetBkMode(d->hDC,TRANSPARENT);SetTextColor(d->hDC,waiting?kRed:kCream);wchar_t value[80]{};GetWindowTextW(d->hwndItem,value,80);DrawTextW(d->hDC,value,-1,&text,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);const int cx=d->rcItem.right-15,cy=(d->rcItem.top+d->rcItem.bottom)/2;HPEN glyph=CreatePen(PS_SOLID,2,kMuted);oldPen=SelectObject(d->hDC,glyph);MoveToEx(d->hDC,cx-3,cy-4,nullptr);LineTo(d->hDC,cx+1,cy);LineTo(d->hDC,cx-3,cy+4);SelectObject(d->hDC,oldPen);DeleteObject(glyph);return TRUE;}
        const bool overlayCheck=id>=ID_OVERLAY_FIRST&&id<ID_OVERLAY_FIRST+8;
        const bool checkbox=id==ID_ASYNC||id==ID_WARM||id==ID_FAST||id==ID_HARDWARE||id==ID_CLOSE_ON_LAUNCH||id==ID_PLAYER2_ENABLED||overlayCheck;
        if(checkbox){const bool checked=id==ID_PLAYER2_ENABLED?g_settings.player2KeyboardEnabled:overlayCheck?g_settings.overlay[id-ID_OVERLAY_FIRST]:id==ID_ASYNC?g_settings.asyncShaders:id==ID_WARM?g_settings.warmShaders:id==ID_FAST?g_settings.fastShaders:id==ID_HARDWARE?g_settings.hardware:g_settings.closeOnLaunch;FillRect(d->hDC,&d->rcItem,g_darkBrush);RECT box{d->rcItem.left,d->rcItem.top+6,d->rcItem.left+16,d->rcItem.top+22};HBRUSH b=CreateSolidBrush(checked?kRed:kPanel);FillRect(d->hDC,&box,b);DeleteObject(b);HBRUSH edge=CreateSolidBrush(kLine);FrameRect(d->hDC,&box,edge);DeleteObject(edge);if(checked){HPEN p=CreatePen(PS_SOLID,2,kCream);auto old=(HPEN)SelectObject(d->hDC,p);MoveToEx(d->hDC,box.left+3,box.top+8,nullptr);LineTo(d->hDC,box.left+7,box.top+12);LineTo(d->hDC,box.left+13,box.top+4);SelectObject(d->hDC,old);DeleteObject(p);}RECT tx=d->rcItem;tx.left+=25;SetBkMode(d->hDC,TRANSPARENT);SetTextColor(d->hDC,kCream);wchar_t label[128]{};GetWindowTextW(d->hwndItem,label,128);DrawTextW(d->hDC,label,-1,&tx,DT_LEFT|DT_VCENTER|DT_SINGLELINE);return TRUE;}
        int pageIndex=-1;switch(id){case ID_HOME:pageIndex=0;break;case ID_GRAPHICS:pageIndex=1;break;case ID_PERFORMANCE:pageIndex=2;break;case ID_GAME:pageIndex=3;break;case ID_ADVANCED:pageIndex=4;break;case ID_CONTROLS:pageIndex=5;break;case ID_ONLINE:pageIndex=6;break;}const bool active=pageIndex>=0&&int(g_tab)==pageIndex;
        const bool play=id==ID_PLAY;const bool controlTab=id==ID_CONTROL_PLAYER1||id==ID_CONTROL_PLAYER2;const bool selectedControl=id==ID_CONTROL_PLAYER1?g_controlPlayer==0:id==ID_CONTROL_PLAYER2&&g_controlPlayer==1;const bool primary=play;const COLORREF fill=primary?kRed:((active||(controlTab&&selectedControl))?RGB(47,34,39):kPanel);HBRUSH brush=CreateSolidBrush(fill);
        HPEN p=CreatePen(PS_SOLID,1,primary?kRed:kLine);auto oldPen=(HPEN)SelectObject(d->hDC,p);auto oldBrush=(HBRUSH)SelectObject(d->hDC,brush);RoundRect(d->hDC,d->rcItem.left,d->rcItem.top,d->rcItem.right,d->rcItem.bottom,8,8);SelectObject(d->hDC,oldBrush);SelectObject(d->hDC,oldPen);DeleteObject(p);DeleteObject(brush);
        SetBkMode(d->hDC,TRANSPARENT);SetTextColor(d->hDC,(active||play||(controlTab&&selectedControl))?kCream:kMuted);
        HFONT f=(play||id==ID_SAVE)?g_bodyFont:g_smallFont;auto old=(HFONT)SelectObject(d->hDC,f);
        wchar_t text[128]{};GetWindowTextW(d->hwndItem,text,128);DrawTextW(d->hDC,text,-1,&d->rcItem,DT_CENTER|DT_VCENTER|DT_SINGLELINE);SelectObject(d->hDC,old);
        if(d->itemState&ODS_FOCUS){RECT r=d->rcItem;InflateRect(&r,-4,-4);DrawFocusRect(d->hDC,&r);}return TRUE;
    }
    case WM_COMMAND: {
        const int id=LOWORD(wp);
        if(id>=ID_HOME&&id<=ID_ONLINE){ReadControls();if(g_tab==Online)SaveRankedSettings();g_tab=(Tab)(id-ID_HOME);g_scrollOffset=0;for(size_t i=0;i<g_nav.size();++i)InvalidateRect(g_nav[i],nullptr,TRUE);BuildPage();return 0;}
        if(id==ID_SAVE){ReadControls();SaveSettings();if(g_tab==Online)SaveRankedSettings();SetWindowTextW(g_status,L"Settings saved");return 0;}
        if(id==ID_PLAY){LaunchGame();return 0;}
        if(id==ID_CONTROL_PLAYER1||id==ID_CONTROL_PLAYER2){g_capturingKey=-1;g_controlPlayer=id==ID_CONTROL_PLAYER1?0:1;g_scrollOffset=0;BuildPage();return 0;}
        if(id==ID_PLAYER2_ENABLED){g_settings.player2KeyboardEnabled=!g_settings.player2KeyboardEnabled;InvalidateRect(GetDlgItem(hwnd,id),nullptr,TRUE);return 0;}
        if((id>=ID_KEY_FIRST&&id<ID_KEY_FIRST+24)||(id>=ID_KEY2_FIRST&&id<ID_KEY2_FIRST+24)){g_controlPlayer=id>=ID_KEY2_FIRST?1:0;g_capturingKey=id-(g_controlPlayer?ID_KEY2_FIRST:ID_KEY_FIRST);SetWindowTextW(GetDlgItem(hwnd,id),L"PRESS A KEY...");SetFocus(GetDlgItem(hwnd,id));InvalidateRect(GetDlgItem(hwnd,id),nullptr,TRUE);SetWindowTextW(g_status,L"Press a key for this binding. Escape is reserved.");return 0;}
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
        if(id==ID_MODE||id==ID_RESOLUTION||id==ID_SCALE||id==ID_INPUT_POLL_RATE){ReadControls();return 0;}
        if(id>=ID_OVERLAY_FIRST&&id<ID_OVERLAY_FIRST+8){g_settings.overlay[id-ID_OVERLAY_FIRST]=!g_settings.overlay[id-ID_OVERLAY_FIRST];InvalidateRect(GetDlgItem(hwnd,id),nullptr,TRUE);return 0;}
        if(id==ID_ASYNC||id==ID_WARM||id==ID_FAST||id==ID_HARDWARE||id==ID_CLOSE_ON_LAUNCH){if(id==ID_ASYNC)g_settings.asyncShaders=!g_settings.asyncShaders;else if(id==ID_WARM)g_settings.warmShaders=!g_settings.warmShaders;else if(id==ID_FAST)g_settings.fastShaders=!g_settings.fastShaders;else if(id==ID_HARDWARE)g_settings.hardware=!g_settings.hardware;else g_settings.closeOnLaunch=!g_settings.closeOnLaunch;InvalidateRect(GetDlgItem(hwnd,id),nullptr,TRUE);return 0;}
        break;
    }
    case WM_APP+6: {
        std::unique_ptr<std::wstring> result(reinterpret_cast<std::wstring*>(lp));
        if(GetDlgItem(hwnd,ID_RANKED_RESULT))SetWindowTextW(GetDlgItem(hwnd,ID_RANKED_RESULT),result->c_str());
        for(int id:{ID_RANKED_HEALTH,ID_RANKED_REGISTER,ID_RANKED_PROFILE,ID_RANKED_QUEUE,ID_RANKED_STATUS,ID_RANKED_CANCEL})if(HWND h=GetDlgItem(hwnd,id))EnableWindow(h,TRUE);
        const bool validating=g_registrationValidation;g_registrationValidation=false;
        SetWindowTextW(g_status,validating?(result->rfind(L"HTTP 200",0)==0?L"Token successfully generated and saved!":L"Token saved, but server validation failed."):L"Ranked service response received");g_rankedBusy=false;return 0;
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
