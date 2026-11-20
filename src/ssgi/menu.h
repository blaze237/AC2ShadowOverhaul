// AC2SSGI - in-game menu (F3 by default, top right): live tuning of the SSGI settings.
#pragma once

#include <d3d9.h>
#include "gi.h"

namespace ssgi
{

bool Menu_Init();  // reads [Menu]; returns whether there is a menu key
void Menu_Flush(); // saves pending changes (at exit)
// Polls the keys and draws the menu on the back buffer; call just before Present. Render targets
// are set through D (below our own hooks).
void Menu_OnPresent(IDirect3DDevice9* dev, const GiDeviceFns& D);
void Menu_OnLostDevice();
void Menu_OnResetDevice();

} // namespace ssgi
