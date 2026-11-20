// SSGI's in-game menu (F3 by default, top right): live tuning of the SSGI settings.
#pragma once

#include <d3d9.h>

namespace ssgi
{

bool Menu_Init();  // reads [Menu]; returns whether there is a menu key
void Menu_Flush(); // saves pending changes (at exit)
// Polls the keys and draws the menu on the back buffer; call just before Present.
void Menu_OnPresent(IDirect3DDevice9* dev);

} // namespace ssgi
