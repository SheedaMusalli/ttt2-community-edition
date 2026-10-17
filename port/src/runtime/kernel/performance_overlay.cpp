#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

void GpuHwReadMemoryUsage(uint64_t* usage, uint64_t* budget);

namespace performance_overlay
{
namespace
{
constexpr char kClassName[] = "TTT2PerformanceOverlay";
constexpr int kWidth = 420;
constexpr int kRowHeight = 22;
std::atomic<uint64_t> g_presents{0};
std::atomic<bool> g_started{false};
std::array<std::atomic<double>, 36> g_frameSamples{};

enum Metric : unsigned
{
    Fps = 1u << 0, FrameTime = 1u << 1, MetricCpuUsage = 1u << 2, MetricGpuUsage = 1u << 3,
    SystemRam = 1u << 4, GpuMemory = 1u << 5, CpuTemperature = 1u << 6, GpuTemperature = 1u << 7
};

unsigned SelectedMetrics()
{
    const char* value = std::getenv("TTT2_OVERLAY_MASK");
    if (!value) return 0;
    return static_cast<unsigned>(std::strtoul(value, nullptr, 10)) & 0xFFu;
}

struct Snapshot
{
    double fps = 0, frameMs = 0, cpu = -1, cpuCore = -1, gpu = -1;
    uint64_t ramUsed = 0, ramTotal = 0, processRam = 0, vramUsed = 0, vramBudget = 0;
    std::array<double, 36> frameGraph{};
    size_t graphCount = 0;
};

uint64_t FileTimeValue(const FILETIME& value)
{
    ULARGE_INTEGER result{};
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

double CpuUsage(double& oneCore, uint64_t& previousCpu, std::chrono::steady_clock::time_point& previousTime)
{
    FILETIME created{}, exited{}, kernel{}, user{};
    oneCore = -1;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1;
    const uint64_t currentCpu = FileTimeValue(kernel) + FileTimeValue(user);
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - previousTime).count();
    const uint64_t used = currentCpu - previousCpu;
    previousCpu = currentCpu;
    previousTime = now;
    if (elapsed <= 0) return -1;
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const unsigned cores = static_cast<unsigned>((std::max<DWORD>)(1, info.dwNumberOfProcessors));
    oneCore = double(used) / (10'000'000.0 * elapsed) * 100.0;
    return (std::min)(100.0, oneCore / cores);
}

double GpuUsage(PDH_HQUERY query, PDH_HCOUNTER counter)
{
    if (!query || !counter || PdhCollectQueryData(query) != ERROR_SUCCESS) return -1;
    DWORD bytes = 0, items = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &items, nullptr);
    if (status != PDH_MORE_DATA || !bytes) return -1;
    std::vector<unsigned char> storage(bytes);
    auto* values = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(storage.data());
    status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &items, values);
    if (status != ERROR_SUCCESS) return -1;
    const DWORD pid = GetCurrentProcessId();
    const std::wstring wanted = L"pid_" + std::to_wstring(pid) + L"_";
    double total = 0;
    bool found = false;
    for (DWORD i = 0; i < items; ++i)
    {
        const std::wstring instance = values[i].szName ? values[i].szName : L"";
        if (instance.find(wanted) == std::wstring::npos ||
            (instance.find(L"engtype_3D") == std::wstring::npos && instance.find(L"High Priority 3D") == std::wstring::npos) ||
            values[i].FmtValue.CStatus != ERROR_SUCCESS)
            continue;
        total += values[i].FmtValue.doubleValue;
        found = true;
    }
    return found ? (std::min)(100.0, total) : -1;
}

void Sample(Snapshot& value, unsigned metrics, PDH_HQUERY query, PDH_HCOUNTER counter,
            uint64_t& previousCpu, std::chrono::steady_clock::time_point& previousCpuTime,
            uint64_t& previousPresents, std::chrono::steady_clock::time_point& previousFrameTime)
{
    const auto now = std::chrono::steady_clock::now();
    const uint64_t frames = g_presents.load(std::memory_order_relaxed);
    const double elapsed = std::chrono::duration<double>(now - previousFrameTime).count();
    const uint64_t delta = frames - previousPresents;
    value.fps = elapsed > 0 ? double(delta) / elapsed : 0;
    value.frameMs = delta ? elapsed * 1000.0 / double(delta) : 0;
    previousPresents = frames;
    previousFrameTime = now;
    value.graphCount = static_cast<size_t>((std::min<uint64_t>)(frames, value.frameGraph.size()));
    for (size_t i = 0; i < value.graphCount; ++i)
    {
        const uint64_t frame = frames - value.graphCount + i;
        value.frameGraph[i] = g_frameSamples[frame % g_frameSamples.size()].load(std::memory_order_relaxed);
    }

    if (metrics & MetricCpuUsage) value.cpu = CpuUsage(value.cpuCore, previousCpu, previousCpuTime);
    if (metrics & MetricGpuUsage) value.gpu = GpuUsage(query, counter);
    if (metrics & SystemRam)
    {
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        if (GlobalMemoryStatusEx(&memory))
        {
            value.ramTotal = memory.ullTotalPhys;
            value.ramUsed = memory.ullTotalPhys - memory.ullAvailPhys;
        }
        PROCESS_MEMORY_COUNTERS_EX process{};
        process.cb = sizeof(process);
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), sizeof(process)))
            value.processRam = process.WorkingSetSize;
    }
    if (metrics & GpuMemory)
    {
        GpuHwReadMemoryUsage(&value.vramUsed, &value.vramBudget);
    }
}

