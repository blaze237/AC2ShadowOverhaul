// The merged effects behind the one Direct3D hook layer: see features.h for the frame order.
//
// Ambient occlusion and contact shadows are called from framedump.cpp directly (they were part of
// this plugin from the start); SSGI, volumetrics and TAA come in here.

#include "features.h"
#include "common.h"
#include "ao.h"
#include "ssgi/gi.h"
#include "ssgi/menu.h"
#include "vol/volumetrics.h"
#include "taa/taa.h"
#include "taa/menu.h"
#include "taa/vsjitter.h"
#include "mb/motionblur.h"
#include "mb/menu.h"
#include "core/scene.h"
#include "core/menu.h"
#include "core/gputimer.h"
#include <stdio.h>
#include <string.h>
#include <string>

volatile bool g_fxPassthrough = false;

static bool g_gi = false, g_vol = false, g_taa = false, g_mb = false; // configured on (hooks and shader rewrites set up)
static bool g_volMenu = false;
static int g_allKey = VK_F12; // [Menu] AllEffectsKey

bool Fx_Init()
{
	g_gi = ssgi::GI_Init();
	ssgi::Menu_Init();
	g_vol = vol::Vol_Init();
	g_volMenu = vol::Menu_Init();
	g_taa = taa::Taa_Init();
	taa::Menu_Init();
	g_mb = mb::Mb_Init();
	mb::Menu_Init();
	g_allKey = IniInt("Menu", "AllEffectsKey", VK_F12);
	if (g_allKey > 0 && g_allKey < 256)
		LogInfo("All-effects key: 0x%02X switches every effect off and back on (not saved).", g_allKey);
	core::GpuTimer_SetOn(IniInt("Debug", "ShowFrameTimes", 0) != 0);
	return g_gi || g_vol || g_taa || g_mb;
}

void Fx_Flush()
{
	ssgi::Menu_Flush();
	vol::Menu_Flush();
	taa::Menu_Flush();
	mb::Menu_Flush();
}

// ---- MSAA warning ------------------------------------------------------------------------------
//
// Every screen-space effect here reads the game's z/w target and scene colour, which can't be used
// while the game's own anti-aliasing (MSAA) is on; each effect switches itself off then. Rather
// than leave that in the log, a red warning is drawn while it happens and any of them is on.

static bool g_ppMsaa = false; // the back buffer is multisampled (the main pass is checked by core/scene)

static void NotePresentParams(const D3DPRESENT_PARAMETERS* pp)
{
	g_ppMsaa = pp && pp->MultiSampleType != D3DMULTISAMPLE_NONE;
	if (g_ppMsaa)
		LogWarn("The back buffer is multisampled (in-game anti-aliasing is on): ambient occlusion, contact shadows, SSGI, "
				"volumetrics, TAA and motion blur need it off.");
}

// The effects that are on and need MSAA off, as a list for the warning ("" if none).
static std::string MsaaBlockedEffects()
{
	std::string list;
	auto add = [&](bool on, const char* name) {
		if (!on)
			return;
		if (!list.empty())
			list += ", ";
		list += name;
	};
	add(AO_Active(), "ambient occlusion");
	add(CS_Active(), "contact shadows");
	add(g_gi && ssgi::GI_Active(), "SSGI");
	add(g_vol && vol::g_set.fogOn, "volumetric fog");
	add(g_vol && vol::g_set.shaftsOn, "sun shafts");
	add(g_taa && taa::Taa_Active(), "TAA");
	add(g_mb && mb::Mb_Active(), "motion blur");
	return list;
}

// ---- The all-effects key -------------------------------------------------------------------------------
//
// [Menu] AllEffectsKey (F12) switches every effect off at once for a comparison with the vanilla
// game, and back to what was on before. Not saved. The shadow and volumetrics groups use their menus'
// own "All ..." switches (which keep what they had); SSGI, TAA and motion blur are remembered here.

static bool g_allOff = false;
static struct
{
	bool gi, taa, mb;
} g_allSaved;

static void SetAllEffects(bool on)
{
	if (on == !g_allOff)
		return;
	g_allOff = !on;
	Tuning_SetAllOn(on);
	if (g_vol)
		vol::Menu_SetAllOn(on);
	if (!on)
	{
		g_allSaved.gi = g_gi && ssgi::GI_Active();
		g_allSaved.taa = g_taa && taa::Taa_GetSettings().on;
		g_allSaved.mb = g_mb && mb::Mb_GetSettings().on;
	}
	if (g_gi)
		ssgi::GI_SetSetting(ssgi::GS_ON, on && g_allSaved.gi ? 1.0f : 0.0f);
	if (g_taa)
	{
		taa::TaaSettings t = taa::Taa_GetSettings();
		t.on = on && g_allSaved.taa;
		taa::Taa_SetSettings(t);
	}
	if (g_mb)
	{
		mb::MbSettings m = mb::Mb_GetSettings();
		m.on = on && g_allSaved.mb;
		mb::Mb_SetSettings(m);
	}
	LogInfo("All-effects key: every effect %s.", on ? "back as it was" : "off (vanilla)");
}

