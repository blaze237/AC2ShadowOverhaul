// Ambient occlusion and contact shadows: settings, the opaque-pass tracking and the passes. See
// ao.h for the overview and litshader.cpp for the shader rewrite.
//
// The frame (from F8 dumps): ... main opaque pass with RT0 = scene colour (FP16) and RT1 = z/w
// (R32F), toggled off and on between batches; then RT1 is unbound and the sky is drawn, then
// transparents. We bind our indirect-light target as RT2 and our sun-light target as RT3 whenever
// the game binds a z/w target as RT1, and run our passes at the first thing after the pass.
//
// Camera: this frame's, from the shared scene tracker (core/scene.h, maths in core/camera.h), taken
// once AO's own pass has started: the raster camera, with TAA's jitter folded in, because GTAO and
// the contact shadows turn pixels of the (jittered) z/w target into view-space positions and back.
//
// TAA: while it averages frames (core::Scene_TaaAssist), the GTAO slice / step and contact shadow
// step noise shifts every frame (core::Scene_NoiseOffsets) and the blur is narrower (sigma 1 instead
// of 2: PS_BlurNarrow in ao.hlsl says why); otherwise the noise is the same every frame and the
// passes are exactly those without TAA.

#include "ao.h"
#include "common.h"
#include "shaders_gen.h"
#include "core/scene.h"
#include "core/fxpass.h"
#include "core/geoprep.h"
#include <math.h>
#include <stdio.h>
#include <unordered_map>

// ---- Settings ----------------------------------------------------------------------------------

static bool g_enabled = false;     // the shared machinery (shader outputs, passes) is set up
static bool g_aoConfigured = false; // [AmbientOcclusion] Enabled
static bool g_csConfigured = false; // [ContactShadows] Enabled
static volatile bool g_on = true, g_csOn = true;
static volatile float g_radius = 1.5f, g_strength = 0.5f, g_csLength = 0.5f;
static volatile int g_view = 0;

bool AO_Enabled() { return g_enabled; }
bool AO_Active() { return g_aoConfigured && g_on; }
bool CS_Active() { return g_csConfigured && g_csOn; }
void CS_SetOn(bool on) { g_csOn = on; }
float CS_Length() { return g_csLength; }
void CS_SetLength(float m) { g_csLength = m < 0.05f ? 0.05f : m > 4.0f ? 4.0f : m; }
void AO_SetOn(bool on) { g_on = on; }
float AO_Radius() { return g_radius; }
void AO_SetRadius(float m) { g_radius = m < 0.25f ? 0.25f : m > 8.0f ? 8.0f : m; }
float AO_Strength() { return g_strength; }
void AO_SetStrength(float s) { g_strength = s < 0.0f ? 0.0f : s > 4.0f ? 4.0f : s; }
int AO_View() { return g_view; }
void AO_SetView(int v) { g_view = v < 0 ? 0 : v > 4 ? 4 : v; }

bool AO_Init()
{
	const char* S = "AmbientOcclusion";
	g_aoConfigured = IniInt(S, "Enabled", 1) != 0;
	g_enabled = g_aoConfigured || g_csConfigured;
	if (!g_aoConfigured)
	{
		LogInfo("Ambient occlusion: off ([AmbientOcclusion] Enabled=0).");
		return g_enabled;
	}
	AO_SetRadius(IniFloat(S, "Radius", 1.5f));
	AO_SetStrength(IniFloat(S, "Strength", 0.5f));
	AO_SetView(IniInt(S, "DebugView", 0));
	g_on = IniInt(S, "StartEnabled", 1) != 0;
	LogInfo("Ambient occlusion: radius %.2f m, strength %.2f%s.", (double)g_radius, (double)g_strength, g_on ? "" : " (starting off)");
	return true;
}

bool CS_Init()
{
	const char* S = "ContactShadows";
	g_csConfigured = IniInt(S, "Enabled", 1) != 0;
	g_enabled = g_aoConfigured || g_csConfigured;
	if (!g_csConfigured)
	{
		LogInfo("Contact shadows: off ([ContactShadows] Enabled=0).");
		return g_enabled;
	}
	CS_SetLength(IniFloat(S, "Length", 0.5f));
	g_csOn = IniInt(S, "StartEnabled", 1) != 0;
	LogInfo("Contact shadows: length %.2f m%s.", (double)g_csLength, g_csOn ? "" : " (starting off)");
	return true;
}

// ---- Camera ------------------------------------------------------------------------------------