std::wstring Percent(double value)
{
    return value < 0 ? L"N/A" : std::to_wstring(static_cast<unsigned>(value + 0.5)) + L"%";
}

void Text(HDC dc, int x, int y, const wchar_t* label, const std::wstring& value)
{
    SetTextColor(dc, RGB(226, 234, 243));
    TextOutW(dc, x, y, label, static_cast<int>(wcslen(label)));
    SetTextColor(dc, RGB(65, 220, 190));
    TextOutW(dc, x + 150, y, value.c_str(), static_cast<int>(value.size()));
}

LRESULT CALLBACK OverlayProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT)
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        HBRUSH background = CreateSolidBrush(RGB(12, 17, 25));
        FillRect(dc, &client, background);
        DeleteObject(background);
        SetBkMode(dc, TRANSPARENT);
        HFONT font = CreateFontW(15, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, font);
        const auto* snapshot = reinterpret_cast<const Snapshot*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        const unsigned metrics = SelectedMetrics();
        int y = 10;
        SetTextColor(dc, RGB(233, 240, 247));
        static constexpr wchar_t title[] = L"TTT2  /  PERFORMANCE";
        TextOutW(dc, 12, y, title, static_cast<int>(std::size(title) - 1));
        y += 28;
        if (snapshot)
        {
            if (metrics & Fps) { wchar_t v[32]{}; swprintf_s(v, L"%.1f FPS", snapshot->fps); Text(dc, 12, y, L"Frame rate", v); y += kRowHeight; }
            if (metrics & FrameTime)
            {
                wchar_t v[32]{}; swprintf_s(v, L"%.2f ms", snapshot->frameMs); Text(dc, 12, y, L"Frame time", v); y += kRowHeight;
                const int graphTop = y, graphHeight = 54, graphLeft = 12, graphWidth = kWidth - 24;
                HBRUSH graphBg = CreateSolidBrush(RGB(22, 30, 40));
                RECT graph{graphLeft,graphTop,graphLeft+graphWidth,graphTop+graphHeight};
                FillRect(dc,&graph,graphBg); DeleteObject(graphBg);
                HPEN pen = CreatePen(PS_SOLID, 2, RGB(65,220,190));
                HGDIOBJ oldPen = SelectObject(dc, pen);
                const size_t count = (std::min)(snapshot->graphCount, snapshot->frameGraph.size());
                for(size_t i=0;i<count;++i)
                {
                    const size_t index=(snapshot->graphCount-count+i)%snapshot->frameGraph.size();
                    const int x=graphLeft+int(i*(graphWidth-1)/(count>1?count-1:1));
                    const int h=(std::min)(graphHeight-4,int(snapshot->frameGraph[index]*2.4));
                    const int py=graphTop+graphHeight-2-h;
                    if(i==0) MoveToEx(dc,x,py,nullptr); else LineTo(dc,x,py);
                }
                SelectObject(dc,oldPen); DeleteObject(pen);
                y += graphHeight + 8;
            }
            if (metrics & MetricCpuUsage) { const auto v=Percent(snapshot->cpu)+L" / "+Percent(snapshot->cpuCore); Text(dc,12,y,L"CPU total / core",v); y+=kRowHeight; }
            if (metrics & MetricGpuUsage) { const auto v=Percent(snapshot->gpu); Text(dc,12,y,L"GPU usage",v); y+=kRowHeight; }
            if (metrics & SystemRam)
            {
                wchar_t v[96]{};
                swprintf_s(v,L"%llu / %llu GB (game %llu MB)",
                    static_cast<unsigned long long>(snapshot->ramUsed/(1024ull*1024*1024)),
                    static_cast<unsigned long long>(snapshot->ramTotal/(1024ull*1024*1024)),
                    static_cast<unsigned long long>(snapshot->processRam/(1024ull*1024)));
                Text(dc,12,y,L"System RAM",v); y+=kRowHeight;
            }
            if (metrics & GpuMemory)
            {
                wchar_t v[64]{};
                if(snapshot->vramBudget) swprintf_s(v,L"%llu / %llu MB",static_cast<unsigned long long>(snapshot->vramUsed/(1024ull*1024)),static_cast<unsigned long long>(snapshot->vramBudget/(1024ull*1024)));
                else wcscpy_s(v,L"N/A");
                Text(dc,12,y,L"GPU memory",v); y+=kRowHeight;
            }
            if (metrics & CpuTemperature) { Text(dc,12,y,L"CPU temperature",L"N/A (sensor unavailable)"); y+=kRowHeight; }
            if (metrics & GpuTemperature) { Text(dc,12,y,L"GPU temperature",L"N/A (sensor unavailable)"); y+=kRowHeight; }
        }
        SelectObject(dc, oldFont);
        DeleteObject(font);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(window, message, wParam, lParam);
}

