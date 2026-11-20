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

// ---- Vertex shaders: where the camera matrices are --------------------------------------------

struct VsInfo
{
	int wvp = -1, world = -1;
};
static CRITICAL_SECTION g_vsLock;
static std::unordered_map<void*, VsInfo> g_vsInfo;
static volatile LONG g_vsLockState = 0; // 0 none, 1 initialising, 2 ready

static bool LockReady()
{
	if (g_vsLockState == 2)
		return true;
	if (InterlockedCompareExchange(&g_vsLockState, 1, 0) == 0)
	{
		InitializeCriticalSection(&g_vsLock);
		g_vsLockState = 2;
	}
	while (g_vsLockState != 2)
		Sleep(0);
	return true;
}

// Minimal vs_3_0 constant table reader.
static bool FindVsMatrices(const DWORD* code, VsInfo& info)
{
	if (!code || (code[0] & 0xFFFF0000) != 0xFFFE0000)
		return false;
	for (size_t i = 1; i < 4096 && code[i] != 0x0000FFFF;)
	{
		DWORD tok = code[i];
		if ((tok & 0xFFFF) != 0xFFFE) // CTAB comes first, before any instruction
			return false;
		size_t len = (tok >> 16) & 0x7FFF;
		if (len >= 8 && code[i + 1] == 0x42415443)
		{
			const uint8_t* base = (const uint8_t*)&code[i + 2];
			size_t size = (len - 1) * 4;
			auto rd32 = [&](size_t o) -> DWORD { return o + 4 <= size ? *(const DWORD*)(base + o) : 0; };
			auto rd16 = [&](size_t o) -> WORD { return o + 2 <= size ? *(const WORD*)(base + o) : 0; };
			DWORD count = rd32(12), infoOff = rd32(16);
			for (DWORD k = 0; k < count && infoOff + 20 * (k + 1) <= size; k++)
			{
				size_t e = infoOff + 20 * k;
				DWORD nameOff = rd32(e);
				WORD set = rd16(e + 4), index = rd16(e + 6), regs = rd16(e + 8);
				char name[32] = {};
				for (size_t c = 0; c < sizeof(name) - 1 && nameOff + c < size && base[nameOff + c]; c++)
					name[c] = (char)base[nameOff + c];
				if (set == 2 && regs == 4 && strcmp(name, "g_WorldViewProj") == 0)
					info.wvp = index;
				if (set == 2 && regs == 4 && strcmp(name, "g_World") == 0)
					info.world = index;
			}
			return info.wvp >= 0 && info.world >= 0;
		}
		i += 1 + len;
	}
	return false;
}

void GI_OnCreateVertexShader(IDirect3DVertexShader9* vs, const DWORD* code)
{
	if (!g_enabled || !vs || !LockReady())
		return;
	VsInfo info;
	bool found = FindVsMatrices(code, info);
	EnterCriticalSection(&g_vsLock);
	if (found)
		g_vsInfo[vs] = info;
	else
		g_vsInfo.erase(vs); // the address may have belonged to a released shader
	LeaveCriticalSection(&g_vsLock);
}

// ---- Camera ------------------------------------------------------------------------------------

static Camera g_cam;          // this frame's (first opaque draw)
static bool g_camWarned = false;

static void CaptureCamera(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs)
{
	if (!vs || g_vsLockState != 2)
		return;
	VsInfo info;
	EnterCriticalSection(&g_vsLock);
	auto it = g_vsInfo.find(vs);
	if (it != g_vsInfo.end())
		info = it->second;
	LeaveCriticalSection(&g_vsLock);
	if (info.wvp < 0)
		return;
	float wvp[16], world[16];
	if (FAILED(dev->GetVertexShaderConstantF(info.wvp, wvp, 4)) || FAILED(dev->GetVertexShaderConstantF(info.world, world, 4)))
		return;
	char why[160];
	Camera cam;
	if (CameraFromMatrices(wvp, world, cam, why, sizeof(why)))
	{
		static bool logged = false;
		if (!logged)
		{
			logged = true;
			LogInfo("SSGI: camera found (near %.3f m, far %.0f m, horizontal fov %.1f deg).", -cam.B / cam.A, cam.B / (1.0f - cam.A),
				2.0 * atan(1.0 / cam.P00) * 57.29578);
		}
		g_cam = cam;
	}
	else if (!g_camWarned)
	{
		g_camWarned = true;
		LogWarn("SSGI: could not read the camera from the shader constants (%s).", why);
	}
}

