// SSGI: settings, opaque-pass tracking and the passes. See gi.h for the overview, gishader.cpp for
// the shader rewrite and shaders/ssgi.hlsl for the passes.
//
// The frame (from AC2ShadowOverhaul's F8 dumps): ... the main opaque pass with RT0 = scene colour and
// RT1 = z/w (R32F), split into batches with RT1 unbound and re-bound between them; then RT1 is
// unbound for good and the sky is drawn, then transparents. While RT0 is the scene colour our GI
// texture (last frame's result) is bound to s13 and the reprojection rows to c216-c218. The first
// thing after the pass (a draw, a clear, another RT0, ...) runs our passes, which write the GI for
// the next frame.

#include "gi.h"
#include "common.h"
#include "shaders_gen.h"
#include "core/scene.h"
#include "core/fxpass.h"
#include <math.h>
#include <stdio.h>
#include <unordered_map>

namespace ssgi
{

// ---- Settings ----------------------------------------------------------------------------------

static bool g_enabled = false;
static volatile bool g_on = true;
static volatile float g_intensity = 1.0f;
static volatile int g_view = 0;
static float g_radius = 10.0f, g_maxScreenRadius = 1.0f, g_thickness = 0.5f, g_minBlend = 0.1f, g_radianceClamp = 4.0f, g_depthTolerance = 0.05f;
static int g_slices = 3, g_steps = 8;
static volatile bool g_historyReset = false; // set when switched on/off (no stale light when turned back on)

bool GI_Enabled() { return g_enabled; }
bool GI_Active() { return g_enabled && g_on; }
float GI_Intensity() { return g_intensity; }
int GI_View() { return g_view; }

static float Clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

bool GI_Init()
{
	const char* S = "SSGI";
	g_enabled = IniInt(S, "Enabled", 1) != 0;
	if (!g_enabled)
	{
		LogInfo("SSGI: off ([SSGI] Enabled=0); shaders left unchanged.");
		return false;
	}
	g_on = IniInt(S, "StartEnabled", 1) != 0;
	g_intensity = Clamp(IniFloat(S, "Intensity", 1.0f), 0.0f, 8.0f);
	g_radius = Clamp(IniFloat(S, "Radius", 10.0f), 0.25f, 50.0f);
	g_maxScreenRadius = Clamp(IniFloat(S, "MaxScreenRadius", 1.0f), 0.05f, 4.0f);
	g_thickness = Clamp(IniFloat(S, "Thickness", 0.5f), 0.02f, 10.0f);
	g_slices = (int)Clamp((float)IniInt(S, "Slices", 3), 1, 8);
	g_steps = (int)Clamp((float)IniInt(S, "Steps", 8), 2, 24);
	g_minBlend = Clamp(IniFloat(S, "TemporalBlend", 0.1f), 0.02f, 1.0f);
	g_radianceClamp = Clamp(IniFloat(S, "RadianceClamp", 4.0f), 0.1f, 1000.0f);
	g_view = (int)Clamp((float)IniInt(S, "DebugView", 0), 0, 3);
	LogInfo("SSGI: intensity %.2f, radius %.2f m (at most %.2f x screen height), thickness %.2f m, %d slices x %d steps, temporal blend %.2f, radiance clamp %.1f%s.",
		(double)g_intensity, (double)g_radius, (double)g_maxScreenRadius, (double)g_thickness, g_slices, g_steps, (double)g_minBlend, (double)g_radianceClamp,
		g_on ? "" : " (starting off)");
	return true;
}

// ---- Live settings (debug menu) -----------------------------------------------------------------

float GI_GetSetting(int s)
{
	switch (s)
	{
	case GS_ON: return g_on ? 1.0f : 0.0f;
	case GS_VIEW: return (float)g_view;
	case GS_INTENSITY: return g_intensity;
	case GS_RADIUS: return g_radius;
	case GS_MAX_SCREEN: return g_maxScreenRadius;
	case GS_THICKNESS: return g_thickness;
	case GS_SLICES: return (float)g_slices;
	case GS_STEPS: return (float)g_steps;
	case GS_BLEND: return g_minBlend;
	case GS_CLAMP: return g_radianceClamp;
	}
	return 0;
}

void GI_SetSetting(int s, float v)
{
	switch (s)
	{
	case GS_ON:
		if ((v > 0.5f) != g_on)
		{
			g_on = v > 0.5f;
			g_historyReset = true;
		}
		break;
	case GS_VIEW: g_view = (int)Clamp(v + 0.5f, 0, 3); break;
	case GS_INTENSITY: g_intensity = Clamp(v, 0.0f, 8.0f); break;
	case GS_RADIUS: g_radius = Clamp(v, 0.25f, 50.0f); break;
	case GS_MAX_SCREEN: g_maxScreenRadius = Clamp(v, 0.05f, 4.0f); break;
	case GS_THICKNESS: g_thickness = Clamp(v, 0.02f, 10.0f); break;
	case GS_SLICES: g_slices = (int)Clamp(v + 0.5f, 1, 8); break;
	case GS_STEPS: g_steps = (int)Clamp(v + 0.5f, 2, 24); break;
	case GS_BLEND: g_minBlend = Clamp(v, 0.02f, 1.0f); break;
	case GS_CLAMP: g_radianceClamp = Clamp(v, 0.1f, 1000.0f); break;
	}
}

// ---- Camera ------------------------------------------------------------------------------------

// This frame's camera is core::Scene().camera (the first opaque draw with the camera matrices);
// g_camOk says it was there by the time our pass drew. g_prevCam: the camera of the frame that wrote
// g_out[g_outBound] / the history.
static bool g_camOk = false;
static core::Camera g_prevCam;
static const core::Camera& Cam() { return core::Scene().camera; }

// ---- Device resources --------------------------------------------------------------------------

static const core::DeviceFns& D = core::Orig();

static core::RenderTarget g_prep[2], g_rad, g_raw, g_hist[2], g_tmp, g_out[2], g_black, g_colourCopy;
static D3DFORMAT g_colourCopyFmt = D3DFMT_UNKNOWN;
static UINT g_w, g_h, g_hw, g_hh;
static core::PassState g_state;
static IDirect3DVertexShader9* g_vsFull;
static IDirect3DPixelShader9 *g_psPrep, *g_psTrace, *g_psTemporal, *g_psBlur, *g_psDebug;
static IDirect3DVertexDeclaration9* g_decl;
static bool g_failed = false;
static bool g_capsChecked = false;

// Frame-to-frame state.
static int g_cur = 0;             // prep / history written this frame; 1 - g_cur = last frame's
static int g_outBound = 0;        // GI texture the game's shaders read this frame
static bool g_outValid = false;   // g_out[g_outBound] holds a result
static bool g_histValid = false;  // g_hist[1 - g_cur], g_prep[1 - g_cur] hold last frame's
static bool g_wroteThisFrame = false;
static unsigned g_frame = 0;

// GPU time of our passes (debug menu): timestamp queries, read back a few frames later without
// waiting. -1 = not measured.
struct GpuTiming
{
	IDirect3DQuery9 *disjoint = nullptr, *freq = nullptr, *begin = nullptr, *end = nullptr;
	bool issued = false;
	void Release()
	{
		for (IDirect3DQuery9** q : { &disjoint, &freq, &begin, &end })
			if (*q)
			{
				(*q)->Release();
				*q = nullptr;
			}
		issued = false;
	}
};
static GpuTiming g_timing[4];
static int g_timingNext = 0;
static volatile bool g_timingOn = false;
static bool g_timingFailed = false;
static volatile float g_gpuMs = -1.0f;

static void PollTiming()
{
	for (GpuTiming& t : g_timing)
	{
		if (!t.issued)
			continue;
		BOOL disjoint = TRUE;
		UINT64 freq = 0, b = 0, e = 0;
		if (t.disjoint->GetData(&disjoint, sizeof(disjoint), 0) != S_OK || t.freq->GetData(&freq, sizeof(freq), 0) != S_OK ||
			t.begin->GetData(&b, sizeof(b), 0) != S_OK || t.end->GetData(&e, sizeof(e), 0) != S_OK)
			continue;
		t.issued = false;
		if (!disjoint && freq && e >= b)
		{
			float ms = (float)((double)(e - b) * 1000.0 / (double)freq);
			g_gpuMs = g_gpuMs < 0 ? ms : g_gpuMs + (ms - g_gpuMs) * 0.1f; // smoothed
		}
	}
}

// Returns the slot to issue into this frame (null: not measuring, or all slots still in flight).
static GpuTiming* BeginTiming(IDirect3DDevice9* dev)
{
	if (!g_timingOn || g_timingFailed)
		return nullptr;
	PollTiming();
	GpuTiming& t = g_timing[g_timingNext];
	if (t.issued)
		return nullptr;
	if (!t.disjoint)
	{
		if (FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &t.disjoint)) || FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &t.freq)) ||
			FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &t.begin)) || FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &t.end)) ||
			!t.disjoint || !t.freq || !t.begin || !t.end)
		{
			t.Release();
			g_timingFailed = true;
			LogInfo("SSGI: GPU timestamp queries not available; the debug menu can't show the cost.");
			return nullptr;
		}
	}
	g_timingNext = (g_timingNext + 1) % 4;
	t.disjoint->Issue(D3DISSUE_BEGIN);
	t.begin->Issue(D3DISSUE_END);
	return &t;
}

