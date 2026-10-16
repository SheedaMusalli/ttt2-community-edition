// Keyboard bindings for two local players, kept as
//   [Keyboard]
//   Up=0x26 ...           (Windows virtual-key codes; 0 = no key)
//   [Keyboard2]
//   Up=0x68 ...
// in %LOCALAPPDATA%\TTT2Recomp\settings.ini (TTT2_SETTINGS_INI overrides the file, for tests).
#pragma once
#include <cstdint>
#include <string>

namespace keyboard_keys {

enum Control : int {
    Up, Down, Left, Right, A, B, X, Y, LB, RB, LT, RT, LS, RS, Start, Back,
    LSUp, LSDown, LSLeft, LSRight, RSUp, RSDown, RSLeft, RSRight, Count
};

// The control's key (virtual-key code, 0 = unassigned). Player is 0 or 1.
int Key(int control, int player = 0);
// Gives `control` the key `vk`; a control that had `vk` takes `control`'s old key. Saved at once.
void Assign(int control, int vk, int player = 0);
void RestoreDefaults(int player = 0);
// True when some control has `vk`.
bool Bound(int vk, int player = 0);

// XInput button mask of a control (0 for the triggers LT / RT).
uint16_t Mask(int control);
// The control as the game names it ("UP", "X", "LT", "START", ...).
const char* ControlName(int control);
// A key as shown in the game ("Z", "ENTER", "NUM 4", "L SHIFT", ...); "-" for no key.
std::string KeyName(int vk);
// False for keys that cannot be assigned (ESC keeps a key in the guided setup).
bool Assignable(int vk);

} // namespace keyboard_keys
