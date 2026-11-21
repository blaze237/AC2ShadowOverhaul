// In-game settings menu ([Menu] TAAMenuKey, default F1), with a status line. Changes apply at once.
// The keys, drawing and saving are the shared menu engine's (core/menu.h).
#include "menu.h"
#include "common.h"
#include "taa.h"
#include "core/menu.h"
#include <d3d9.h>
#include <stdio.h>
#include <math.h>

namespace taa
{

enum
{
	ITEM_ON,
	ITEM_SHARPNESS,
	ITEM_FEEDBACK,
	ITEM_MOTION_FEEDBACK,
	ITEM_MIP_BIAS,
	ITEM_JITTER,
	ITEM_CLIP,
	ITEM_DEPTH_TOL,
	ITEM_ASSIST,
	ITEM_DEBUG,
	ITEM_COUNT
};

namespace
{

const core::MenuItem kItems[ITEM_COUNT] = {
	{ "TAA", "TAA", "StartEnabled", true },
	{ "Sharpness", "TAA", "Sharpness", true },
	{ "Feedback (smoothness)", "TAA", "Feedback", true },
	{ "Feedback in motion", "TAA", "MotionFeedback", true },
	{ "Texture mip bias", "TAA", "MipBias", true },
	{ "Jitter size", "TAA", "JitterScale", true },
	{ "Clip box", "TAA", "ClipBox", true },
	{ "Depth tolerance", "TAA", "DepthTolerance", true },
	{ "Average other effects' noise", "TAA", "AssistEffects", true },
	{ "Debug view", "TAA", "DebugView", true },
};
// Saves aren't logged (each change is).
const core::MenuDef kDef = { core::MENU_TAA, "AC2 Shadow Overhaul - TAA", "TAA menu", "TAAMenuKey", VK_F1, kItems, ITEM_COUNT, false, false };
const float kStep[ITEM_COUNT] = { 0, 0.05f, 0.01f, 0.01f, 0.1f, 0.1f, 0.05f, 0.005f, 0, 0 };
const char* const kDebugViews[] = { "off", "history source", "motion" };

float* FloatOf(TaaSettings& t, int item)
{
	switch (item)
	{
	case ITEM_SHARPNESS: return &t.sharpness;
	case ITEM_FEEDBACK: return &t.feedback;
	case ITEM_MOTION_FEEDBACK: return &t.motionFeedback;
	case ITEM_MIP_BIAS: return &t.mipBias;
	case ITEM_JITTER: return &t.jitterScale;
	case ITEM_CLIP: return &t.clipBox;
	case ITEM_DEPTH_TOL: return &t.depthTolerance;
	}
	return nullptr;
}

class TaaMenu : public core::Menu
{
public:
	TaaMenu() : Menu(kDef) {}

protected:
	void Format(int item, char* buf, size_t n) override
	{
		TaaSettings t = Taa_GetSettings();
		if (item == ITEM_ON || item == ITEM_ASSIST)
			snprintf(buf, n, "%s", (item == ITEM_ON ? t.on : t.assist) ? "ON" : "OFF");
		else if (item == ITEM_DEBUG)
			snprintf(buf, n, "%s", kDebugViews[t.debugView]);
		else
			snprintf(buf, n, item == ITEM_DEPTH_TOL ? "%.3f" : "%.2f", *FloatOf(t, item));
	}

	// Ini text for an item (what Format shows, but plain numbers).
	void IniValue(int item, char* buf, size_t n) override
	{
		TaaSettings t = Taa_GetSettings();
		if (item == ITEM_ON || item == ITEM_ASSIST)
			snprintf(buf, n, "%d", (item == ITEM_ON ? t.on : t.assist) ? 1 : 0);
		else if (item == ITEM_DEBUG)
			snprintf(buf, n, "%d", t.debugView);
		else
			snprintf(buf, n, item == ITEM_DEPTH_TOL ? "%.3f" : "%.2f", *FloatOf(t, item));
	}

	bool Change(int item, int dir) override
	{
		TaaSettings t = Taa_GetSettings();
		if (item == ITEM_ON)
			t.on = dir > 0;
		else if (item == ITEM_ASSIST)
			t.assist = dir > 0;
		else if (item == ITEM_DEBUG)
			t.debugView = (t.debugView + dir + 3) % 3;
		else
		{
			float* v = FloatOf(t, item);
			*v = roundf((*v + dir * kStep[item]) / kStep[item]) * kStep[item]; // stay on the step grid
		}
		Taa_SetSettings(t); // clamps; Format then shows the clamped value
		return true;
	}

	bool Status(char* buf, size_t n) override
	{
		snprintf(buf, n, "%s", Taa_Status());
		return true;
	}
};

} // namespace

static TaaMenu g_menu;

void Menu_OnPresent(IDirect3DDevice9* dev) { g_menu.OnPresent(dev); }

void Menu_Flush() { g_menu.Flush(); }

void Menu_Init() { g_menu.Init(); }

} // namespace taa