static bool g_camOk = false; // this frame's camera (core::Scene().rasterCamera) was found during our pass

// ---- Device resources --------------------------------------------------------------------------

static const core::DeviceFns& D = core::Orig();

static core::RenderTarget g_indirect, g_sun; // full size: the opaque pass's indirect and sun light
static core::RenderTarget g_ao[2];           // half size: r = AO, g = view z, b = contact shadow
static UINT g_w, g_h, g_hw, g_hh;
static core::PassState g_state;
static IDirect3DVertexShader9* g_vsFull;
static IDirect3DPixelShader9 *g_psGtao, *g_psBlur, *g_psBlurNarrow, *g_psComposite;
static IDirect3DVertexDeclaration9* g_decl;
static bool g_failed = false;     // AO can't run on this device (logged once)
static bool g_capsChecked = false;

static void ReleaseTargets()
{
	g_indirect.Release();
	g_sun.Release();
	for (int i = 0; i < 2; i++)
		g_ao[i].Release();
	g_state.Release();
	g_w = g_h = 0;
}

static void Fail(const char* what, HRESULT hr)
{
	LogWarn("Ambient occlusion disabled: %s (0x%08lX).", what, hr);
	g_failed = true;
	ReleaseTargets();
}

static bool EnsureResources(IDirect3DDevice9* dev, UINT w, UINT h)
{
	if (g_failed)
		return false;
	if (!g_capsChecked)
	{
		g_capsChecked = true;
		D3DCAPS9 caps = {};
		dev->GetDeviceCaps(&caps);
		if (caps.NumSimultaneousRTs < 4 || !(caps.PrimitiveMiscCaps & D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS) ||
			caps.PixelShaderVersion < D3DPS_VERSION(3, 0))
		{
			Fail("the GPU lacks 4 render targets with independent formats or pixel shader 3.0", 0);
			return false;
		}
	}
	if (!g_vsFull)
	{
		HRESULT hr;
		if (FAILED(hr = D.CreateVertexShader(dev, kAoFullscreenVs, &g_vsFull)) || FAILED(hr = D.CreatePixelShader(dev, kAoGtaoPs, &g_psGtao)) ||
			FAILED(hr = D.CreatePixelShader(dev, kAoBlurPs, &g_psBlur)) || FAILED(hr = D.CreatePixelShader(dev, kAoBlurNarrowPs, &g_psBlurNarrow)) ||
			FAILED(hr = D.CreatePixelShader(dev, kAoCompositePs, &g_psComposite)) || !(g_decl = core::FullscreenDecl(dev)))
		{
			Fail("could not create its shaders", hr);
			return false;
		}
	}
	if (g_indirect.tex && g_w == w && g_h == h)
		return true;
	ReleaseTargets();
	HRESULT hr;
	UINT hw = (w + 1) / 2, hh = (h + 1) / 2;
	if (FAILED(hr = g_indirect.Create(dev, w, h, D3DFMT_A16B16G16R16F)))
	{
		Fail("could not create its indirect light target", hr);
		return false;
	}
	if (FAILED(hr = g_sun.Create(dev, w, h, D3DFMT_A16B16G16R16F)))
	{
		Fail("could not create its sun light target", hr);
		return false;
	}
	for (int i = 0; i < 2; i++)
		if (FAILED(hr = g_ao[i].Create(dev, hw, hh, D3DFMT_A16B16G16R16F)))
		{
			Fail("could not create its AO targets", hr);
			return false;
		}
	if (FAILED(hr = g_state.Create(dev)))
	{
		Fail("could not create a state block", hr);
		return false;
	}
	g_w = w;
	g_h = h;
	g_hw = hw;
	g_hh = hh;
	LogInfo("Ambient occlusion: render targets created for %ux%u (AO at %ux%u).", w, h, hw, hh);
	return true;
}

void AO_OnLostDevice() { ReleaseTargets(); }

// ---- Opaque pass tracking ----------------------------------------------------------------------

static bool g_inPass = false;   // our target is bound as RT2
static bool g_pending = false;  // the pass ended; AO is due before the next thing that draws
static bool g_done = false;     // AO applied this frame (or skipped for good this frame)
static bool g_cleared = false;  // indirect target cleared this frame
static int g_passDraws = 0;
static IDirect3DSurface9* g_depthSurf;  // the game's z/w target during the pass
static IDirect3DSurface9* g_colourSurf; // the game's scene colour target during the pass

