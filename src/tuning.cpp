// In-game menu: live tuning of the shadow settings, ambient occlusion and contact shadows.
//
// The menu key ([Menu] MenuKey, default F9) shows / hides it; [Menu] StartVisible shows it at
// start. Left / Right change the selected setting: scales by 0.5, whole-number settings by 1,
// on/off settings switch. The keys, drawing and saving are the shared menu engine's (core/menu.h).

#include "common.h"
#include "softshadows.h"
#include "ao.h"
#include "core/menu.h"
#include <d3d9.h>
#include <stdio.h>

namespace
{

enum
{
	ITEM_DISTANCE,
	ITEM_SPLIT,
	ITEM_CASTER,
	ITEM_DRAW,
	ITEM_SOFT_WIDTH,
	ITEM_AO_MODE,
	ITEM_AO_RADIUS,
	ITEM_AO_STRENGTH,
	ITEM_CS_ON,
	ITEM_CS_LENGTH,
	ITEM_ALL_ON,     // master switch for every effect; not saved
	ITEM_FRAME_DUMP, // writes a frame dump ([Debug] FrameDump=1 only); not a setting
	ITEM_COUNT
};
const core::MenuItem kItems[ITEM_COUNT] = {
	{ "Shadow distance", "Shadows", "ShadowDistanceScale", true },
	{ "Cascade splits", "Shadows", "ShadowCascadeSplitScale", true },
	{ "Caster distance", "Shadows", "ShadowCasterDistanceScale", true },
	{ "Detail draw distance", "Shadows", "DetailDrawDistanceScale", true },
	{ "Soft shadow width", "SoftShadows", "FilterWidth", true },
	{ "Ambient occlusion", "AmbientOcclusion", "DebugView", true }, // StartEnabled too (AfterSave)
	{ "AO radius", "AmbientOcclusion", "Radius", true },
	{ "AO strength", "AmbientOcclusion", "Strength", true },
	{ "Contact shadows", "ContactShadows", "StartEnabled", true },
	{ "Contact shadow length", "ContactShadows", "Length", true },
	{ "All shadow effects", nullptr, nullptr, true },
	{ "Frame dump", nullptr, nullptr, false },
};
const core::MenuDef kDef = { core::MENU_SHADOWS, "AC2 Shadow Overhaul - Shadows & AO", "Shadow menu", "MenuKey", VK_F9, kItems, ITEM_COUNT,
	true, true };

// Soft shadow width as the menu shows it: 0 = off (the game's own filter), else 1..4.
int SoftWidth() { return SoftShadows_Active() ? SoftShadows_LiveWidth() : 0; }

// Ambient occlusion menu modes: off, on, then the debug views (AO, the indirect light buffer, contact
// shadows, the sun light buffer).
const char* const kAoModes[] = { "OFF", "ON", "show AO only", "show indirect light", "show contact shadows only", "show sun light" };
int AoMode() { return AO_View() ? AO_View() + 1 : AO_Active() ? 1 : 0; }

// "All effects": off switches the distance tweaks, soft shadows, AO (and its debug view) and contact
// shadows off together, for before/after comparisons; on puts back whatever was on before.
bool g_allOn = true;
struct
{
	bool tweaks, soft, ao, cs;
	int view;
} g_allSaved;

void SetAllOn(bool on)
{
	if (on == g_allOn)
		return;
	g_allOn = on;
	if (!on)
	{
		g_allSaved = { Shadows_TweaksOn(), SoftShadows_Active(), AO_Active(), CS_Active(), AO_View() };
		if (Shadows_Installed())
			Shadows_SetTweaksOn(false);
		SoftShadows_SetActive(false);
		if (AO_Enabled())
		{
			AO_SetView(0);
			AO_SetOn(false);
			CS_SetOn(false);
		}
	}
	else
	{
		if (Shadows_Installed())
			Shadows_SetTweaksOn(g_allSaved.tweaks);
		SoftShadows_SetActive(g_allSaved.soft);
		if (AO_Enabled())
		{
			AO_SetOn(g_allSaved.ao);
			AO_SetView(g_allSaved.view);
			CS_SetOn(g_allSaved.cs);
		}
	}
}

class ShadowMenu : public core::Menu
{
public:
	ShadowMenu() : Menu(kDef) {}

protected:
	// The frame dump entry only when [Debug] FrameDump=1.
	int Count() const override { return FrameDump_Available() ? ITEM_COUNT : ITEM_COUNT - 1; }

	void IniValue(int item, char* buf, size_t n) override
	{
		switch (item)
		{
		case ITEM_DISTANCE:
		case ITEM_SPLIT:
		case ITEM_CASTER:
		case ITEM_DRAW:
			snprintf(buf, n, "%.1f", Shadows_GetSetting(item));
			break;
		case ITEM_SOFT_WIDTH:
			snprintf(buf, n, "%d", SoftWidth());
			break;
		case ITEM_AO_MODE:
			snprintf(buf, n, "%d", AO_View());
			break;
		case ITEM_AO_RADIUS:
			snprintf(buf, n, "%.2f", AO_Radius());
			break;
		case ITEM_AO_STRENGTH:
			snprintf(buf, n, "%.2f", AO_Strength());
			break;
		case ITEM_CS_ON:
			snprintf(buf, n, "%d", CS_Active() ? 1 : 0);
			break;
		case ITEM_CS_LENGTH:
			snprintf(buf, n, "%.2f", CS_Length());
			break;
		default:
			buf[0] = 0;
			break;
		}
	}