// ---- Device resources --------------------------------------------------------------------------

static GiDeviceFns D;
static const GUID kIID_Texture9 = { 0x85c31227, 0x3de5, 0x4f00, { 0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5 } };
void GI_SetDeviceFns(const GiDeviceFns& fns) { D = fns; }

struct Target
{
	IDirect3DTexture9* tex = nullptr;
	IDirect3DSurface9* surf = nullptr;
	bool Create(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt)
	{
		if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &tex, NULL)) || !tex)
			return false;
		return SUCCEEDED(tex->GetSurfaceLevel(0, &surf)) && surf;
	}
	void Release()
	{
		if (surf)
			surf->Release();
		if (tex)
			tex->Release();
		surf = nullptr;
		tex = nullptr;
	}
};

static Target g_prep[2], g_rad, g_raw, g_hist[2], g_tmp, g_out[2], g_black, g_colourCopy;
static D3DFORMAT g_colourCopyFmt = D3DFMT_UNKNOWN;
static UINT g_w, g_h, g_hw, g_hh;
static IDirect3DStateBlock9* g_state;
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
static Mat4 g_prevVP;             // camera of the frame that wrote g_out[g_outBound] / the history
static double g_prevEye[3], g_prevFwd[3];
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
	if (g_state)
		g_state->Release();
	g_state = nullptr;
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
		const D3DVERTEXELEMENT9 decl[] = { { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 }, D3DDECL_END() };
		if (FAILED(hr = D.CreateVertexShader(dev, kGiFullscreenVs, &g_vsFull)) || FAILED(hr = D.CreatePixelShader(dev, kGiPrepPs, &g_psPrep)) ||
			FAILED(hr = D.CreatePixelShader(dev, kGiTracePs, &g_psTrace)) ||
			FAILED(hr = D.CreatePixelShader(dev, kGiTemporalPs, &g_psTemporal)) ||
			FAILED(hr = D.CreatePixelShader(dev, kGiBlurPs, &g_psBlur)) || FAILED(hr = D.CreatePixelShader(dev, kGiDebugPs, &g_psDebug)) ||
			FAILED(hr = dev->CreateVertexDeclaration(decl, &g_decl)))
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
	bool ok = g_rad.Create(dev, hw, hh, F) && g_raw.Create(dev, hw, hh, F) && g_tmp.Create(dev, hw, hh, F) && g_black.Create(dev, 1, 1, F);
	for (int i = 0; i < 2 && ok; i++)
		ok = g_prep[i].Create(dev, hw, hh, F) && g_hist[i].Create(dev, hw, hh, F) && g_out[i].Create(dev, hw, hh, F);
	HRESULT hr = ok ? dev->CreateStateBlock(D3DSBT_ALL, &g_state) : E_FAIL;
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
	if (!s || !rt0)
		return false;
	D3DSURFACE_DESC d = {}, c = {};
	if (FAILED(s->GetDesc(&d)) || FAILED(rt0->GetDesc(&c)))
		return false;
	if (d.Format != D3DFMT_R32F || !(d.Usage & D3DUSAGE_RENDERTARGET) || d.MultiSampleType != D3DMULTISAMPLE_NONE)
		return false;
	if (c.Width != d.Width || c.Height != d.Height)
		return false;
	if (c.MultiSampleType != D3DMULTISAMPLE_NONE)
	{
		static bool warned = false;
		if (!warned)
		{
			warned = true;
			LogWarn("SSGI: the scene is multisampled (in-game anti-aliasing); SSGI is not supported with it.");
		}
		return false;
	}
	IDirect3DTexture9* tex = nullptr;
	if (FAILED(s->GetContainer(kIID_Texture9, (void**)&tex)) || !tex)
		return false;
	tex->Release();
	w = d.Width;
	h = d.Height;
	return true;
}