// The game's z/w target being bound as RT1 next to a scene colour target of the same size.
static bool IsOpaquePass(IDirect3DSurface9* s, IDirect3DSurface9* rt0, UINT& w, UINT& h)
{
	const core::ZwBinding b = core::InspectRt1(s, rt0);
	if (!b.zw)
		return false;
	if (b.colourMsaa)
	{
		static bool warned = false;
		if (!warned)
		{
			warned = true;
			LogWarn("Ambient occlusion: the scene is multisampled (in-game anti-aliasing); AO is not supported with it yet.");
		}
		return false;
	}
	if (!b.zwTexture)
		return false;
	w = b.width;
	h = b.height;
	return true;
}

static void Composite(IDirect3DDevice9* dev);

// Between its opaque batches the game unbinds render target 1 and also resets targets 2 and 3, so
// only a change of render target 0 (or a draw, clear, ...) ends the pass.
bool AO_BeforeSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s)
{
	if (!g_pending || index != 0 || s == g_colourSurf)
		return false;
	Composite(dev);
	return true;
}

void AO_AfterSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s, IDirect3DSurface9* rt0)
{
	if (!g_enabled || g_failed || index == 0)
		return;
	if (index >= 2)
	{
		if (!g_inPass || index > 3)
			return;
		if (!s)
			D.SetRenderTarget(dev, index, index == 2 ? g_indirect.surf : g_sun.surf); // the game reset the slot: keep ours bound
		else
		{
			static bool warned = false;
			if (!warned)
			{
				warned = true;
				LogWarn("Ambient occlusion: the game bound its own render target %lu during the opaque pass; AO skipped there.", index);
			}
			g_inPass = false;
			g_done = true;
		}
		return;
	}
	if (s)
	{
		UINT w, h;
		if (g_done || !(AO_Active() || CS_Active() || g_view) || !IsOpaquePass(s, rt0, w, h) || !EnsureResources(dev, w, h))
			return;
		if (!g_cleared)
		{
			D.ColorFill(dev, g_indirect.surf, NULL, 0);
			D.ColorFill(dev, g_sun.surf, NULL, 0);
			g_cleared = true;
			g_passDraws = 0;
		}
		D.SetRenderTarget(dev, 2, g_indirect.surf);
		D.SetRenderTarget(dev, 3, g_sun.surf);
		g_inPass = true;
		g_pending = false;
		g_depthSurf = s;
		g_colourSurf = rt0;
	}
	else if (g_inPass)
	{
		D.SetRenderTarget(dev, 2, nullptr);
		D.SetRenderTarget(dev, 3, nullptr);
		g_inPass = false;
		if (g_passDraws > 0)
			g_pending = true;
	}
}

bool AO_BeforeDraw(IDirect3DDevice9* dev)
{
	if (g_pending)
	{
		Composite(dev);
		return true;
	}
	if (g_inPass)
	{
		g_passDraws++;
		if (!g_camOk)
			g_camOk = core::Scene().rasterCamera.ok;
	}
	return false;
}

bool AO_InPass() { return g_inPass; }

bool AO_BeforeOther(IDirect3DDevice9* dev)
{
	if (!g_pending)
		return false;
	Composite(dev);
	return true;
}

bool AO_BeforePresent(IDirect3DDevice9* dev)
{
	bool applied = AO_BeforeOther(dev);
	if (g_inPass)
	{
		// The pass never unbound its z/w target: leave the frame alone.
		D.SetRenderTarget(dev, 2, nullptr);
		D.SetRenderTarget(dev, 3, nullptr);
		g_inPass = false;
	}
	g_done = g_cleared = g_pending = false;
	g_camOk = false;
	g_passDraws = 0;
	return applied;
}

// ---- The AO passes -----------------------------------------------------------------------------

static float g_lastConsts[8][4];
static bool g_haveLastConsts = false;

std::string AO_FrameInfo()
{
	if (!g_haveLastConsts)
		return "(no AO pass has run)";
	std::string s;
	char buf[128];
	for (int i = 0; i < 8; i++)
	{
		snprintf(buf, sizeof(buf), "%sc%d = (%.9g, %.9g, %.9g, %.9g)", i ? " | " : "", i, g_lastConsts[i][0], g_lastConsts[i][1],
			g_lastConsts[i][2], g_lastConsts[i][3]);
		s += buf;
	}
	return s;
}