static core::TextFont g_noteFont;

// Polls the key; while everything is off, a small note in the top-left corner says so.
static void PollAllEffectsKey(IDirect3DDevice9* dev)
{
	static bool was = false;
	bool down = g_allKey > 0 && g_allKey < 256 && core::MenuKeyHeld(g_allKey);
	if (down && !was)
		SetAllEffects(g_allOff);
	was = down;
	if (!g_allOff)
		return;
	core::BackBufferOverlay o(dev);
	if (!o.Ok())
		return;
	UINT height = o.Height() / 60;
	if (!g_noteFont.Ensure(dev, height < 12 ? 12 : height) || !o.Begin())
		return;
	const char* text = "AC2 Shadow Overhaul: all effects OFF (F12)";
	const int h = (int)g_noteFont.Height();
	const DWORD fmt = DT_LEFT | DT_TOP | DT_NOCLIP | DT_SINGLELINE;
	g_noteFont.Draw(text, { h / 2 + 1, h / 2 + 1, (int)o.Width(), 2 * h }, fmt, D3DCOLOR_ARGB(220, 0, 0, 0));
	g_noteFont.Draw(text, { h / 2, h / 2, (int)o.Width(), 2 * h }, fmt, D3DCOLOR_XRGB(255, 220, 80));
}

// ---- Frame times ---------------------------------------------------------------------------------
//
// [Debug] ShowFrameTimes (the F9 menu's "Frame times"): the GPU time of each effect that is running,
// and their total, in the top-left corner (below the all-effects note when that shows).

static void DrawFrameTimes(IDirect3DDevice9* dev)
{
	if (!core::GpuTimer_On())
		return;
	core::GpuTimes t = core::GpuTimer_Get();
	char lines[core::GT_COUNT + 3][96];
	D3DCOLOR colours[core::GT_COUNT + 3];
	int n = 0;
	auto line = [&](D3DCOLOR c) -> char* {
		colours[n] = c;
		return lines[n++];
	};
	const D3DCOLOR white = D3DCOLOR_XRGB(255, 255, 255), light = D3DCOLOR_XRGB(220, 220, 220), yellow = D3DCOLOR_XRGB(255, 220, 80),
				   grey = D3DCOLOR_XRGB(170, 170, 170);
	snprintf(line(white), sizeof(lines[0]), "GPU frame times (F9 menu)");
	if (t.failed)
		snprintf(line(light), sizeof(lines[0]), "not available: the driver has no timestamp queries");
	else if (!t.valid)
		snprintf(line(light), sizeof(lines[0]), "measuring...");
	else
	{
		for (int id = 0; id < core::GT_COUNT; id++)
			if (t.active[id])
				snprintf(line(light), sizeof(lines[0]), "%-21s %6.2f ms", core::GpuTimer_Name(id), (double)t.ms[id]);
		if (n == 1)
			snprintf(line(light), sizeof(lines[0]), "no effect is running");
		snprintf(line(yellow), sizeof(lines[0]), "%-21s %6.2f ms", "Total", (double)t.totalMs);
		if (t.frameMs > 0)
			snprintf(line(grey), sizeof(lines[0]), "%-21s %6.2f ms (%.0f fps)", "Whole frame", (double)t.frameMs, 1000.0 / t.frameMs);
	}

	core::BackBufferOverlay o(dev);
	if (!o.Ok())
		return;
	UINT height = o.Height() / 60;
	if (!g_noteFont.Ensure(dev, height < 12 ? 12 : height) || !o.Begin())
		return;
	const int h = (int)g_noteFont.Height(), w = (int)o.Width();
	const DWORD fmt = DT_LEFT | DT_TOP | DT_NOCLIP | DT_SINGLELINE;
	int y = g_allOff ? h * 2 : h / 2; // below the all-effects note
	for (int i = 0; i < n; i++)
	{
		g_noteFont.Draw(lines[i], { h / 2 + 1, y + 1, w, y + 2 * h }, fmt, D3DCOLOR_ARGB(220, 0, 0, 0));
		g_noteFont.Draw(lines[i], { h / 2, y, w, y + 2 * h }, fmt, colours[i]);
		y += h + (i == 0 ? h / 4 : 0);
	}
}

// Bigger than the menus' text, centred a sixth of the way down the screen.
static core::TextFont g_warnFont;