void Run(HWND gameWindow)
{
    const unsigned metrics = SelectedMetrics();
    if (!metrics || !IsWindow(gameWindow)) return;
    const char* enabledValue=std::getenv("TTT2_OVERLAY_ENABLED");
    if(enabledValue&&std::strcmp(enabledValue,"0")==0)return;
    int toggleKey=VK_F10;
    if(const char* key=std::getenv("TTT2_OVERLAY_TOGGLE_KEY")){const int parsed=std::atoi(key);if(parsed>0&&parsed<=255)toggleKey=parsed;}
    WNDCLASSW wc{};
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TTT2PerformanceOverlay";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&wc);
    const int rows = ((metrics & MetricCpuUsage) ? 1 : 0) + ((metrics & MetricGpuUsage) ? 1 : 0) +
                     ((metrics & SystemRam) ? 1 : 0) + ((metrics & GpuMemory) ? 1 : 0) +
                     ((metrics & CpuTemperature) ? 1 : 0) + ((metrics & GpuTemperature) ? 1 : 0) +
                     ((metrics & Fps) ? 1 : 0) + ((metrics & FrameTime) ? 2 : 0);
    const int height = 48 + rows * kRowHeight + ((metrics & FrameTime) ? 62 : 0);
    HWND overlay = CreateWindowExW(WS_EX_TOPMOST|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_LAYERED,
        wc.lpszClassName,L"TTT2 Performance Overlay",WS_POPUP,0,0,kWidth,height,nullptr,nullptr,wc.hInstance,nullptr);
    if (!overlay) return;
    SetLayeredWindowAttributes(overlay,0,222,LWA_ALPHA);
    SetWindowLongPtrW(overlay,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(new Snapshot{}));
    PDH_HQUERY query=nullptr; PDH_HCOUNTER counter=nullptr;
    if ((metrics & MetricGpuUsage) && PdhOpenQueryW(nullptr,0,&query)==ERROR_SUCCESS)
        if(PdhAddEnglishCounterW(query,L"\\GPU Engine(*)\\Utilization Percentage",0,&counter)!=ERROR_SUCCESS){PdhCloseQuery(query);query=nullptr;counter=nullptr;}
    Snapshot snapshot{};
    uint64_t previousCpu=0,previousPresents=g_presents.load();
    FILETIME created{},exited{},kernel{},user{};GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user);previousCpu=FileTimeValue(kernel)+FileTimeValue(user);
    auto cpuTime=std::chrono::steady_clock::now(),frameTime=cpuTime;
    auto nextSample=cpuTime;
    bool userVisible=true,previousToggleDown=false;
    while(IsWindow(gameWindow)&&IsWindow(overlay))
    {
        MSG message{};
        while(PeekMessageW(&message,overlay,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        const bool toggleDown=(GetAsyncKeyState(toggleKey)&0x8000)!=0;
        if(toggleDown&&!previousToggleDown)userVisible=!userVisible;
        previousToggleDown=toggleDown;
        const bool visible=userVisible&&IsWindowVisible(gameWindow)&&!IsIconic(gameWindow)&&GetForegroundWindow()==gameWindow;
        if(visible)
        {
            RECT r{};GetClientRect(gameWindow,&r);POINT p{12,46};ClientToScreen(gameWindow,&p);
            SetWindowPos(overlay,HWND_TOPMOST,p.x,p.y,kWidth,height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
            const auto now=std::chrono::steady_clock::now();
            if(now>=nextSample){Sample(snapshot,metrics,query,counter,previousCpu,cpuTime,previousPresents,frameTime);
                *reinterpret_cast<Snapshot*>(GetWindowLongPtrW(overlay,GWLP_USERDATA))=snapshot;
                InvalidateRect(overlay,nullptr,FALSE);UpdateWindow(overlay);nextSample=now+std::chrono::milliseconds(500);}
        }
        else ShowWindow(overlay,SW_HIDE);
        Sleep(16);
    }
    if(query)PdhCloseQuery(query);
    delete reinterpret_cast<Snapshot*>(GetWindowLongPtrW(overlay,GWLP_USERDATA));
    DestroyWindow(overlay);
}
}

void Start(void* gameWindow)
{
    const char* enabledValue=std::getenv("TTT2_OVERLAY_ENABLED");
    if ((enabledValue&&std::strcmp(enabledValue,"0")==0)||!SelectedMetrics()||g_started.exchange(true)) return;
    std::thread(Run,static_cast<HWND>(gameWindow)).detach();
}

void NotifyPresent()
{
    static auto previous = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (previous.time_since_epoch().count())
    {
        const double frameMs = std::chrono::duration<double, std::milli>(now - previous).count();
        const uint64_t index = g_presents.load(std::memory_order_relaxed);
        g_frameSamples[index % g_frameSamples.size()].store(frameMs, std::memory_order_relaxed);
    }
    previous = now;
    g_presents.fetch_add(1,std::memory_order_relaxed);
}

uint64_t PresentedFrameCount()
{
    return g_presents.load(std::memory_order_relaxed);
}
}
