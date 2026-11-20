// Ambient occlusion and contact shadows: settings, the opaque-pass tracking and the passes. See
// ao.h for the overview and aoshader.cpp for the shader rewrite.
//
// The frame (from F8 dumps): ... main opaque pass with RT0 = scene colour (FP16) and RT1 = z/w
// (R32F), toggled off and on between batches; then RT1 is unbound and the sky is drawn, then
// transparents. We bind our indirect-light target as RT2 and our sun-light target as RT3 whenever
// the game binds a z/w target as RT1, and run our passes at the first thing after the pass.
//
// Camera: this frame's, from the shared scene tracker (core/scene.h, maths in core/camera.h), taken
// once AO's own pass has started.

#include "ao.h"
#include "common.h"
#include "shaders_gen.h"
#include "core/scene.h"
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

// ---- Pixel shaders: where the sun direction is -------------------------------------------------

static SRWLOCK g_psLock = SRWLOCK_INIT;
static std::unordered_map<void*, int> g_psSun; // game pixel shader -> register of g_ShadowedDirect

void AO_OnCreatePixelShader(IDirect3DPixelShader9* ps, const DWORD* code)
{
	if (!g_enabled || !ps)
		return;
	int reg = AO_SunRegister(code);
	AcquireSRWLockExclusive(&g_psLock);
	if (reg >= 0)
		g_psSun[ps] = reg;
	else
		g_psSun.erase(ps);
	ReleaseSRWLockExclusive(&g_psLock);
}

static float g_sunWorld[3];   // direction towards the sun, world space (last seen)
static bool g_sunKnown = false, g_sunThisFrame = false;

static void CaptureSun(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps)
{
	if (!ps)
		return;
	int reg = -1;
	AcquireSRWLockShared(&g_psLock);
	auto it = g_psSun.find(ps);
	if (it != g_psSun.end())
		reg = it->second;
	ReleaseSRWLockShared(&g_psLock);
	float v[4];
	if (reg < 0 || FAILED(dev->GetPixelShaderConstantF(reg, v, 1)))
		return;
	float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (!(len > 0.5f && len < 2.0f))
		return;
	for (int k = 0; k < 3; k++)
		g_sunWorld[k] = v[k] / len;
	if (!g_sunKnown)
		LogInfo("Contact shadows: sun direction found (%.3f, %.3f, %.3f).", g_sunWorld[0], g_sunWorld[1], g_sunWorld[2]);
	g_sunKnown = g_sunThisFrame = true;
}

// ---- Camera ------------------------------------------------------------------------------------

static bool g_camOk = false; // this frame's camera (core::Scene().camera) was found during our pass

// ---- Device resources --------------------------------------------------------------------------

static AoDeviceFns D;
static const GUID kIID_Texture9 = { 0x85c31227, 0x3de5, 0x4f00, { 0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5 } };
void AO_SetDeviceFns(const AoDeviceFns& fns) { D = fns; }

static IDirect3DTexture9* g_indirectTex;
static IDirect3DSurface9* g_indirectSurf;
static IDirect3DTexture9* g_sunTex;
static IDirect3DSurface9* g_sunSurf;
static IDirect3DTexture9* g_aoTex[2];
static IDirect3DSurface9* g_aoSurf[2];
static UINT g_w, g_h, g_hw, g_hh;
static IDirect3DStateBlock9* g_state;
static IDirect3DVertexShader9* g_vsFull;
static IDirect3DPixelShader9 *g_psGtao, *g_psBlur, *g_psComposite;
static IDirect3DVertexDeclaration9* g_decl;
static bool g_failed = false;     // AO can't run on this device (logged once)
static bool g_capsChecked = false;

