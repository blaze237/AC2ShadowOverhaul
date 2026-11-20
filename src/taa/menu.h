// In-game settings menu (F1): see menu.cpp.
#pragma once

#include <d3d9.h>

namespace taa
{
void Menu_Init();                                // [Menu] from the ini
void Menu_OnPresent(IDirect3DDevice9* dev);      // polls keys, draws the menu if shown
void Menu_OnLostDevice();                        // before Reset
void Menu_OnResetDevice();                       // after a successful Reset
void Menu_Flush();                               // writes pending changes to the ini (exit)

} // namespace taa