static void EndTiming(GpuTiming* t)
{
	if (!t)
		return;
	t->end->Issue(D3DISSUE_END);
	t->freq->Issue(D3DISSUE_END);
	t->disjoint->Issue(D3DISSUE_END);
	t->issued = true;
}

static void ReleaseTargets()
{
	for (GpuTiming& t : g_timing)
		t.Release();
	for (int i = 0; i < 2; i++)
	{
		g_prep[i].Release();
		g_hist[i].Release();
		g_out[i].Release();
	}
	g_rad.Release();
	g_raw.Release();
	g_tmp.Release();
	g_black.Release();
	g_colourCopy.Release();
	g_colourCopyFmt = D3DFMT_UNKNOWN;
	g_state.Release();
	g_w = g_h = 0;
	g_outValid = g_histValid = g_wroteThisFrame = false;
}

static void Fail(const char* what, HRESULT hr)
{
	LogWarn("SSGI disabled: %s (0x%08lX).", what, hr);
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
		if (caps.NumSimultaneousRTs < 2 || caps.PixelShaderVersion < D3DPS_VERSION(3, 0))
		{
			Fail("the GPU lacks pixel shader 3.0 or 2 render targets", 0);
			return false;
		}
	}
	if (!g_vsFull)
	{
		HRESULT hr;
		if (FAILED(hr = D.CreateVertexShader(dev, kGiFullscreenVs, &g_vsFull)) || FAILED(hr = D.CreatePixelShader(dev, kGiPrepPs, &g_psPrep)) ||
			FAILED(hr = D.CreatePixelShader(dev, kGiTracePs, &g_psTrace)) ||
			FAILED(hr = D.CreatePixelShader(dev, kGiTemporalPs, &g_psTemporal)) ||
			FAILED(hr = D.CreatePixelShader(dev, kGiBlurPs, &g_psBlur)) || FAILED(hr = D.CreatePixelShader(dev, kGiDebugPs, &g_psDebug)) ||
			!(g_decl = core::FullscreenDecl(dev)))
		{
			Fail("could not create its shaders", hr);
			return false;
		}
	}
	if (g_out[0].tex && g_w == w && g_h == h)
		return true;
	ReleaseTargets();
	UINT hw = (w + 1) / 2, hh = (h + 1) / 2;
	const D3DFORMAT F = D3DFMT_A16B16G16R16F;
	bool ok = SUCCEEDED(g_rad.Create(dev, hw, hh, F)) && SUCCEEDED(g_raw.Create(dev, hw, hh, F)) && SUCCEEDED(g_tmp.Create(dev, hw, hh, F)) &&
		SUCCEEDED(g_black.Create(dev, 1, 1, F));
	for (int i = 0; i < 2 && ok; i++)
		ok = SUCCEEDED(g_prep[i].Create(dev, hw, hh, F)) && SUCCEEDED(g_hist[i].Create(dev, hw, hh, F)) && SUCCEEDED(g_out[i].Create(dev, hw, hh, F));
	HRESULT hr = ok ? g_state.Create(dev) : E_FAIL;
	if (!ok || FAILED(hr))
	{
		Fail(ok ? "could not create a state block" : "could not create its render targets", hr);
		return false;
	}
	dev->ColorFill(g_black.surf, NULL, 0);
	g_w = w;
	g_h = h;
	g_hw = hw;
	g_hh = hh;
	LogInfo("SSGI: render targets created for %ux%u (GI at %ux%u).", w, h, hw, hh);
	return true;
}