static void SetLinearClamp(IDirect3DDevice9* dev, DWORD s, bool linear)
{
	dev->SetSamplerState(s, D3DSAMP_MINFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
	dev->SetSamplerState(s, D3DSAMP_MAGFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
	dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
	dev->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, 0);
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
	SetLinearClamp(dev, GI_SAMPLER, true);
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
	bool reproject = g_outValid && g_cam.ok && ReprojectionRows(g_prevVP, g_cam.viewProj, g_w, g_h, rows);
	if (!reproject)
		IdentityRows(g_w, g_h, rows);
	dev->SetPixelShaderConstantF(GI_CONST, &rows[0][0], 3);
}

static void CheckCut()
{
	if (!g_cam.ok || !g_outValid)
		return;
	double d2 = 0, dot = 0;
	for (int k = 0; k < 3; k++)
	{
		double e = g_cam.eye[k] - g_prevEye[k];
		d2 += e * e;
		dot += g_cam.forward[k] * g_prevFwd[k];
	}
	if (d2 > 4.0 * 4.0 || dot < 0.85) // 4 m or about 32 degrees in one frame
	{
		g_cut = true;
		g_histValid = false;
	}
}

bool GI_BeforeDraw(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs)
{
	if (g_pending)
	{
		RunPasses(dev);
		return true;
	}
	if (g_inPass)
	{
		g_passDraws++;
		if (!g_cam.ok)
		{
			// The first opaque draw with the camera matrices sets this frame's camera.
			CaptureCamera(dev, vs);
			if (g_cam.ok)
			{
				CheckCut();
				g_constsDirty = true;
			}
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
	if (g_cam.ok)
		SetFrameConstants(dev);
	BindGi(dev, g_cam.ok);
	g_constsDirty = !g_cam.ok && g_inPass; // try the camera again on the next draw
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
		g_prevVP = g_cam.viewProj;
		for (int k = 0; k < 3; k++)
		{
			g_prevEye[k] = g_cam.eye[k];
			g_prevFwd[k] = g_cam.forward[k];
		}
	}
	if (!GI_Active())
		g_outValid = g_histValid = false;
	g_done = g_pending = g_wroteThisFrame = g_cut = false;
	g_cam.ok = false;
	g_passDraws = 0;
	return ran;
}

// ---- The passes --------------------------------------------------------------------------------

static void Fullscreen(IDirect3DDevice9* dev)
{
	static const float tri[3][4] = { { -1, -1, 0, 1 }, { -1, 3, 0, 1 }, { 3, -1, 0, 1 } };
	D.DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0]));
}

static void SetTarget(IDirect3DDevice9* dev, IDirect3DSurface9* s, UINT w, UINT h)
{
	D.SetRenderTarget(dev, 0, s);
	D3DVIEWPORT9 vp = { 0, 0, w, h, 0.0f, 1.0f };
	dev->SetViewport(&vp);
}

