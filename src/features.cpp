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
#include <stdio.h>
#include <string.h>
#include <string>

volatile bool g_fxPassthrough = false;

static bool g_gi = false, g_vol = false, g_taa = false; // configured on (hooks and shader rewrites set up)
static bool g_volMenu = false;
static FxDeviceFns g_fns;

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

void Fx_SetDeviceFns(const FxDeviceFns& f)
{
	g_fns = f;

	ssgi::GiDeviceFns gi;
	gi.SetRenderTarget = f.SetRenderTarget;
	gi.SetDepthStencilSurface = f.SetDepthStencilSurface;
	gi.SetTexture = f.SetTexture;
	gi.DrawPrimitiveUP = f.DrawPrimitiveUP;
	gi.SetVertexShader = f.SetVertexShader;
	gi.SetPixelShader = f.SetPixelShader;
	gi.CreatePixelShader = f.CreatePixelShader;
	gi.CreateVertexShader = f.CreateVertexShader;
	gi.StretchRect = f.StretchRect;
	ssgi::GI_SetDeviceFns(gi);

	vol::DeviceFns v;
	v.SetRenderTarget = f.SetRenderTarget;
	v.SetViewport = f.SetViewport;
	v.DrawPrimitiveUP = f.DrawPrimitiveUP;
	v.SetVertexShader = f.SetVertexShader;
	v.SetPixelShader = f.SetPixelShader;
	v.CreateVertexShader = f.CreateVertexShader;
	v.CreatePixelShader = f.CreatePixelShader;
	vol::Vol_SetDeviceFns(v);

	taa::TaaDeviceFns t;
	t.SetRenderTarget = f.SetRenderTarget;
	t.SetDepthStencilSurface = f.SetDepthStencilSurface;
	t.SetTexture = f.SetTexture;
	t.SetSamplerState = f.SetSamplerState;
	t.DrawPrimitiveUP = f.DrawPrimitiveUP;
	t.SetVertexShader = f.SetVertexShader;
	t.SetVertexShaderConstantF = f.SetVertexShaderConstantF;
	t.CreateVertexShader = f.CreateVertexShader;
	t.CreateTexture = f.CreateTexture;
	taa::Taa_SetDeviceFns(t);
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

// Minimal ID3DXFont (from the d3dx9_42.dll the game already loads), as in the menus.
struct WarnFont;
struct WarnFontVtbl
{
	void* QueryInterface;
	void* AddRef;
	ULONG(__stdcall* Release)(WarnFont*);
	void* pad[11];
	INT(__stdcall* DrawTextA)(WarnFont*, void* sprite, LPCSTR text, INT count, RECT* rect, DWORD format, D3DCOLOR color);
	void* DrawTextW;
	HRESULT(__stdcall* OnLostDevice)(WarnFont*);
	HRESULT(__stdcall* OnResetDevice)(WarnFont*);
};
struct WarnFont
{
	WarnFontVtbl* vt;
};
typedef HRESULT(WINAPI* PFN_D3DXCreateFontA)(IDirect3DDevice9*, INT, UINT, UINT, UINT, BOOL, DWORD, DWORD, DWORD, DWORD, LPCSTR, WarnFont**);
static WarnFont* g_warnFont;
static UINT g_warnFontHeight;
static bool g_warnFontFailed;

static bool EnsureWarnFont(IDirect3DDevice9* dev, UINT bbHeight)
{
	UINT height = bbHeight / 40;
	if (height < 15)
		height = 15;
	if (g_warnFont && height == g_warnFontHeight)
		return true;
	if (g_warnFont)
		g_warnFont->vt->Release(g_warnFont);
	g_warnFont = nullptr;
	if (g_warnFontFailed)
		return false;
	PFN_D3DXCreateFontA create = nullptr;
	for (const char* d : { "d3dx9_42.dll", "d3dx9_43.dll" })
	{
		HMODULE m = GetModuleHandleA(d);
		if (!m)
			m = LoadLibraryA(d);
		if (m && (create = (PFN_D3DXCreateFontA)GetProcAddress(m, "D3DXCreateFontA")) != nullptr)
			break;
	}
	if (!create || FAILED(create(dev, (INT)height, 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
						   DEFAULT_PITCH | FF_DONTCARE, "Consolas", &g_warnFont)) ||
		!g_warnFont)
	{
		g_warnFont = nullptr;
		g_warnFontFailed = true;
		return false;
	}
	g_warnFontHeight = height;
	return true;
}

static void DrawWarning(IDirect3DDevice9* dev, unsigned bbW, unsigned bbH)
{
	bool msaa = g_ppMsaa || core::Scene_MsaaRecently();
	if (!msaa || !bbW || !bbH)
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
	if (!EnsureWarnFont(dev, bbH))
		return;
	std::string line1 = "AC2 Shadow Overhaul: the game's anti-aliasing (MSAA) is on";
	std::string line2 = "These need it off and are not working: " + effects;
	std::string line3 = "Turn anti-aliasing off in the game's graphics options (or switch these effects off in their menus)";

	IDirect3DSurface9 *bb = nullptr, *oldRt = nullptr;
	D3DVIEWPORT9 oldVp;
	if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
		return;
	dev->GetRenderTarget(0, &oldRt);
	dev->GetViewport(&oldVp);
	dev->SetRenderTarget(0, bb);
	D3DVIEWPORT9 vp = { 0, 0, bbW, bbH, 0.0f, 1.0f };
	dev->SetViewport(&vp);
	if (SUCCEEDED(dev->BeginScene()))
	{
		int h = (int)g_warnFontHeight;
		int y = (int)bbH / 6;
		for (const std::string* s : { &line1, &line2, &line3 })
		{
			int shadow = h / 12 + 1;
			RECT r = { shadow, y + shadow, (LONG)bbW + shadow, y + 2 * h };
			g_warnFont->vt->DrawTextA(g_warnFont, nullptr, s->c_str(), -1, &r, DT_CENTER | DT_TOP | DT_NOCLIP | DT_SINGLELINE,
				D3DCOLOR_ARGB(220, 0, 0, 0));
			r = { 0, y, (LONG)bbW, y + 2 * h };
			g_warnFont->vt->DrawTextA(g_warnFont, nullptr, s->c_str(), -1, &r, DT_CENTER | DT_TOP | DT_NOCLIP | DT_SINGLELINE,
				D3DCOLOR_XRGB(255, 40, 40));
			y += h * 5 / 4;
		}
		dev->EndScene();
	}
	dev->SetRenderTarget(0, oldRt ? oldRt : bb);
	dev->SetViewport(&oldVp);
	if (oldRt)
		oldRt->Release();
	bb->Release();
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
	ssgi::Menu_OnLostDevice();
	if (g_volMenu)
		vol::Menu_OnLostDevice();
	taa::Menu_OnLostDevice();
	if (g_warnFont)
		g_warnFont->vt->OnLostDevice(g_warnFont);
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
	ssgi::Menu_OnResetDevice();
	if (g_volMenu)
		vol::Menu_OnResetDevice();
	taa::Menu_OnResetDevice();
	if (g_warnFont)
		g_warnFont->vt->OnResetDevice(g_warnFont);
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

void Fx_DrawOverlays(IDirect3DDevice9* dev, unsigned bbW, unsigned bbH)
{
	g_fxPassthrough = true;
	ssgi::Menu_OnPresent(dev, [] {
		ssgi::GiDeviceFns d = {};
		d.SetRenderTarget = g_fns.SetRenderTarget;
		d.SetDepthStencilSurface = g_fns.SetDepthStencilSurface;
		d.SetTexture = g_fns.SetTexture;
		d.DrawPrimitiveUP = g_fns.DrawPrimitiveUP;
		d.SetVertexShader = g_fns.SetVertexShader;
		d.SetPixelShader = g_fns.SetPixelShader;
		d.CreatePixelShader = g_fns.CreatePixelShader;
		d.CreateVertexShader = g_fns.CreateVertexShader;
		d.StretchRect = g_fns.StretchRect;
		return d;
	}());
	if (g_volMenu)
		vol::Menu_OnPresent(dev);
	taa::Menu_OnPresent(dev);
	DrawWarning(dev, bbW, bbH);
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

void Fx_BeforeDraw(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs, IDirect3DPixelShader9* ps)
{
	FxScope scope;
	(void)ps;
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
