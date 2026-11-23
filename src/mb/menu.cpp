// In-game settings menu ([Menu] MotionBlurMenuKey, default F6), with a status line. Changes apply
// at once. The keys, drawing and saving are the shared menu engine's (core/menu.h).
#include "menu.h"
#include "motionblur.h"
#include "core/menu.h"
#include <stdio.h>
#include <math.h>

namespace mb
{

enum
{
	ITEM_ON,
	ITEM_STRENGTH,
	ITEM_SHUTTER,
	ITEM_MAX,
	ITEM_SAMPLES,
	ITEM_DEBUG,
	ITEM_COUNT
};

namespace
{

const core::MenuItem kItems[ITEM_COUNT] = {
	{ "Motion blur", "MotionBlur", "StartEnabled", true },
	{ "Strength", "MotionBlur", "Strength", true },
	{ "Shutter (blur length)", "MotionBlur", "Shutter", true },
	{ "Longest blur (px at 1080p)", "MotionBlur", "MaxBlur", true },
	{ "Samples", "MotionBlur", "Samples", true },
	{ "Debug view", "MotionBlur", "DebugView", true },
};
// Saves aren't logged (each change is).
const core::MenuDef kDef = { core::MENU_MOTIONBLUR, "AC2 Shadow Overhaul - Motion blur", "Motion blur menu", "MotionBlurMenuKey", VK_F6, kItems,
	ITEM_COUNT, false, false };
const char* const kDebugViews[] = { "off", "velocity" };

float* FloatOf(MbSettings& s, int item)
{
	switch (item)
	{
	case ITEM_STRENGTH: return &s.strength;
	case ITEM_SHUTTER: return &s.shutter;
	}
	return nullptr;
}

class MbMenu : public core::Menu
{
public:
	MbMenu() : Menu(kDef) {}

protected:
	void Format(int item, char* buf, size_t n) override
	{
		MbSettings s = Mb_GetSettings();
		if (item == ITEM_ON)
			snprintf(buf, n, "%s", s.on ? "ON" : "OFF");
		else if (item == ITEM_MAX)
			snprintf(buf, n, "%d", s.maxBlur);
		else if (item == ITEM_SAMPLES)
			snprintf(buf, n, "%d", s.samples);
		else if (item == ITEM_DEBUG)
			snprintf(buf, n, "%s", kDebugViews[s.debugView]);
		else
			snprintf(buf, n, "%.2f", *FloatOf(s, item));
	}

	// Ini text for an item (what Format shows, but plain numbers).
	void IniValue(int item, char* buf, size_t n) override
	{
		MbSettings s = Mb_GetSettings();
		if (item == ITEM_ON)
			snprintf(buf, n, "%d", s.on ? 1 : 0);
		else if (item == ITEM_MAX)
			snprintf(buf, n, "%d", s.maxBlur);
		else if (item == ITEM_SAMPLES)
			snprintf(buf, n, "%d", s.samples);
		else if (item == ITEM_DEBUG)
			snprintf(buf, n, "%d", s.debugView);
		else
			snprintf(buf, n, "%.2f", *FloatOf(s, item));
	}

	bool Change(int item, int dir) override
	{
		MbSettings s = Mb_GetSettings();
		const MbSettings before = s;
		if (item == ITEM_ON)
			s.on = dir > 0;
		else if (item == ITEM_MAX)
			s.maxBlur += 4 * dir;
		else if (item == ITEM_SAMPLES)
			s.samples += 4 * dir;
		else if (item == ITEM_DEBUG)
			s.debugView = (s.debugView + dir + 2) % 2;
		else
		{
			const float step = item == ITEM_SHUTTER ? 0.05f : 0.1f;
			float* v = FloatOf(s, item);
			*v = roundf((*v + dir * step) / step) * step; // stay on the step grid
		}
		Mb_SetSettings(s); // clamps; Format then shows the clamped value
		s = Mb_GetSettings();
		return s.on != before.on || s.strength != before.strength || s.shutter != before.shutter || s.maxBlur != before.maxBlur || s.samples != before.samples || s.debugView != before.debugView;
	}

	bool Status(char* buf, size_t n) override
	{
		snprintf(buf, n, "%s", Mb_Status());
		return true;
	}
};

} // namespace

static MbMenu g_menu;

void Menu_OnPresent(IDirect3DDevice9* dev) { g_menu.OnPresent(dev); }

void Menu_Flush() { g_menu.Flush(); }

void Menu_Init() { g_menu.Init(); }

} // namespace mb