static bool g_inPass, g_pending, g_giBound;
static IDirect3DSurface9 *g_depthSurf, *g_colourSurf;
static IDirect3DBaseTexture9* g_gameTex13;

void GI_OnLostDevice()
{
	ReleaseTargets();
	g_inPass = g_pending = g_giBound = false;
	g_depthSurf = g_colourSurf = nullptr;
	g_gameTex13 = nullptr;
}

// ---- Opaque pass tracking ----------------------------------------------------------------------

// g_inPass: the game's z/w target is bound as RT1. g_pending: the pass ended; our passes are due
// before the next thing that draws. g_giBound: s13 holds our texture (else black or the game's).
// g_depthSurf / g_colourSurf: the game's z/w and scene colour targets of the pass. g_gameTex13:
// what the game itself last bound to s13. (Declared above GI_OnLostDevice.)
static bool g_done = false;      // our passes ran this frame (or were skipped for good)
static bool g_constsDirty = false;
static bool g_cut = false;       // this frame's camera jumped: no GI from the last frame
static int g_passDraws = 0;

void GI_OnSetTexture(DWORD stage, IDirect3DBaseTexture9* tex)
{
	if (stage == GI_SAMPLER)
	{
		g_gameTex13 = tex;
		g_giBound = false;
		g_constsDirty = true; // rebind ours before the next draw in the pass
	}
}

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
			LogWarn("SSGI: the scene is multisampled (in-game anti-aliasing); SSGI is not supported with it.");
		}
		return false;
	}
	if (!b.zwTexture)
		return false;
	w = b.width;
	h = b.height;
	return true;
}

