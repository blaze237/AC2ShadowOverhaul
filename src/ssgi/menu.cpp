// In-game menu: live tuning of the SSGI settings ([Menu] SSGIMenuKey, default F3). A status line
// shows whether the passes are running and what they cost on the GPU (measured only while the menu
// is shown). The keys, drawing and saving are the shared menu engine's (core/menu.h).

#include "common.h"
#include "gi.h"
#include "menu.h"
#include "core/menu.h"
#include <d3d9.h>
#include <stdio.h>
#include <math.h>

namespace ssgi
{

namespace
{

// The items are the settings, in GiSettingId order, all in [SSGI].
const core::MenuItem kItems[GS_COUNT] = {
	{ "SSGI", "SSGI", "StartEnabled", true },
	{ "Debug view", "SSGI", "DebugView", true },
	{ "Intensity", "SSGI", "Intensity", true },
	{ "Radius", "SSGI", "Radius", true },
	{ "Max screen radius", "SSGI", "MaxScreenRadius", true },
	{ "Thickness", "SSGI", "Thickness", true },
	{ "Slices", "SSGI", "Slices", true },
	{ "Steps", "SSGI", "Steps", true },
	{ "Temporal blend", "SSGI", "TemporalBlend", true },
	{ "Radiance clamp", "SSGI", "RadianceClamp", true },
};
const core::MenuDef kDef = { core::MENU_SSGI, "AC2 Shadow Overhaul - SSGI", "SSGI menu", "SSGIMenuKey", VK_F3, kItems, GS_COUNT, false, true };

// How each value is shown and saved.
const struct
{
	const char* unit;
	int decimals;
} kFormat[GS_COUNT] = {
	{ "", 0 }, { "", 0 }, { "", 2 }, { " m", 1 }, { " x height", 2 }, { " m", 2 }, { "", 0 }, { "", 0 }, { "", 2 }, { "", 1 },
};
const char* const kViews[] = { "off", "GI light", "raw trace", "normals" };

// Step for a change from value v in direction dir (finer steps at the low end of each range).
float Step(int id, float v, int dir)
{
	bool up = dir > 0;
	switch (id)
	{
	case GS_INTENSITY: return (up ? v < 0.999f : v < 1.001f) ? 0.05f : 0.25f;
	case GS_RADIUS: return (up ? v < 3.999f : v < 4.001f) ? 0.5f : 1.0f;
	case GS_MAX_SCREEN: return 0.05f;
	case GS_THICKNESS: return (up ? v < 0.499f : v < 0.501f) ? 0.05f : 0.25f;
	case GS_BLEND: return 0.02f;
	case GS_CLAMP: return (up ? v < 7.999f : v < 8.001f) ? 0.5f : 4.0f;
	default: return 1.0f;
	}
}

class SsgiMenu : public core::Menu
{
public:
	SsgiMenu() : Menu(kDef) {}

protected:
	void Format(int id, char* buf, size_t n) override
	{
		float v = GI_GetSetting(id);
		if (id == GS_ON)
			snprintf(buf, n, "%s", v > 0.5f ? "ON" : "OFF");
		else if (id == GS_VIEW)
			snprintf(buf, n, "%s", kViews[(int)v & 3]);
		else
			snprintf(buf, n, "%.*f%s", kFormat[id].decimals, (double)v, kFormat[id].unit);
	}

	void IniValue(int id, char* buf, size_t n) override { snprintf(buf, n, "%.*f", kFormat[id].decimals, (double)GI_GetSetting(id)); }

	bool Change(int id, int dir) override
	{
		float v = GI_GetSetting(id);
		if (id == GS_ON)
			v = dir > 0 ? 1.0f : 0.0f;
		else if (id == GS_VIEW)
			v = (float)(((int)v + dir + 4) % 4);
		else
		{
			// Round to the step so repeated changes land on tidy values.
			float step = Step(id, v, dir);
			v = floorf((v + step * dir) / step + 0.5f) * step;
		}
		GI_SetSetting(id, v);
		return true;
	}

	// "tracing 960x540, GPU 0.42 ms" / why not.
	bool Status(char* buf, size_t n) override
	{
		static unsigned lastFrames = 0;
		static DWORD lastSeen = 0;
		GiStatus st = GI_GetStatus();
		DWORD now = GetTickCount();
		if (st.framesTraced != lastFrames)
		{
			lastFrames = st.framesTraced;
			lastSeen = now;
		}
		bool running = lastSeen && now - lastSeen < 500;
		if (GI_GetSetting(GS_ON) < 0.5f)
			snprintf(buf, n, "off (the first setting turns it on)");
		else if (st.failed)
			snprintf(buf, n, "FAILED: could not create its textures (see AC2ShadowOverhaul.log)");
		else if (!running)
			snprintf(buf, n, "not running: no opaque pass seen (in-game anti-aliasing on? menus?)");
		else if (st.gpuMs >= 0)
			snprintf(buf, n, "tracing %ux%u, GPU %.2f ms", st.traceW, st.traceH, (double)st.gpuMs);
		else
			snprintf(buf, n, "tracing %ux%u", st.traceW, st.traceH);
		return true;
	}

	// The GPU time is measured only while the menu is shown.
	void OnVisibleChanged(bool visible) override { GI_SetTiming(visible); }
};

SsgiMenu g_menu;

} // namespace

void Menu_OnPresent(IDirect3DDevice9* dev) { g_menu.OnPresent(dev); }

void Menu_Flush() { g_menu.Flush(); }

bool Menu_Init()
{
	if (!g_menu.Init())
		return false;
	GI_SetTiming(false);
	return true;
}

} // namespace ssgi
