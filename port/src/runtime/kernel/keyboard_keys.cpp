// Keyboard key settings (see keyboard_keys.h). The input poll thread reads the keys, the game's
// main thread changes them: one atomic per control.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "keyboard_keys.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace keyboard_keys {
namespace {

struct Info { const char* ini; const char* name; uint16_t mask; int defaultKey; };
// Defaults: the keys the port always had (arrows, A S Z X, Q W, ENTER, BACKSPACE) plus E R D F for
// the controls that had none.
const Info kInfo[Count] = {
    { "Up", "UP", 0x0001, VK_UP },          { "Down", "DOWN", 0x0002, VK_DOWN },
    { "Left", "LEFT", 0x0004, VK_LEFT },    { "Right", "RIGHT", 0x0008, VK_RIGHT },
    { "A", "A", 0x1000, 'A' },              { "B", "B", 0x2000, 'S' },
    { "X", "X", 0x4000, 'Z' },              { "Y", "Y", 0x8000, 'X' },
    { "LB", "LB", 0x0100, 'Q' },            { "RB", "RB", 0x0200, 'W' },
    { "LT", "LT", 0, 'E' },                 { "RT", "RT", 0, 'R' },
    { "LS", "LS", 0x0040, 'D' },            { "RS", "RS", 0x0080, 'F' },
    { "Start", "START", 0x0010, VK_RETURN }, { "Back", "BACK", 0x0020, VK_BACK },
    { "LSUp", "LS UP", 0, 0 }, { "LSDown", "LS DOWN", 0, 0 },
    { "LSLeft", "LS LEFT", 0, 0 }, { "LSRight", "LS RIGHT", 0, 0 },
    { "RSUp", "RS UP", 0, 0 }, { "RSDown", "RS DOWN", 0, 0 },
    { "RSLeft", "RS LEFT", 0, 0 }, { "RSRight", "RS RIGHT", 0, 0 },
};
const int kPlayer2Defaults[Count] = {
    'I','K','J','L','U','O','N','M','Y','H','7','8','5','6',VK_NUMPAD0,VK_DECIMAL,
    0,0,0,0,0,0,0,0,
};
int DefaultKey(int control, int player)
{
    return player == 1 ? kPlayer2Defaults[control] : kInfo[control].defaultKey;
}

std::atomic<int> g_keys[2][Count];
std::once_flag g_loaded;
std::mutex g_writeMutex;

std::string IniPath()
{
    if (const char* e = std::getenv("TTT2_SETTINGS_INI"); e && *e) return e;
    char dir[MAX_PATH];
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", dir, sizeof(dir));
    std::string path = (n && n < sizeof(dir)) ? std::string(dir) : std::string(".");
    path += "\\TTT2Recomp";
    CreateDirectoryA(path.c_str(), nullptr);
    return path + "\\settings.ini";
}

void Load()
{
    std::call_once(g_loaded, [] {
        const std::string ini = IniPath();
        for (int p = 0; p < 2; ++p)
        for (int c = 0; c < Count; ++c)
        {
            char value[32] = {};
            const char* section = p == 0 ? "Keyboard" : "Keyboard2";
            GetPrivateProfileStringA(section, kInfo[c].ini, "", value, sizeof(value), ini.c_str());
            int vk = DefaultKey(c, p);
            if (*value) vk = int(std::strtol(value, nullptr, 0));
            g_keys[p][c] = (vk >= 0 && vk < 256) ? vk : DefaultKey(c, p);
        }
        // Duplicate keys in one player's map are resolved deterministically. Players may
        // intentionally use identical keys across their separate maps.
        for (int p = 0; p < 2; ++p)
        for (int c = 0; c < Count; ++c)
            for (int d = 0; d < c; ++d)
                if (g_keys[p][c] && g_keys[p][c] == g_keys[p][d]) g_keys[p][c] = 0;
        std::string line = "[keyboard] player 1 keys:";
        for (int c = 0; c < Count; ++c) line += std::string(" ") + kInfo[c].name + "=" + KeyName(g_keys[0][c]);
        std::fprintf(stderr, "%s (%s)\n", line.c_str(), ini.c_str());
    });
}

void Save()
{
    const std::string ini = IniPath();
    for (int c = 0; c < Count; ++c)
    {
        char value[16];
        std::snprintf(value, sizeof(value), "0x%02X", unsigned(g_keys[0][c].load()));
        WritePrivateProfileStringA("Keyboard", kInfo[c].ini, value, ini.c_str());
        std::snprintf(value, sizeof(value), "0x%02X", unsigned(g_keys[1][c].load()));
        WritePrivateProfileStringA("Keyboard2", kInfo[c].ini, value, ini.c_str());
    }
}

} // namespace