// The GI texture the game's shaders should read now: last frame's result, or black.
static IDirect3DBaseTexture9* GiTexture()
{
	if (GI_Active() && g_outValid && !g_cut)
		return g_out[g_outBound].tex;
	return g_black.tex;
}

static void BindGi(IDirect3DDevice9* dev, bool frameKnown)
{
	if (!g_black.tex)
		return;
	D.SetTexture(dev, GI_SAMPLER, frameKnown ? GiTexture() : g_black.tex);
	core::SetSamplerClamp(dev, GI_SAMPLER, D3DTEXF_LINEAR);
	g_giBound = true;
}

// While the scene colour isn't the target (reflections, shadow maps, post-processing), the
// rewritten shaders get black: their pixels aren't where our texture expects them.
static void UnbindGi(IDirect3DDevice9* dev)
{
	if (!g_giBound)
		return;
	D.SetTexture(dev, GI_SAMPLER, g_gameTex13 ? g_gameTex13 : g_black.tex);
	g_giBound = false;
}

static void RunPasses(IDirect3DDevice9* dev);

bool GI_BeforeSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s)
{
	if (!g_pending || index != 0 || s == g_colourSurf)
		return false;
	RunPasses(dev);
	return true;
}

void GI_AfterSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s, IDirect3DSurface9* rt0)
{
	if (!g_enabled || g_failed)
		return;
	if (index == 0)
	{
		// Leaving the scene colour: hide our texture. Coming back (transparents): show it again.
		if (s != g_colourSurf)
			UnbindGi(dev);
		else if (g_colourSurf && !g_giBound)
			g_constsDirty = true;
		return;
	}
	if (index != 1)
		return;
	if (s)
	{
		UINT w, h;
		if (g_done || !IsOpaquePass(s, rt0, w, h) || !EnsureResources(dev, w, h))
			return;
		if (g_historyReset)
		{
			// Toggled: start again from this frame (no light from before the toggle).
			g_historyReset = false;
			g_outValid = g_histValid = false;
		}
		g_inPass = true;
		g_pending = false;
		g_depthSurf = s;
		g_colourSurf = rt0;
		g_constsDirty = true;
	}
	else if (g_inPass)
	{
		g_inPass = false;
		if (g_passDraws > 0)
			g_pending = true;
	}
}

// Rows for the shaders and the temporal pass, and whether last frame's GI is usable.
static void SetFrameConstants(IDirect3DDevice9* dev)
{
	float rows[3][4];
	bool reproject = g_outValid && g_camOk && core::ReprojectionRows(g_prevCam.viewProj, Cam().viewProj, g_w, g_h, rows);
	if (!reproject)
		core::IdentityRows(g_w, g_h, rows);
	dev->SetPixelShaderConstantF(GI_CONST, &rows[0][0], 3);
}

static void CheckCut()
{
	if (!g_camOk || !g_outValid)
		return;
	if (core::IsCameraCut(Cam(), g_prevCam))
	{
		g_cut = true;
		g_histValid = false;
	}
}

