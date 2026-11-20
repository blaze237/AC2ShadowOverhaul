// In-game menu: live tuning of the fog and the shafts ([Menu] VolumetricsMenuKey, default F11), with
// a status line. The keys, drawing and saving are the shared menu engine's (core/menu.h).

#include "common.h"
#include "volumetrics.h"
#include "core/menu.h"
#include <d3d9.h>
#include <stdio.h>

namespace vol
{

namespace
{

enum
{
	ITEM_FOG,
	ITEM_DENSITY,
	ITEM_FALLOFF,
	ITEM_BASE,
	ITEM_SUN,
	ITEM_ANISO,
	ITEM_AMBIENT,
	ITEM_DISTANCE,
	ITEM_STEPS,
	ITEM_SHAFTS,
	ITEM_SHAFT_INTENSITY,
	ITEM_SHAFT_LENGTH,
	ITEM_SHAFT_THRESHOLD,
	ITEM_ALL, // master switch for comparisons; not saved
	ITEM_COUNT
};
const core::MenuItem kItems[ITEM_COUNT] = {
	{ "Volumetric fog", "VolumetricFog", "StartEnabled", true }, // DebugView too (AfterSave)
	{ "Fog density", "VolumetricFog", "Density", true },
	{ "Height falloff", "VolumetricFog", "HeightFalloff", true },
	{ "Base height", "VolumetricFog", "BaseHeight", true },
	{ "Sun scattering (god rays)", "VolumetricFog", "SunIntensity", true },
	{ "Sun focus", "VolumetricFog", "Anisotropy", true },
	{ "Sky light scattering", "VolumetricFog", "AmbientIntensity", true },
	{ "Fog distance", "VolumetricFog", "MaxDistance", true },
	{ "Ray march steps", "VolumetricFog", "Steps", true },
	{ "Sun shafts", "SunShafts", "StartEnabled", true }, // [VolumetricFog] DebugView too
	{ "Shaft intensity", "SunShafts", "Intensity", true },
	{ "Shaft length", "SunShafts", "Length", true },
	{ "Shaft sky threshold", "SunShafts", "Threshold", true },
	{ "All volumetric effects", nullptr, nullptr, true },
};
const core::MenuDef kDef = { core::MENU_VOLUMETRICS, "AC2 Shadow Overhaul - Volumetrics", "Volumetrics menu", "VolumetricsMenuKey", VK_F11,
	kItems, ITEM_COUNT, false, true };

// Fog modes: off, on, then the debug views. Shaft modes: off, on, shafts only.
int FogMode() { return g_set.view == 1 ? 2 : g_set.view == 2 ? 3 : g_set.fogOn ? 1 : 0; }
int ShaftMode() { return g_set.view == 3 ? 2 : g_set.shaftsOn ? 1 : 0; }
const char* const kFogModes[] = { "OFF", "ON", "show fog only", "show fog transmittance" };
const char* const kShaftModes[] = { "OFF", "ON", "show shafts only" };

bool g_allOn = true;
struct
{
	bool fog, shafts;
	int view;
} g_allSaved;

void SetAllOn(bool on)
{
	if (on == g_allOn)
		return;
	g_allOn = on;
	if (!on)
	{
		g_allSaved = { g_set.fogOn, g_set.shaftsOn, g_set.view };
		g_set.fogOn = g_set.shaftsOn = false;
		g_set.view = 0;
	}
	else
	{
		g_set.fogOn = g_allSaved.fog;
		g_set.shaftsOn = g_allSaved.shafts;
		g_set.view = g_allSaved.view;
	}
}

class VolMenu : public core::Menu
{
public:
	VolMenu() : Menu(kDef) {}

protected:
	void IniValue(int item, char* buf, size_t n) override
	{
		switch (item)
		{
		case ITEM_FOG: snprintf(buf, n, "%d", g_set.fogOn ? 1 : 0); break;
		case ITEM_DENSITY: snprintf(buf, n, "%.5f", g_set.density); break;
		case ITEM_FALLOFF: snprintf(buf, n, "%.4f", g_set.heightFalloff); break;
		case ITEM_BASE: snprintf(buf, n, "%.1f", g_set.baseHeight); break;
		case ITEM_SUN: snprintf(buf, n, "%.2f", g_set.sunIntensity); break;
		case ITEM_ANISO: snprintf(buf, n, "%.2f", g_set.anisotropy); break;
		case ITEM_AMBIENT: snprintf(buf, n, "%.2f", g_set.ambientIntensity); break;
		case ITEM_DISTANCE: snprintf(buf, n, "%.0f", g_set.maxDistance); break;
		case ITEM_STEPS: snprintf(buf, n, "%d", g_set.steps); break;
		case ITEM_SHAFTS: snprintf(buf, n, "%d", g_set.shaftsOn ? 1 : 0); break;
		case ITEM_SHAFT_INTENSITY: snprintf(buf, n, "%.2f", g_set.shaftsIntensity); break;
		case ITEM_SHAFT_LENGTH: snprintf(buf, n, "%.2f", g_set.shaftsLength); break;
		case ITEM_SHAFT_THRESHOLD: snprintf(buf, n, "%.2f", g_set.shaftsThreshold); break;
		default: buf[0] = 0; break;
		}
	}