int Key(int control, int player)
{
    Load();
    return player >= 0 && player < 2 && control >= 0 && control < Count ? g_keys[player][control].load() : 0;
}

bool Bound(int vk, int player)
{
    Load();
    if (player < 0 || player >= 2) return false;
    for (int c = 0; c < Count; ++c)
        if (vk && g_keys[player][c].load() == vk) return true;
    return false;
}

void Assign(int control, int vk, int player)
{
    Load();
    if (player < 0 || player >= 2 || control < 0 || control >= Count || !Assignable(vk)) return;
    std::lock_guard<std::mutex> l(g_writeMutex);
    const int old = g_keys[player][control].load();
    if (old == vk) return;
    for (int c = 0; c < Count; ++c)
        if (c != control && g_keys[player][c].load() == vk) g_keys[player][c] = old; // swap
    g_keys[player][control] = vk;
    Save();
    std::fprintf(stderr, "[keyboard] player %d %s = %s\n", player + 1, kInfo[control].name, KeyName(vk).c_str());
}

void RestoreDefaults(int player)
{
    Load();
    if (player < 0 || player >= 2) return;
    std::lock_guard<std::mutex> l(g_writeMutex);
    for (int c = 0; c < Count; ++c) g_keys[player][c] = DefaultKey(c, player);
    Save();
    std::fprintf(stderr, "[keyboard] default keys restored\n");
}

uint16_t Mask(int control) { return control >= 0 && control < Count ? kInfo[control].mask : 0; }
const char* ControlName(int control) { return control >= 0 && control < Count ? kInfo[control].name : "?"; }

bool Assignable(int vk)
{
    // ESC keeps a key in the guided setup; the Windows keys and the lock / system keys do not
    // reach a game reliably.
    switch (vk)
    {
    case 0: case VK_ESCAPE: case VK_LWIN: case VK_RWIN: case VK_APPS: case VK_SNAPSHOT: case VK_SCROLL:
    case VK_PAUSE: case VK_NUMLOCK: case VK_CAPITAL: case VK_SHIFT: case VK_CONTROL: case VK_MENU:
    case VK_LBUTTON: case VK_RBUTTON: case VK_MBUTTON: case VK_XBUTTON1: case VK_XBUTTON2:
        return false;
    default:
        return vk > 0 && vk < 256;
    }
}

std::string KeyName(int vk)
{
    if (vk >= 'A' && vk <= 'Z') return std::string(1, char(vk));
    if (vk >= '0' && vk <= '9') return std::string(1, char(vk));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "NUM " + std::to_string(vk - VK_NUMPAD0);
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    switch (vk)
    {
    case 0: return "-";
    case VK_UP: return "UP";
    case VK_DOWN: return "DOWN";
    case VK_LEFT: return "LEFT";
    case VK_RIGHT: return "RIGHT";
    case VK_RETURN: return "ENTER";
    case VK_SPACE: return "SPACE";
    case VK_BACK: return "BACKSPACE";
    case VK_TAB: return "TAB";
    case VK_LSHIFT: return "L SHIFT";
    case VK_RSHIFT: return "R SHIFT";
    case VK_LCONTROL: return "L CTRL";
    case VK_RCONTROL: return "R CTRL";
    case VK_LMENU: return "L ALT";
    case VK_RMENU: return "R ALT";
    case VK_INSERT: return "INSERT";
    case VK_DELETE: return "DELETE";
    case VK_HOME: return "HOME";
    case VK_END: return "END";
    case VK_PRIOR: return "PAGE UP";
    case VK_NEXT: return "PAGE DOWN";
    case VK_MULTIPLY: return "NUM *";
    case VK_ADD: return "NUM +";
    case VK_SUBTRACT: return "NUM -";
    case VK_DECIMAL: return "NUM .";
    case VK_DIVIDE: return "NUM /";
    case VK_OEM_1: return ";";
    case VK_OEM_PLUS: return "=";
    case VK_OEM_COMMA: return ",";
    case VK_OEM_MINUS: return "MINUS";                 // "-" means no key
    case VK_OEM_PERIOD: return ".";
    case VK_OEM_2: return "/";
    case VK_OEM_3: return "`";
    case VK_OEM_4: return "[";
    case VK_OEM_5: return "\\";
    case VK_OEM_6: return "]";
    case VK_OEM_7: return "'";
    default:
    {
        char name[32];
        std::snprintf(name, sizeof(name), "KEY %02X", unsigned(vk));
        return name;
    }
    }
}

} // namespace keyboard_keys
