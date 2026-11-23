// While one of the in-game menus is open, the arrow keys drive the menu only: the game doesn't see
// them. AC2 reads the keyboard through DirectInput 8 (GetDeviceState / GetDeviceData of its keyboard
// device) and GetAsyncKeyState; both are filtered here. The menus read the keys through the
// plugin's own GetAsyncKeyState import, which isn't touched.
#pragma once

namespace core
{
void InputBlock_Init(); // patches the game exe's DirectInput8Create and GetAsyncKeyState imports
}
