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
#include "core/scene.h"
#include "core/menu.h"
#include <stdio.h>
#include <string.h>
#include <string>

volatile bool g_fxPassthrough = false;

static bool g_gi = false, g_vol = false, g_taa = false; // configured on (hooks and shader rewrites set up)
static bool g_volMenu = false;

bool Fx_Init()
{
	g_gi = ssgi::GI_Init();
	ssgi::Menu_Init();
	g_vol = vol::Vol_Init();
	g_volMenu = vol::Menu_Init();
	g_taa = taa::Taa_Init();
	taa::Menu_Init();
	return g_gi || g_vol || g_taa;
}

void Fx_Flush()
{
	ssgi::Menu_Flush();
	vol::Menu_Flush();
	taa::Menu_Flush();
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
				"volumetrics and TAA need it off.");
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
	return list;
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
				"volumetrics and TAA can't find the camera.");
	if (g_vol)
		vol::Vol_OnDeviceReady(dev);
	if (g_taa)
		taa::Taa_OnDeviceCreated(d3d, adapter, dev);
}

void Fx_OnLostDevice()
{
	core::Menu_OnLostDevice();
	g_warnFont.OnLostDevice();
	if (g_gi)
		ssgi::GI_OnLostDevice();
	if (g_vol)
		vol::Vol_OnLostDevice();
	if (g_taa)
		taa::Taa_OnLostDevice();
}

void Fx_OnResetDevice(IDirect3DDevice9* dev, const D3DPRESENT_PARAMETERS* pp)
{
	NotePresentParams(pp);
	if (g_vol)
		vol::Vol_OnDeviceReady(dev);
	core::Menu_OnResetDevice();
	g_warnFont.OnResetDevice();
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
}

void Fx_DrawOverlays(IDirect3DDevice9* dev)
{
	g_fxPassthrough = true;
	ssgi::Menu_OnPresent(dev);
	if (g_volMenu)
		vol::Menu_OnPresent(dev);
	taa::Menu_OnPresent(dev);
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

bool Fx_RewritePSForGI(const DWORD* code, uint32_t hash, std::vector<DWORD>& out)
{
	if (!g_gi || !code)
		return false;
	std::string reason;
	if (ssgi::GI_RewritePS(code, out, reason))
	{
		LONG n = InterlockedIncrement(&g_giRewritten);
		if (n == 1 || n % 50 == 0)
			LogInfo("SSGI: %ld opaque shaders given bounce light so far.", n);
		return true;
	}
	if (reason != "not an opaque pass shader" && reason != "not ps_3_0")
	{
		LONG n = InterlockedIncrement(&g_giSkipped);
		if (n <= 30)
			LogInfo("SSGI: opaque shader %08X gets no bounce light (%s).", hash, reason.c_str());
	}
	return false;
}

void Fx_OnCreatePixelShader(IDirect3DPixelShader9* ps, const DWORD* gameCode)
{
	if (g_vol && ps && gameCode)
		vol::Vol_OnCreatePixelShader(ps, gameCode);
}