	void AfterSave(int item) override
	{
		if (item == ITEM_AO_MODE)
			IniWrite("AmbientOcclusion", "StartEnabled", AO_Active() ? "1" : "0");
	}

	void Format(int item, char* buf, size_t n) override
	{
		switch (item)
		{
		case ITEM_DISTANCE:
		case ITEM_SPLIT:
		case ITEM_CASTER:
		case ITEM_DRAW:
			if (!Shadows_Installed())
				snprintf(buf, n, "n/a");
			else
				snprintf(buf, n, "x%.1f", Shadows_GetSetting(item));
			break;
		case ITEM_SOFT_WIDTH:
			if (SoftWidth())
				snprintf(buf, n, "%d (%d taps)", SoftWidth(), (SoftWidth() + 1) * (SoftWidth() + 1));
			else
				snprintf(buf, n, "OFF (game's filter)");
			break;
		case ITEM_AO_MODE:
			snprintf(buf, n, "%s", !AO_Enabled() ? "n/a (Enabled=0)" : kAoModes[AoMode()]);
			break;
		case ITEM_AO_RADIUS:
			snprintf(buf, n, "%.2f m", AO_Radius());
			break;
		case ITEM_AO_STRENGTH:
			snprintf(buf, n, "%.2f", AO_Strength());
			break;
		case ITEM_CS_ON:
			snprintf(buf, n, "%s", !AO_Enabled() ? "n/a (Enabled=0)" : CS_Active() ? "ON" : "OFF");
			break;
		case ITEM_CS_LENGTH:
			snprintf(buf, n, "%.2f m", CS_Length());
			break;
		case ITEM_ALL_ON:
			snprintf(buf, n, "%s", g_allOn ? "ON" : "OFF (vanilla)");
			break;
		default:
			snprintf(buf, n, "%s", FrameDump_Busy() ? "writing..." : "Right writes one");
			break;
		}
	}

	bool Change(int item, int dir) override
	{
		switch (item)
		{
		case ITEM_DISTANCE:
		case ITEM_SPLIT:
		case ITEM_CASTER:
		case ITEM_DRAW:
		{
			if (!Shadows_Installed())
				return false;
			float v = Shadows_GetSetting(item) + 0.5f * dir;
			v = v < 0.5f ? 0.5f : v > 20.0f ? 20.0f : v;
			Shadows_SetSetting(item, v);
			return true;
		}
		case ITEM_SOFT_WIDTH:
		{
			int w = SoftWidth() + dir;
			w = w < 0 ? 0 : w > 4 ? 4 : w;
			SoftShadows_SetActive(w > 0);
			if (w > 0)
				SoftShadows_SetLiveWidth(w);
			return true;
		}
		case ITEM_AO_MODE:
		{
			if (!AO_Enabled())
				return false;
			int mode = AoMode() + dir;
			mode = mode < 0 ? 0 : mode > 5 ? 5 : mode;
			AO_SetOn(mode >= 1);
			AO_SetView(mode >= 2 ? mode - 1 : 0);
			return true;
		}
		case ITEM_AO_RADIUS:
			AO_SetRadius(AO_Radius() + 0.25f * dir);
			return true;
		case ITEM_AO_STRENGTH:
			AO_SetStrength(AO_Strength() + 0.25f * dir);
			return true;
		case ITEM_CS_ON:
			if (!AO_Enabled())
				return false;
			CS_SetOn(dir > 0);
			return true;
		case ITEM_CS_LENGTH:
			// 0.05 m steps up to half a metre, 0.25 m above
			CS_SetLength(CS_Length() + ((dir > 0 ? CS_Length() < 0.499f : CS_Length() < 0.501f) ? 0.05f : 0.25f) * dir);
			return true;
		case ITEM_ALL_ON:
			SetAllOn(dir > 0);
			return true;
		default: // frame dump: Right writes one (not a setting, nothing to log or save)
			if (dir > 0)
			{
				LogInfo("Menu: frame dump requested.");
				FrameDump_Request();
			}
			return false;
		}
	}
};

ShadowMenu g_menu;

} // namespace

// ---- Init --------------------------------------------------------------------------------------

bool Tuning_Enabled() { return g_menu.Enabled(); }

void Tuning_OnPresent(IDirect3DDevice9* dev) { g_menu.OnPresent(dev); }

void Tuning_Flush() { g_menu.Flush(); }

bool Tuning_Init()
{
	if (!g_menu.Init())
		return false;
	if (IniInt("Menu", "StartVisible", 0) != 0)
		g_menu.Show();
	SoftShadows_SetBuildAllWidths(true);
	return true;
}

void Tuning_SetAllOn(bool on) { SetAllOn(on); }
bool Tuning_AllOn() { return g_allOn; }