static void ReleaseTargets()
{
	auto rel = [](auto*& p) {
		if (p)
			p->Release();
		p = nullptr;
	};
	rel(g_indirectSurf);
	rel(g_indirectTex);
	rel(g_sunSurf);
	rel(g_sunTex);
	for (int i = 0; i < 2; i++)
	{
		rel(g_aoSurf[i]);
		rel(g_aoTex[i]);
	}
	rel(g_state);
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
		const D3DVERTEXELEMENT9 decl[] = { { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 }, D3DDECL_END() };
		if (FAILED(hr = D.CreateVertexShader(dev, kAoFullscreenVs, &g_vsFull)) || FAILED(hr = D.CreatePixelShader(dev, kAoGtaoPs, &g_psGtao)) ||
			FAILED(hr = D.CreatePixelShader(dev, kAoBlurPs, &g_psBlur)) ||
			FAILED(hr = D.CreatePixelShader(dev, kAoCompositePs, &g_psComposite)) || FAILED(hr = dev->CreateVertexDeclaration(decl, &g_decl)))
		{
			Fail("could not create its shaders", hr);
			return false;
		}
	}
	if (g_indirectTex && g_w == w && g_h == h)
		return true;
	ReleaseTargets();
	HRESULT hr;
	UINT hw = (w + 1) / 2, hh = (h + 1) / 2;
	if (FAILED(hr = D.CreateTexture(dev, w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &g_indirectTex, NULL)) ||
		FAILED(hr = g_indirectTex->GetSurfaceLevel(0, &g_indirectSurf)))
	{
		Fail("could not create its indirect light target", hr);
		return false;
	}
	if (FAILED(hr = D.CreateTexture(dev, w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &g_sunTex, NULL)) ||
		FAILED(hr = g_sunTex->GetSurfaceLevel(0, &g_sunSurf)))
	{
		Fail("could not create its sun light target", hr);
		return false;
	}
	for (int i = 0; i < 2; i++)
	{
		// r = AO, g = view z, b = contact shadow
		if (FAILED(hr = D.CreateTexture(dev, hw, hh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &g_aoTex[i], NULL)))
		{
			Fail("could not create its AO targets", hr);
			return false;
		}
		if (FAILED(hr = g_aoTex[i]->GetSurfaceLevel(0, &g_aoSurf[i])))
		{
			Fail("could not get its AO targets", hr);
			return false;
		}
	}
	if (FAILED(hr = dev->CreateStateBlock(D3DSBT_ALL, &g_state)))
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
			D.SetRenderTarget(dev, index, index == 2 ? g_indirectSurf : g_sunSurf); // the game reset the slot: keep ours bound
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
			D.ColorFill(dev, g_indirectSurf, NULL, 0);
			D.ColorFill(dev, g_sunSurf, NULL, 0);
			g_cleared = true;
			g_passDraws = 0;
		}
		D.SetRenderTarget(dev, 2, g_indirectSurf);
		D.SetRenderTarget(dev, 3, g_sunSurf);
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

bool AO_BeforeDraw(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps)
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
			g_camOk = core::Scene().camera.ok;
		if (!g_sunThisFrame)
			CaptureSun(dev, ps);
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
	g_sunThisFrame = false;
	g_passDraws = 0;
	return applied;
}

// ---- The AO passes -----------------------------------------------------------------------------

static void Fullscreen(IDirect3DDevice9* dev)
{
	static const float tri[3][4] = { { -1, -1, 0, 1 }, { -1, 3, 0, 1 }, { 3, -1, 0, 1 } };
	D.DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0]));
}