// The scene colour as a texture: the target itself if it is one, else a copy.
static IDirect3DTexture9* ColourTexture(IDirect3DDevice9* dev)
{
	IDirect3DTexture9* tex = nullptr;
	if (SUCCEEDED(g_colourSurf->GetContainer(kIID_Texture9, (void**)&tex)) && tex)
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
		if (!g_colourCopy.Create(dev, d.Width, d.Height, d.Format))
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
	if (!g_cam.ok)
	{
		static bool warned = false;
		if (!warned)
		{
			warned = true;
			LogWarn("SSGI: skipped a frame without camera matrices (logged once).");
		}
		return;
	}
	if (!g_state || !g_depthSurf || !g_colourSurf)
		return;
	IDirect3DTexture9* depthTex = nullptr;
	if (FAILED(g_depthSurf->GetContainer(kIID_Texture9, (void**)&depthTex)) || !depthTex)
		return;

	// Save everything we touch: state block for states, shaders, constants, textures, viewport; render
	// targets, depth buffer and vertex input by hand (the game draws instanced foliage, and our
	// DrawPrimitiveUP calls reset stream 0 and must run with instancing off).
	g_state->Capture();
	IDirect3DSurface9* oldRt[4] = {};
	IDirect3DSurface9* oldDs = nullptr;
	for (DWORD i = 0; i < 4; i++)
		if (FAILED(dev->GetRenderTarget(i, &oldRt[i])))
			oldRt[i] = nullptr;
	if (FAILED(dev->GetDepthStencilSurface(&oldDs)))
		oldDs = nullptr;
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

	IDirect3DTexture9* colourTex = ColourTexture(dev);

	const DWORD rs[][2] = {
		{ D3DRS_ZENABLE, FALSE }, { D3DRS_ZWRITEENABLE, FALSE }, { D3DRS_STENCILENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE },
		{ D3DRS_ALPHABLENDENABLE, FALSE }, { D3DRS_SEPARATEALPHABLENDENABLE, FALSE }, { D3DRS_CULLMODE, D3DCULL_NONE },
		{ D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_COLORWRITEENABLE, 0xF }, { D3DRS_COLORWRITEENABLE1, 0xF },
		{ D3DRS_SRGBWRITEENABLE, FALSE }, { D3DRS_FOGENABLE, FALSE }, { D3DRS_CLIPPLANEENABLE, 0 }, { D3DRS_FILLMODE, D3DFILL_SOLID },
		{ D3DRS_TWOSIDEDSTENCILMODE, FALSE }, { D3DRS_POINTSPRITEENABLE, FALSE },
	};
	for (const auto& r : rs)
		dev->SetRenderState((D3DRENDERSTATETYPE)r[0], r[1]);
	for (DWORD s = 0; s < 4; s++)
		SetLinearClamp(dev, s, false);
	D.SetDepthStencilSurface(dev, nullptr);
	for (DWORD i = 1; i < 4; i++)
		if (oldRt[i])
			D.SetRenderTarget(dev, i, nullptr);
	dev->SetVertexDeclaration(g_decl);
	D.SetVertexShader(dev, g_vsFull);

	const int cur = g_cur, prev = 1 - g_cur;
	float rows[3][4];
	bool reproject = g_histValid && !g_cut && ReprojectionRows(g_prevVP, g_cam.viewProj, g_w, g_h, rows);
	if (!reproject)
		IdentityRows(g_w, g_h, rows);
	const float c[11][4] = {
		{ (float)g_w, (float)g_h, 1.0f / g_w, 1.0f / g_h },
		{ (float)g_hw, (float)g_hh, 1.0f / g_hw, 1.0f / g_hh },
		{ g_cam.A, g_cam.B, g_cam.P00, g_cam.P11 },
		{ g_cam.P02, g_cam.P12, (float)(g_frame % 64), g_radianceClamp },
		{ g_radius, 0.5f * g_cam.P00 * g_w, g_maxScreenRadius * g_h, g_thickness },
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
		SetTarget(dev, g_prep[cur].surf, g_hw, g_hh);
		D.SetRenderTarget(dev, 1, g_rad.surf);
		D.SetTexture(dev, 0, depthTex);
		D.SetTexture(dev, 1, colourTex);
		SetLinearClamp(dev, 1, true);
		D.SetPixelShader(dev, g_psPrep);
		Fullscreen(dev);
		D.SetRenderTarget(dev, 1, nullptr);
		SetLinearClamp(dev, 1, false);

		// Trace.
		SetTarget(dev, g_raw.surf, g_hw, g_hh);
		D.SetTexture(dev, 0, g_prep[cur].tex);
		D.SetTexture(dev, 1, g_rad.tex);
		D.SetPixelShader(dev, g_psTrace);
		Fullscreen(dev);

		// Temporal.
		SetTarget(dev, g_hist[cur].surf, g_hw, g_hh);
		D.SetTexture(dev, 1, g_raw.tex);
		D.SetTexture(dev, 2, g_hist[prev].tex);
		D.SetTexture(dev, 3, g_prep[prev].tex);
		D.SetPixelShader(dev, g_psTemporal);
		Fullscreen(dev);
		D.SetTexture(dev, 2, nullptr);
		D.SetTexture(dev, 3, nullptr);

		// Blur across, then down into the texture the game reads next frame (x intensity).
		const int next = 1 - g_outBound;
		SetTarget(dev, g_tmp.surf, g_hw, g_hh);
		D.SetTexture(dev, 1, g_hist[cur].tex);
		D.SetPixelShader(dev, g_psBlur);
		Fullscreen(dev);
		const float down[4] = { 0, 1.5f, 1, 0 };
		dev->SetPixelShaderConstantF(10, down, 1);
		SetTarget(dev, g_out[next].surf, g_hw, g_hh);
		D.SetTexture(dev, 1, g_tmp.tex);
		Fullscreen(dev);
		g_wroteThisFrame = true;

		if (g_view)
		{
			SetTarget(dev, g_colourSurf, g_w, g_h);
			D.SetTexture(dev, 1, g_view == 2 ? g_raw.tex : g_out[next].tex);
			D.SetTexture(dev, 2, g_prep[cur].tex);
			SetLinearClamp(dev, 1, true);
			D.SetPixelShader(dev, g_psDebug);
			Fullscreen(dev);
		}
	}

	EndTiming(timing);

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