static void DrawWarning(IDirect3DDevice9* dev)
{
	bool msaa = g_ppMsaa || core::Scene_MsaaRecently();
	if (!msaa)
		return;
	std::string effects = MsaaBlockedEffects();
	if (effects.empty())
		return;
	static bool logged = false;
	if (!logged)
	{
		logged = true;
		LogWarn("In-game anti-aliasing (MSAA) is on, so these are off: %s. Showing the on-screen warning.", effects.c_str());
	}
	core::BackBufferOverlay o(dev);
	if (!o.Ok())
		return;
	UINT height = o.Height() / 40;
	if (!g_warnFont.Ensure(dev, height < 15 ? 15 : height) || !o.Begin())
		return;
	std::string line1 = "AC2 Shadow Overhaul: the game's anti-aliasing (MSAA) is on";
	std::string line2 = "These need it off and are not working: " + effects;
	std::string line3 = "Turn anti-aliasing off in the game's graphics options (or switch these effects off in their menus)";
	const int h = (int)g_warnFont.Height(), w = (int)o.Width(), shadow = h / 12 + 1;
	const DWORD fmt = DT_CENTER | DT_TOP | DT_NOCLIP | DT_SINGLELINE;
	int y = (int)o.Height() / 6;
	for (const std::string* s : { &line1, &line2, &line3 })
	{
		g_warnFont.Draw(s->c_str(), { shadow, y + shadow, w + shadow, y + 2 * h }, fmt, D3DCOLOR_ARGB(220, 0, 0, 0));
		g_warnFont.Draw(s->c_str(), { 0, y, w, y + 2 * h }, fmt, D3DCOLOR_XRGB(255, 40, 40));
		y += h * 5 / 4;
	}
}

// ---- Device lifetime ---------------------------------------------------------------------------

void Fx_OnDeviceCreated(IDirect3D9* d3d, UINT adapter, IDirect3DDevice9* dev, DWORD flags, const D3DPRESENT_PARAMETERS* pp)
{
	NotePresentParams(pp);
	if (flags & D3DCREATE_PUREDEVICE)
		LogWarn("The device is a pure device: shader constants can't be read back, so ambient occlusion, contact shadows, SSGI, "
				"volumetrics, TAA and motion blur can't find the camera.");
	if (g_vol)
		vol::Vol_OnDeviceReady(dev);
	if (g_taa)
		taa::Taa_OnDeviceCreated(d3d, adapter, dev);
	if (g_mb)
		mb::Mb_OnDeviceCreated(dev, flags);
}

void Fx_OnLostDevice()
{
	core::Menu_OnLostDevice();
	core::GpuTimer_OnLostDevice();
	g_warnFont.OnLostDevice();
	g_noteFont.OnLostDevice();
	if (g_gi)
		ssgi::GI_OnLostDevice();
	if (g_vol)
		vol::Vol_OnLostDevice();
	if (g_taa)
		taa::Taa_OnLostDevice();
	if (g_mb)
		mb::Mb_OnLostDevice();
}

void Fx_OnResetDevice(IDirect3DDevice9* dev, const D3DPRESENT_PARAMETERS* pp)
{
	NotePresentParams(pp);
	if (g_vol)
		vol::Vol_OnDeviceReady(dev);
	core::Menu_OnResetDevice();
	g_warnFont.OnResetDevice();
	g_noteFont.OnResetDevice();
}

// ---- Device calls ------------------------------------------------------------------------------

// While an effect handles a device call (and may run its passes), the hooks pass straight through:
// device calls an effect makes through the vtable (SSGI's and TAA's SetViewport, the volumetrics'
// SetTexture, ...) are its own, not the game's, so the other effects and the frame dump must not
// track them (the state blocks then restore the game's state behind the hooks' backs).
struct FxScope
{
	bool was;
	FxScope() : was(g_fxPassthrough) { g_fxPassthrough = true; }
	~FxScope() { g_fxPassthrough = was; }
};

void Fx_BeforePresent(IDirect3DDevice9* dev)
{
	FxScope scope;
	if (g_gi)
		ssgi::GI_BeforePresent(dev);
	if (g_vol)
		vol::Vol_BeforePresent(dev);
	if (g_taa)
		taa::Taa_OnPresent(dev);
	if (g_mb)
		mb::Mb_OnPresent(dev);
	core::GpuTimer_EndFrame(dev); // after every effect's passes of the frame
}

void Fx_DrawOverlays(IDirect3DDevice9* dev)
{
	g_fxPassthrough = true;
	ssgi::Menu_OnPresent(dev);
	if (g_volMenu)
		vol::Menu_OnPresent(dev);
	taa::Menu_OnPresent(dev);
	mb::Menu_OnPresent(dev);
	PollAllEffectsKey(dev);
	DrawFrameTimes(dev);
	DrawWarning(dev);
	g_fxPassthrough = false;
}