bool GI_BeforeDraw(IDirect3DDevice9* dev)
{
	if (g_pending)
	{
		RunPasses(dev);
		return true;
	}
	if (g_inPass)
	{
		g_passDraws++;
		if (!g_camOk && Cam().ok)
		{
			// The first opaque draw with the camera matrices set this frame's camera.
			g_camOk = true;
			CheckCut();
			g_constsDirty = true;
		}
	}
	if (!g_constsDirty || !g_colourSurf || !g_black.tex)
		return false;
	bool scene = g_inPass;
	if (!scene)
	{
		// Transparents etc. after the pass: only while drawing into the scene colour.
		IDirect3DSurface9* rt = nullptr;
		if (SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt)
		{
			scene = rt == g_colourSurf;
			rt->Release();
		}
	}
	if (!scene)
		return false;
	if (g_camOk)
		SetFrameConstants(dev);
	BindGi(dev, g_camOk);
	g_constsDirty = !g_camOk && g_inPass; // try the camera again on the next draw
	return false;
}

bool GI_InPass() { return g_inPass; }

bool GI_BeforeOther(IDirect3DDevice9* dev)
{
	if (!g_pending)
		return false;
	RunPasses(dev);
	return true;
}

bool GI_BeforePresent(IDirect3DDevice9* dev)
{
	bool ran = GI_BeforeOther(dev);
	g_inPass = false;
	if (g_wroteThisFrame)
	{
		g_outBound = 1 - g_outBound;
		g_outValid = true;
		g_histValid = true;
		g_cur = 1 - g_cur;
		g_prevCam = Cam();
	}
	if (!GI_Active())
		g_outValid = g_histValid = false;
	g_done = g_pending = g_wroteThisFrame = g_cut = false;
	g_camOk = false;
	g_passDraws = 0;
	return ran;
}

// ---- The passes --------------------------------------------------------------------------------

// The scene colour as a texture: the target itself if it is one, else a copy.
static IDirect3DTexture9* ColourTexture(IDirect3DDevice9* dev)
{
	IDirect3DTexture9* tex = nullptr;
	if (SUCCEEDED(g_colourSurf->GetContainer(core::kIID_Texture9, (void**)&tex)) && tex)
	{
		tex->Release(); // the game holds it
		return tex;
	}
	D3DSURFACE_DESC d = {};
	if (FAILED(g_colourSurf->GetDesc(&d)))
		return nullptr;
	if (!g_colourCopy.tex || g_colourCopyFmt != d.Format)
	{
		g_colourCopy.Release();
		if (FAILED(g_colourCopy.Create(dev, d.Width, d.Height, d.Format)))
		{
			g_colourCopy.Release();
			static bool warned = false;
			if (!warned)
			{
				warned = true;
				LogWarn("SSGI: the scene colour isn't a texture and can't be copied (format %d).", (int)d.Format);
			}
			return nullptr;
		}
		g_colourCopyFmt = d.Format;
		LogInfo("SSGI: the scene colour isn't a texture; copying it each frame.");
	}
	if (FAILED(D.StretchRect(dev, g_colourSurf, NULL, g_colourCopy.surf, NULL, D3DTEXF_NONE)))
		return nullptr;
	return g_colourCopy.tex;
}