static void SetTarget(IDirect3DDevice9* dev, IDirect3DSurface9* s, UINT w, UINT h)
{
	D.SetRenderTarget(dev, 0, s);
	D3DVIEWPORT9 vp = { 0, 0, w, h, 0.0f, 1.0f };
	D.SetViewport(dev, &vp);
}

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
	if (!g_state || !g_depthSurf || !g_colourSurf)
		return;
	IDirect3DTexture9* depthTex = nullptr;
	if (FAILED(g_depthSurf->GetContainer(kIID_Texture9, (void**)&depthTex)) || !depthTex)
		return;

	// Save everything we touch: state block for states/shaders/constants/textures/viewport, and the
	// render targets and depth buffer by hand (state blocks don't hold them).
	g_state->Capture();
	IDirect3DSurface9* oldRt[4] = {};
	IDirect3DSurface9* oldDs = nullptr;
	for (DWORD i = 0; i < 4; i++)
		if (FAILED(dev->GetRenderTarget(i, &oldRt[i])))
			oldRt[i] = nullptr;
	if (FAILED(dev->GetDepthStencilSurface(&oldDs)))
		oldDs = nullptr;
	// Vertex input, by hand as well: the game draws instanced geometry (foliage), and our
	// DrawPrimitiveUP calls reset stream 0 and must run with instancing off. A stale stream or
	// frequency on the game's next instanced draw shows up as long stretched triangles.
	const UINT kStreams = 8;
	IDirect3DVertexBuffer9* oldVb[kStreams] = {};
	UINT oldOffset[kStreams] = {}, oldStride[kStreams] = {}, oldFreq[kStreams];
	for (UINT i = 0; i < kStreams; i++)
	{
		oldFreq[i] = 1;
		if (FAILED(dev->GetStreamSource(i, &oldVb[i], &oldOffset[i], &oldStride[i])))
			oldVb[i] = nullptr;
		dev->GetStreamSourceFreq(i, &oldFreq[i]);
	}
	IDirect3DIndexBuffer9* oldIb = nullptr;
	if (FAILED(dev->GetIndices(&oldIb)))
		oldIb = nullptr;
	IDirect3DVertexDeclaration9* oldDecl = nullptr;
	if (FAILED(dev->GetVertexDeclaration(&oldDecl)))
		oldDecl = nullptr;
	DWORD oldFvf = 0;
	dev->GetFVF(&oldFvf);
	for (UINT i = 0; i < kStreams; i++)
		if (oldFreq[i] != 1)
			dev->SetStreamSourceFreq(i, 1);

	const DWORD rs[][2] = {
		{ D3DRS_ZENABLE, FALSE }, { D3DRS_ZWRITEENABLE, FALSE }, { D3DRS_STENCILENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE },
		{ D3DRS_ALPHABLENDENABLE, FALSE }, { D3DRS_SEPARATEALPHABLENDENABLE, FALSE }, { D3DRS_CULLMODE, D3DCULL_NONE },
		{ D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_COLORWRITEENABLE, 0xF }, { D3DRS_SRGBWRITEENABLE, FALSE },
		{ D3DRS_FOGENABLE, FALSE }, { D3DRS_CLIPPLANEENABLE, 0 }, { D3DRS_FILLMODE, D3DFILL_SOLID },
		{ D3DRS_TWOSIDEDSTENCILMODE, FALSE }, { D3DRS_POINTSPRITEENABLE, FALSE },
	};
	for (const auto& r : rs)
		dev->SetRenderState((D3DRENDERSTATETYPE)r[0], r[1]);
	for (DWORD s = 0; s < 4; s++)
	{
		dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
		dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
		dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
		dev->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, 0);
	}
	D.SetDepthStencilSurface(dev, nullptr);
	for (DWORD i = 1; i < 4; i++)
		if (oldRt[i])
			D.SetRenderTarget(dev, i, nullptr);
	dev->SetVertexDeclaration(g_decl);
	D.SetVertexShader(dev, g_vsFull);

	const core::CameraFloats& cam = core::Scene().camera.f;
	const float view = (float)g_view;
	// Sun direction in view space; contact shadows off = length 0. AO off = strength 0 (AO^0 = 1).
	float sunView[3] = { 0, 0, 0 };
	const float* axes[3] = { cam.right, cam.up, cam.forward };
	for (int k = 0; k < 3; k++)
		sunView[k] = axes[k][0] * g_sunWorld[0] + axes[k][1] * g_sunWorld[1] + axes[k][2] * g_sunWorld[2];
	const bool cs = CS_Active() && g_sunKnown;
	const float aoStrength = AO_Active() ? g_strength : 0.0f;
	const float c[8][4] = {
		{ (float)g_w, (float)g_h, 1.0f / g_w, 1.0f / g_h },
		{ (float)g_hw, (float)g_hh, 1.0f / g_hw, 1.0f / g_hh },
		{ cam.A, cam.B, cam.P00, cam.P11 },
		{ cam.P02, cam.P12, 0, 0 },
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
	SetTarget(dev, g_aoSurf[0], g_hw, g_hh);
	D.SetPixelShader(dev, g_psGtao);
	Fullscreen(dev);
	D.SetPixelShader(dev, g_psBlur);
	SetTarget(dev, g_aoSurf[1], g_hw, g_hh);
	D.SetTexture(dev, 1, g_aoTex[0]);
	Fullscreen(dev);
	const float down[4] = { aoStrength, view, 0, 1 };
	dev->SetPixelShaderConstantF(5, down, 1);
	SetTarget(dev, g_aoSurf[0], g_hw, g_hh);
	D.SetTexture(dev, 1, g_aoTex[1]);
	Fullscreen(dev);

	// Composite: scene colour -= (1 - AO) x indirect + (1 - contact) x sun (colour channels only).
	SetTarget(dev, g_colourSurf, g_w, g_h);
	D.SetTexture(dev, 1, g_aoTex[0]);
	D.SetTexture(dev, 2, g_indirectTex);
	D.SetTexture(dev, 3, g_sunTex);
	if (g_view == 0)
	{
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
		dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
		dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_REVSUBTRACT);
		dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
	}
	D.SetPixelShader(dev, g_psComposite);
	Fullscreen(dev);

	// Restore.
	for (DWORD i = 0; i < 4; i++)
		if (i == 0 || oldRt[i])
			D.SetRenderTarget(dev, i, oldRt[i]);
	D.SetDepthStencilSurface(dev, oldDs);
	g_state->Apply();
	if (oldDecl)
		dev->SetVertexDeclaration(oldDecl);
	else
		dev->SetFVF(oldFvf);
	for (UINT i = 0; i < kStreams; i++)
	{
		dev->SetStreamSource(i, oldVb[i], oldOffset[i], oldStride[i]);
		dev->SetStreamSourceFreq(i, oldFreq[i]);
		if (oldVb[i])
			oldVb[i]->Release();
	}
	dev->SetIndices(oldIb);
	if (oldIb)
		oldIb->Release();
	if (oldDecl)
		oldDecl->Release();
	for (DWORD i = 0; i < 4; i++)
		if (oldRt[i])
			oldRt[i]->Release();
	if (oldDs)
		oldDs->Release();
	depthTex->Release();

	static bool first = true;
	if (first)
	{
		first = false;
		LogInfo("Ambient occlusion: first frame applied.");
	}
}