void Fx_BeforeCopyFrom(IDirect3DDevice9* dev, IDirect3DSurface9* src, bool stretchRect)
{
	FxScope scope;
	if (stretchRect)
	{
		if (g_gi)
			ssgi::GI_BeforeOther(dev);
		if (g_vol)
			vol::Vol_BeforeStretchRect(dev, src);
	}
	if (g_taa)
		taa::Taa_BeforeCopyFrom(dev, src);
	if (g_mb)
		mb::Mb_BeforeCopyFrom(dev, src); // after TAA's resolve: blurs the anti-aliased image
}

void Fx_BeforeOther(IDirect3DDevice9* dev)
{
	FxScope scope;
	if (g_gi)
		ssgi::GI_BeforeOther(dev);
}

void Fx_BeforeSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s)
{
	FxScope scope;
	if (g_gi)
		ssgi::GI_BeforeSetRenderTarget(dev, index, s);
	if (g_vol)
		vol::Vol_BeforeSetRenderTarget(dev, index, s);
}

void Fx_AfterSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s, IDirect3DSurface9* rt0)
{
	FxScope scope;
	if (g_gi && index < 4)
		ssgi::GI_AfterSetRenderTarget(dev, index, s, rt0);
	if (g_vol)
		vol::Vol_AfterSetRenderTarget(dev, index, s);
	if (g_taa)
		taa::Taa_AfterSetRenderTarget(dev, index, s);
}

void Fx_BeforeSetTexture(DWORD stage, IDirect3DBaseTexture9* tex)
{
	if (g_gi)
		ssgi::GI_OnSetTexture(stage, tex);
}

void Fx_AfterSetTexture(DWORD stage, IDirect3DBaseTexture9* tex)
{
	if (g_taa)
		taa::Taa_OnSetTexture(stage, tex);
	if (g_mb)
		mb::Mb_OnSetTexture(stage, tex);
}

DWORD Fx_SamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value)
{
	return g_taa ? taa::Taa_SamplerState(sampler, type, value) : value;
}

void Fx_AfterVertexConstants(IDirect3DDevice9* dev, UINT start, UINT count)
{
	if (g_taa)
		taa::Taa_AfterVertexConstants(dev, start, count);
}

void Fx_BeforeDraw(IDirect3DDevice9* dev)
{
	FxScope scope;
	if (g_gi)
		ssgi::GI_BeforeDraw(dev);
	if (g_vol)
		vol::Vol_BeforeDraw(dev);
	if (g_taa)
		taa::Taa_BeforeDraw(dev);
	if (g_mb)
		mb::Mb_BeforeDraw(dev); // after TAA's resolve
}

void Fx_AfterSetVertexShader(IDirect3DVertexShader9* vs)
{
	if (g_vol)
		vol::Vol_AfterSetVertexShader(vs);
}

void Fx_AfterSetPixelShader(IDirect3DPixelShader9* ps)
{
	if (g_vol)
		vol::Vol_AfterSetPixelShader(ps);
}

// ---- Shaders -----------------------------------------------------------------------------------

bool Fx_RewriteVS(const DWORD* code, std::vector<DWORD>& out)
{
	if (!g_taa || !code)
		return false;
	taa::VsJitterInfo info;
	return taa::Taa_RewriteVS(code, out, info);
}

void Fx_OnCreateVertexShader(IDirect3DVertexShader9* vs, const DWORD* gameCode)
{
	if (vs && gameCode && g_vol)
		vol::Vol_OnCreateVertexShader(vs, gameCode);
}

static LONG g_giRewritten = 0, g_giSkipped = 0;

bool Fx_GiShaders() { return g_gi; }

void Fx_NoteGiRewrite(uint32_t hash, bool applied, const std::string& reason)
{
	if (applied)
	{
		LONG n = InterlockedIncrement(&g_giRewritten);
		if (n == 1 || n % 50 == 0)
			LogInfo("SSGI: %ld opaque shaders given bounce light so far.", n);
		return;
	}
	if (reason != "not an opaque pass shader" && reason != "not ps_3_0")
	{
		LONG n = InterlockedIncrement(&g_giSkipped);
		if (n <= 30)
			LogInfo("SSGI: opaque shader %08X gets no bounce light (%s).", hash, reason.c_str());
	}
}

void Fx_OnCreatePixelShader(IDirect3DPixelShader9* ps, const DWORD* gameCode)
{
	if (g_vol && ps && gameCode)
		vol::Vol_OnCreatePixelShader(ps, gameCode);
}