static void Composite(IDirect3DDevice9* dev)
{
	g_pending = false;
	g_done = true;
	if (!g_camOk)
	{
		static bool warned = false;
		if (!warned)
		{
			warned = true;
			LogWarn("Ambient occlusion: skipped a frame without camera matrices (logged once).");
		}
		return;
	}
	if (!g_state.Ready() || !g_depthSurf || !g_colourSurf)
		return;
	IDirect3DTexture9* depthTex = nullptr;
	if (FAILED(g_depthSurf->GetContainer(core::kIID_Texture9, (void**)&depthTex)) || !depthTex)
		return;

	g_state.Save(dev);
	g_state.BeginPasses(dev, g_decl, g_vsFull);
	const core::CameraFloats& cam = core::Scene().rasterCamera.f; // jittered like the z/w target
	float noise[2];
	core::Scene_NoiseOffsets(noise); // (0, 0) unless TAA averages frames
	// Normals at half res, shared with SSGI (which runs next and reuses them).
	IDirect3DTexture9* geo = core::Geo_Ensure(dev, depthTex, g_w, g_h, cam);
	if (!geo)
	{
		g_state.Restore(dev);
		depthTex->Release();
		return;
	}
	for (DWORD s = 0; s < 4; s++)
		core::SetSamplerClamp(dev, s, D3DTEXF_POINT);

	const float view = (float)g_view;
	// Sun direction in view space (the scene tracker's: this frame's, else the last seen);
	// contact shadows off = length 0. AO off = strength 0 (AO^0 = 1).
	const core::SceneState& sc = core::Scene();
	float sunView[3] = { 0, 0, 0 };
	const float* axes[3] = { cam.right, cam.up, cam.forward };
	for (int k = 0; k < 3; k++)
		sunView[k] = axes[k][0] * sc.sunDir[0] + axes[k][1] * sc.sunDir[1] + axes[k][2] * sc.sunDir[2];
	const bool cs = CS_Active() && sc.sunKnown;
	const float aoStrength = AO_Active() ? g_strength : 0.0f;
	const float c[8][4] = {
		{ (float)g_w, (float)g_h, 1.0f / g_w, 1.0f / g_h },
		{ (float)g_hw, (float)g_hh, 1.0f / g_hw, 1.0f / g_hh },
		{ cam.A, cam.B, cam.P00, cam.P11 },
		{ cam.P02, cam.P12, noise[0], noise[1] }, // zw: added to the slice / step noise
		{ g_radius, 0.5f * cam.P00 * g_w, 0.1f * g_h, 0.6f },
		{ aoStrength, view, 1, 0 },
		{ sunView[0], sunView[1], sunView[2], cs ? g_csLength : 0.0f },
		{ 0.05f, 0.01f, 1.0f, 0.002f }, // contact shadows: thickness base (m), thickness per metre, strength, depth bias per metre
	};
	dev->SetPixelShaderConstantF(0, &c[0][0], 8);
	memcpy(g_lastConsts, c, sizeof(c));
	g_haveLastConsts = true;

	// GTAO -> ao[0]; blur across -> ao[1]; blur down -> ao[0]
	D.SetTexture(dev, 0, depthTex);
	D.SetTexture(dev, 1, geo);
	core::SetTarget(dev, g_ao[0].surf, g_hw, g_hh);
	D.SetPixelShader(dev, g_psGtao);
	core::DrawFullscreen(dev);
	D.SetPixelShader(dev, core::Scene_TaaAssist() ? g_psBlurNarrow : g_psBlur);
	core::SetTarget(dev, g_ao[1].surf, g_hw, g_hh);
	D.SetTexture(dev, 1, g_ao[0].tex);
	core::DrawFullscreen(dev);
	const float down[4] = { aoStrength, view, 0, 1 };
	dev->SetPixelShaderConstantF(5, down, 1);
	core::SetTarget(dev, g_ao[0].surf, g_hw, g_hh);
	D.SetTexture(dev, 1, g_ao[1].tex);
	core::DrawFullscreen(dev);

	// Composite: scene colour -= (1 - AO) x indirect + (1 - contact) x sun (colour channels only).
	core::SetTarget(dev, g_colourSurf, g_w, g_h);
	D.SetTexture(dev, 1, g_ao[0].tex);
	D.SetTexture(dev, 2, g_indirect.tex);
	D.SetTexture(dev, 3, g_sun.tex);
	if (g_view == 0)
	{
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
		dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
		dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_REVSUBTRACT);
		dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
	}
	D.SetPixelShader(dev, g_psComposite);
	core::DrawFullscreen(dev);

	g_state.Restore(dev);
	depthTex->Release();

	static bool first = true;
	if (first)
	{
		first = false;
		LogInfo("Ambient occlusion: first frame applied.");
	}
}