static void RunPasses(IDirect3DDevice9* dev)
{
	g_pending = false;
	g_done = true;
	if (!GI_Active())
		return;
	if (!g_camOk)
	{
		static bool warned = false;
		if (!warned)
		{
			warned = true;
			LogWarn("SSGI: skipped a frame without camera matrices (logged once).");
		}
		return;
	}
	if (!g_state.Ready() || !g_depthSurf || !g_colourSurf)
		return;
	IDirect3DTexture9* depthTex = nullptr;
	if (FAILED(g_depthSurf->GetContainer(core::kIID_Texture9, (void**)&depthTex)) || !depthTex)
		return;

	g_state.Save(dev);
	IDirect3DTexture9* colourTex = ColourTexture(dev);
	g_state.BeginPasses(dev, g_decl, g_vsFull);
	dev->SetRenderState(D3DRS_COLORWRITEENABLE1, 0xF); // the prep pass writes two targets
	for (DWORD s = 0; s < 4; s++)
		core::SetSamplerClamp(dev, s, D3DTEXF_POINT);

	const int cur = g_cur, prev = 1 - g_cur;
	float rows[3][4];
	const core::CameraFloats& cam = Cam().f;
	bool reproject = g_histValid && !g_cut && core::ReprojectionRows(g_prevCam.viewProj, Cam().viewProj, g_w, g_h, rows);
	if (!reproject)
		core::IdentityRows(g_w, g_h, rows);
	const float c[11][4] = {
		{ (float)g_w, (float)g_h, 1.0f / g_w, 1.0f / g_h },
		{ (float)g_hw, (float)g_hh, 1.0f / g_hw, 1.0f / g_hh },
		{ cam.A, cam.B, cam.P00, cam.P11 },
		{ cam.P02, cam.P12, (float)(g_frame % 64), g_radianceClamp },
		{ g_radius, 0.5f * cam.P00 * g_w, g_maxScreenRadius * g_h, g_thickness },
		{ (float)g_slices, (float)g_steps, g_intensity, (float)g_view },
		{ rows[0][0], rows[0][1], rows[0][2], rows[0][3] },
		{ rows[1][0], rows[1][1], rows[1][2], rows[1][3] },
		{ rows[2][0], rows[2][1], rows[2][2], rows[2][3] },
		{ g_minBlend, g_depthTolerance, reproject ? 1.0f : 0.0f, 1.0f / g_minBlend },
		{ 1.5f, 0, 0, 0 },
	};
	dev->SetPixelShaderConstantF(0, &c[0][0], 11);
	g_frame++;

	GpuTiming* timing = colourTex ? BeginTiming(dev) : nullptr;
	if (colourTex)
	{
		// Prep: normals + depth, and the lit colour at half res.
		core::SetTarget(dev, g_prep[cur].surf, g_hw, g_hh);
		D.SetRenderTarget(dev, 1, g_rad.surf);
		D.SetTexture(dev, 0, depthTex);
		D.SetTexture(dev, 1, colourTex);
		core::SetSamplerClamp(dev, 1, D3DTEXF_LINEAR);
		D.SetPixelShader(dev, g_psPrep);
		core::DrawFullscreen(dev);
		D.SetRenderTarget(dev, 1, nullptr);
		core::SetSamplerClamp(dev, 1, D3DTEXF_POINT);

		// Trace.
		core::SetTarget(dev, g_raw.surf, g_hw, g_hh);
		D.SetTexture(dev, 0, g_prep[cur].tex);
		D.SetTexture(dev, 1, g_rad.tex);
		D.SetPixelShader(dev, g_psTrace);
		core::DrawFullscreen(dev);

		// Temporal.
		core::SetTarget(dev, g_hist[cur].surf, g_hw, g_hh);
		D.SetTexture(dev, 1, g_raw.tex);
		D.SetTexture(dev, 2, g_hist[prev].tex);
		D.SetTexture(dev, 3, g_prep[prev].tex);
		D.SetPixelShader(dev, g_psTemporal);
		core::DrawFullscreen(dev);
		D.SetTexture(dev, 2, nullptr);
		D.SetTexture(dev, 3, nullptr);

		// Blur across, then down into the texture the game reads next frame (x intensity).
		const int next = 1 - g_outBound;
		core::SetTarget(dev, g_tmp.surf, g_hw, g_hh);
		D.SetTexture(dev, 1, g_hist[cur].tex);
		D.SetPixelShader(dev, g_psBlur);
		core::DrawFullscreen(dev);
		const float down[4] = { 0, 1.5f, 1, 0 };
		dev->SetPixelShaderConstantF(10, down, 1);
		core::SetTarget(dev, g_out[next].surf, g_hw, g_hh);
		D.SetTexture(dev, 1, g_tmp.tex);
		core::DrawFullscreen(dev);
		g_wroteThisFrame = true;

		if (g_view)
		{
			core::SetTarget(dev, g_colourSurf, g_w, g_h);
			D.SetTexture(dev, 1, g_view == 2 ? g_raw.tex : g_out[next].tex);
			D.SetTexture(dev, 2, g_prep[cur].tex);
			core::SetSamplerClamp(dev, 1, D3DTEXF_LINEAR);
			D.SetPixelShader(dev, g_psDebug);
			core::DrawFullscreen(dev);
		}
	}

	EndTiming(timing);

	g_state.Restore(dev);
	depthTex->Release();

	static bool first = true;
	if (first && g_wroteThisFrame)
	{
		first = false;
		LogInfo("SSGI: first frame traced.");
	}
}

// ---- Status (debug menu) -------------------------------------------------------------------------

GiStatus GI_GetStatus()
{
	GiStatus st;
	st.framesTraced = g_frame;
	st.failed = g_failed;
	st.traceW = g_hw;
	st.traceH = g_hh;
	st.gpuMs = g_gpuMs;
	return st;
}

void GI_SetTiming(bool on)
{
	g_timingOn = on;
	if (!on)
		g_gpuMs = -1.0f;
}

} // namespace ssgi