	// The fog and shaft modes include the debug view.
	void AfterSave(int item) override
	{
		if (item == ITEM_FOG || item == ITEM_SHAFTS)
		{
			char view[8];
			snprintf(view, sizeof(view), "%d", g_set.view);
			IniWrite("VolumetricFog", "DebugView", view);
		}
	}

	void Format(int item, char* buf, size_t n) override
	{
		switch (item)
		{
		case ITEM_FOG: snprintf(buf, n, "%s", !g_set.fogConfigured ? "n/a (Enabled=0)" : kFogModes[FogMode()]); break;
		case ITEM_DENSITY: snprintf(buf, n, "%.4f /m", g_set.density); break;
		case ITEM_FALLOFF: snprintf(buf, n, "%.4f /m (halves every %.0f m)", g_set.heightFalloff, g_set.heightFalloff > 0 ? 0.693f / g_set.heightFalloff : 0.0f); break;
		case ITEM_BASE: snprintf(buf, n, "%.0f m", g_set.baseHeight); break;
		case ITEM_SUN: snprintf(buf, n, "%.2f", g_set.sunIntensity); break;
		case ITEM_ANISO: snprintf(buf, n, "%.2f", g_set.anisotropy); break;
		case ITEM_AMBIENT: snprintf(buf, n, "%.2f", g_set.ambientIntensity); break;
		case ITEM_DISTANCE: snprintf(buf, n, "%.0f m", g_set.maxDistance); break;
		case ITEM_STEPS: snprintf(buf, n, "%d", g_set.steps); break;
		case ITEM_SHAFTS: snprintf(buf, n, "%s", !g_set.shaftsConfigured ? "n/a (Enabled=0)" : kShaftModes[ShaftMode()]); break;
		case ITEM_SHAFT_INTENSITY: snprintf(buf, n, "%.2f", g_set.shaftsIntensity); break;
		case ITEM_SHAFT_LENGTH: snprintf(buf, n, "%.2f", g_set.shaftsLength); break;
		case ITEM_SHAFT_THRESHOLD: snprintf(buf, n, "%.2f", g_set.shaftsThreshold); break;
		default: snprintf(buf, n, "%s", g_allOn ? "ON" : "OFF (vanilla)"); break;
		}
	}

	bool Change(int item, int dir) override
	{
		switch (item)
		{
		case ITEM_FOG:
		{
			if (!g_set.fogConfigured)
				return false;
			int mode = FogMode() + dir;
			mode = mode < 0 ? 0 : mode > 3 ? 3 : mode;
			g_set.fogOn = mode >= 1;
			g_set.view = mode == 2 ? 1 : mode == 3 ? 2 : g_set.view == 3 ? 3 : 0;
			break;
		}
		case ITEM_DENSITY: g_set.density = dir > 0 ? (g_set.density < 1e-4f ? 1e-4f : g_set.density * 1.25f) : g_set.density / 1.25f; break;
		case ITEM_FALLOFF:
			g_set.heightFalloff = dir > 0 ? (g_set.heightFalloff < 1e-3f ? 1e-3f : g_set.heightFalloff * 1.25f)
										  : (g_set.heightFalloff <= 1e-3f ? 0.0f : g_set.heightFalloff / 1.25f);
			break;
		case ITEM_BASE: g_set.baseHeight = g_set.baseHeight + 5.0f * dir; break;
		case ITEM_SUN: g_set.sunIntensity = g_set.sunIntensity + 0.1f * dir; break;
		case ITEM_ANISO: g_set.anisotropy = g_set.anisotropy + 0.05f * dir; break;
		case ITEM_AMBIENT: g_set.ambientIntensity = g_set.ambientIntensity + 0.05f * dir; break;
		case ITEM_DISTANCE: // 25 m steps up to 500 m, 100 m above
			g_set.maxDistance = g_set.maxDistance + ((dir > 0 ? g_set.maxDistance < 499.0f : g_set.maxDistance < 501.0f) ? 25.0f : 100.0f) * dir;
			break;
		case ITEM_STEPS: g_set.steps = g_set.steps + 8 * dir; break;
		case ITEM_SHAFTS:
		{
			if (!g_set.shaftsConfigured)
				return false;
			int mode = ShaftMode() + dir;
			mode = mode < 0 ? 0 : mode > 2 ? 2 : mode;
			g_set.shaftsOn = mode >= 1;
			g_set.view = mode == 2 ? 3 : g_set.view == 3 ? 0 : g_set.view;
			break;
		}
		case ITEM_SHAFT_INTENSITY: g_set.shaftsIntensity = g_set.shaftsIntensity + 0.1f * dir; break;
		case ITEM_SHAFT_LENGTH: g_set.shaftsLength = g_set.shaftsLength + 0.05f * dir; break;
		case ITEM_SHAFT_THRESHOLD: g_set.shaftsThreshold = g_set.shaftsThreshold + 0.05f * dir; break;
		default: SetAllOn(dir > 0); break;
		}
		Vol_ClampSettings();
		return true;
	}

	bool Status(char* buf, size_t n) override
	{
		snprintf(buf, n, "%s", Vol_Status());
		return true;
	}
};

VolMenu g_menu;

} // namespace

void Menu_OnPresent(IDirect3DDevice9* dev) { g_menu.OnPresent(dev); }

void Menu_Flush() { g_menu.Flush(); }

bool Menu_Init() { return g_menu.Init(); }

} // namespace vol
