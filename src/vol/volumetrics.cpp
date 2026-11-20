// Volumetric fog and sun shafts: settings, what we read from the game's shaders, the frame
// tracking and the passes. See volumetrics.h for the overview.
//
// Camera: this frame's, from the shared scene tracker (core/scene.h; the maths, from the opaque
// shaders' g_WorldViewProj and g_World, in core/camera.h), taken during our pass.
//
// Sun shadow map: the lit shaders transform world positions with g_WorldToLightProjVS (3 rows:
// x, y across the sun in metres, centred near the camera; z along the direction towards the sun),
// pick a cascade by max(|x|, |y|) against g_ShadowCascadesRangesMin/Max, map to the atlas with
// g_ShadowScales/Offsets and compare z * g_ShadowNearFar.x + .y with the stored depth (lit when
// stored >= depth). The fog does the same, one tap per ray march step.

#include "volumetrics.h"
#include "common.h"
#include "shaders_gen.h"
#include "core/scene.h"
#include "core/fxpass.h"
#include "core/shaderinfo.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unordered_map>

namespace vol
{

VolSettings g_set;

// ---- Settings ----------------------------------------------------------------------------------

static float Clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static int Clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void Vol_ClampSettings()
{
	g_set.density = Clampf(g_set.density, 0.0f, 0.1f);
	g_set.heightFalloff = Clampf(g_set.heightFalloff, 0.0f, 1.0f);
	g_set.baseHeight = Clampf(g_set.baseHeight, -1000.0f, 5000.0f);
	g_set.sunIntensity = Clampf(g_set.sunIntensity, 0.0f, 20.0f);
	g_set.anisotropy = Clampf(g_set.anisotropy, 0.0f, 0.9f);
	g_set.ambientIntensity = Clampf(g_set.ambientIntensity, 0.0f, 10.0f);
	g_set.maxDistance = Clampf(g_set.maxDistance, 25.0f, 3000.0f);
	g_set.steps = Clampi(g_set.steps, 8, 128);
	g_set.resolution = Clampi(g_set.resolution, 1, 4);
	g_set.placement = Clampi(g_set.placement, 0, 1);
	g_set.view = Clampi(g_set.view, 0, 3);
	g_set.shaftsIntensity = Clampf(g_set.shaftsIntensity, 0.0f, 10.0f);
	g_set.shaftsLength = Clampf(g_set.shaftsLength, 0.05f, 1.0f);
	g_set.shaftsThreshold = Clampf(g_set.shaftsThreshold, 0.0f, 0.95f);
	g_set.shaftsFalloff = Clampf(g_set.shaftsFalloff, 1.0f, 200.0f);
	g_set.shaftsDecay = Clampf(g_set.shaftsDecay, 0.5f, 1.0f);
}

void Vol_LoadSettings()
{
	const char* F = "VolumetricFog";
	g_set.density = IniFloat(F, "Density", g_set.density);
	g_set.heightFalloff = IniFloat(F, "HeightFalloff", g_set.heightFalloff);
	g_set.baseHeight = IniFloat(F, "BaseHeight", g_set.baseHeight);
	g_set.sunIntensity = IniFloat(F, "SunIntensity", g_set.sunIntensity);
	g_set.anisotropy = IniFloat(F, "Anisotropy", g_set.anisotropy);
	g_set.ambientIntensity = IniFloat(F, "AmbientIntensity", g_set.ambientIntensity);
	g_set.maxDistance = IniFloat(F, "MaxDistance", g_set.maxDistance);
	g_set.steps = IniInt(F, "Steps", g_set.steps);
	g_set.resolution = IniInt(F, "Resolution", g_set.resolution);
	g_set.placement = IniInt(F, "Placement", g_set.placement);
	g_set.view = IniInt("VolumetricFog", "DebugView", g_set.view);
	const char* S = "SunShafts";
	g_set.shaftsIntensity = IniFloat(S, "Intensity", g_set.shaftsIntensity);
	g_set.shaftsLength = IniFloat(S, "Length", g_set.shaftsLength);
	g_set.shaftsThreshold = IniFloat(S, "Threshold", g_set.shaftsThreshold);
	g_set.shaftsFalloff = IniFloat(S, "SunFalloff", g_set.shaftsFalloff);
	g_set.shaftsDecay = IniFloat(S, "Decay", g_set.shaftsDecay);
	Vol_ClampSettings();
}

static void InitLock();

bool Vol_Init()
{
	InitLock();
	g_set.fogConfigured = IniInt("VolumetricFog", "Enabled", 1) != 0;
	g_set.shaftsConfigured = IniInt("SunShafts", "Enabled", 1) != 0;
	g_set.fogOn = g_set.fogConfigured && IniInt("VolumetricFog", "StartEnabled", 1) != 0;
	g_set.shaftsOn = g_set.shaftsConfigured && IniInt("SunShafts", "StartEnabled", 1) != 0;
	Vol_LoadSettings();
	if (!g_set.fogConfigured && !g_set.shaftsConfigured)
	{
		LogInfo("Volumetric fog and sun shafts are both disabled in the ini; doing nothing.");
		return false;
	}
	LogInfo("Volumetric fog: %s (density %.4f/m, height falloff %.3f/m above %.1f m, sun %.2f, g %.2f, ambient %.2f, %.0f m, "
			"%d steps at 1/%d resolution, %s).",
		!g_set.fogConfigured ? "disabled" : g_set.fogOn ? "on" : "starting off", (double)g_set.density, (double)g_set.heightFalloff,
		(double)g_set.baseHeight, (double)g_set.sunIntensity, (double)g_set.anisotropy, (double)g_set.ambientIntensity,
		(double)g_set.maxDistance, g_set.steps, g_set.resolution, g_set.placement ? "after transparents" : "before transparents");
	LogInfo("Sun shafts: %s (intensity %.2f, length %.2f, threshold %.2f, sun falloff %.1f, decay %.3f).",
		!g_set.shaftsConfigured ? "disabled" : g_set.shaftsOn ? "on" : "starting off", (double)g_set.shaftsIntensity,
		(double)g_set.shaftsLength, (double)g_set.shaftsThreshold, (double)g_set.shaftsFalloff, (double)g_set.shaftsDecay);
	return true;
}

// ---- Shader constant tables: where each shader keeps what we need ------------------------------

// Vertex shaders: the sun shadow atlas's light-space matrix, and the eye position (only to check the
// camera against it in the log). The camera matrices are the scene tracker's.
struct VsInfo
{
	int light = -1, eye = -1;
};
struct PsInfo
{
	int sun = -1, fogColor = -1, nearFar = -1, rangeMin = -1, rangeMax = -1, scaleX = -1, scaleY = -1, offsetX = -1, offsetY = -1,
		bias = -1, fade = -1, cloudUV = -1, shadowSampler = -1, cloudSampler = -1;
	bool Shadow() const
	{
		return nearFar >= 0 && rangeMin >= 0 && rangeMax >= 0 && scaleX >= 0 && scaleY >= 0 && offsetX >= 0 && offsetY >= 0 &&
			fade >= 0 && shadowSampler >= 0;
	}
	bool Any() const { return sun >= 0 || fogColor >= 0 || Shadow() || (cloudUV >= 0 && cloudSampler >= 0); }
};

static CRITICAL_SECTION g_mapLock;
static std::unordered_map<void*, VsInfo> g_vsInfo;
static std::unordered_map<void*, PsInfo> g_psInfo;

static void InitLock()
{
	static bool ready = false;
	if (!ready)
	{
		InitializeCriticalSection(&g_mapLock);
		ready = true;
	}
}

void Vol_OnCreateVertexShader(IDirect3DVertexShader9* vs, const DWORD* code)
{
	if (!vs)
		return;
	VsInfo info;
	core::ConstantTable t;
	if (core::ReadConstantTable(code, t))
	{
		info.light = t.Register("g_WorldToLightProjVS", core::RS_FLOAT4, 3);
		info.eye = t.Register("g_EyePosition", core::RS_FLOAT4);
	}
	EnterCriticalSection(&g_mapLock);
	if (info.light >= 0 || info.eye >= 0)
		g_vsInfo[vs] = info;
	else
		g_vsInfo.erase(vs); // the address may have belonged to a released shader
	LeaveCriticalSection(&g_mapLock);
}

void Vol_OnCreatePixelShader(IDirect3DPixelShader9* ps, const DWORD* code)
{
	if (!ps)
		return;
	PsInfo info;
	core::ConstantTable t;
	if (core::ReadConstantTable(code, t))
	{
		info.shadowSampler = t.Register("g_ShadowSampler", core::RS_SAMPLER);
		info.cloudSampler = t.Register("g_CloudShadowSampler", core::RS_SAMPLER);
		struct
		{
			const char* name;
			int* reg;
			int minCount;
		} const map[] = {
			{ "g_ShadowedDirect", &info.sun, 2 }, // struct { m_Direction; m_Color; ... }
			{ "g_FogColor", &info.fogColor, 1 },
			{ "g_ShadowNearFar", &info.nearFar, 1 },
			{ "g_ShadowCascadesRangesMin", &info.rangeMin, 1 },
			{ "g_ShadowCascadesRangesMax", &info.rangeMax, 1 },
			{ "g_ShadowScalesX", &info.scaleX, 1 },
			{ "g_ShadowScalesY", &info.scaleY, 1 },
			{ "g_ShadowOffsetsX", &info.offsetX, 1 },
			{ "g_ShadowOffsetsY", &info.offsetY, 1 },
			{ "g_LightDirShadowBiasPerCascade", &info.bias, 1 },
			{ "g_ShadowFadeParams", &info.fade, 1 },
			{ "g_CloudUVScaleOffset", &info.cloudUV, 1 },
		};
		for (const auto& m : map)
			*m.reg = t.Register(m.name, core::RS_FLOAT4, m.minCount);
	}
	EnterCriticalSection(&g_mapLock);
	if (info.Any())
		g_psInfo[ps] = info;
	else
		g_psInfo.erase(ps);
	LeaveCriticalSection(&g_mapLock);
}

// ---- What one frame's opaque draws told us ------------------------------------------------------

struct Shadow
{
	float nearFar[4], rangeMin[4], rangeMax[4], scaleX[4], scaleY[4], offsetX[4], offsetY[4], bias[4], fade[4];
};

struct FrameData
{
	bool cam, light, shadow, sun, fog, cloud;
	core::Camera camera;
	float eyeConst[4];
	bool eyeConstOk;
	float light3x4[3][4];
	Shadow sh;
	IDirect3DBaseTexture9* atlas; // referenced until the frame is done
	float sunDir[4], sunCol[4];
	float fogColor[4];
	float cloudUV[4];
	IDirect3DBaseTexture9* cloudTex;
};
static FrameData g_frame;

static void ReleaseFrameRefs()
{
	if (g_frame.atlas)
		g_frame.atlas->Release();
	if (g_frame.cloudTex)
		g_frame.cloudTex->Release();
	g_frame.atlas = g_frame.cloudTex = nullptr;
}

static void ResetFrame()
{
	ReleaseFrameRefs();
	g_frame.cam = g_frame.light = g_frame.shadow = g_frame.sun = g_frame.fog = g_frame.cloud = false;
	g_frame.eyeConstOk = false;
	g_frame.camera.ok = false;
}

// Cloud shadow parameters come with the shadow ones (same shaders), so they don't hold this up.
static bool FrameComplete() { return g_frame.cam && g_frame.light && g_frame.shadow && g_frame.sun && g_frame.fog; }

static IDirect3DVertexShader9* g_curVs;
static IDirect3DPixelShader9* g_curPs;

static bool GetPs(IDirect3DDevice9* dev, int reg, float* v)
{
	return reg >= 0 && SUCCEEDED(dev->GetPixelShaderConstantF(reg, v, 1));
}

static void CaptureFromDraw(IDirect3DDevice9* dev)
{
	VsInfo vi;
	PsInfo pi;
	bool haveVs = false, havePs = false;
	EnterCriticalSection(&g_mapLock);
	if (g_curVs)
	{
		auto it = g_vsInfo.find(g_curVs);
		if (it != g_vsInfo.end())
			vi = it->second, haveVs = true;
	}
	if (g_curPs)
	{
		auto it = g_psInfo.find(g_curPs);
		if (it != g_psInfo.end())
			pi = it->second, havePs = true;
	}
	LeaveCriticalSection(&g_mapLock);

	if (!g_frame.cam && core::Scene().camera.ok)
	{
		// The scene tracker found this frame's camera (at this draw, the first opaque one with the
		// camera matrices): its g_EyePosition, if it has one, should be the same point.
		g_frame.camera = core::Scene().camera;
		g_frame.cam = true;
		if (haveVs && vi.eye >= 0 && SUCCEEDED(dev->GetVertexShaderConstantF(vi.eye, g_frame.eyeConst, 1)))
			g_frame.eyeConstOk = true;
	}
	if (haveVs && !g_frame.light && vi.light >= 0 && SUCCEEDED(dev->GetVertexShaderConstantF(vi.light, &g_frame.light3x4[0][0], 3)))
		g_frame.light = true;
	if (!havePs)
		return;
	if (!g_frame.sun && pi.sun >= 0)
	{
		float v[8];
		if (SUCCEEDED(dev->GetPixelShaderConstantF(pi.sun, v, 2)))
		{
			float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
			if (len > 0.5f && len < 2.0f)
			{
				for (int k = 0; k < 3; k++)
				{
					g_frame.sunDir[k] = v[k] / len;
					g_frame.sunCol[k] = v[4 + k] > 0 ? v[4 + k] : 0;
				}
				g_frame.sun = true;
			}
		}
	}
	if (!g_frame.fog && GetPs(dev, pi.fogColor, g_frame.fogColor))
		g_frame.fog = true;
	if (!g_frame.shadow && pi.Shadow())
	{
		Shadow& s = g_frame.sh;
		bool ok = GetPs(dev, pi.nearFar, s.nearFar) && GetPs(dev, pi.rangeMin, s.rangeMin) && GetPs(dev, pi.rangeMax, s.rangeMax) &&
			GetPs(dev, pi.scaleX, s.scaleX) && GetPs(dev, pi.scaleY, s.scaleY) && GetPs(dev, pi.offsetX, s.offsetX) &&
			GetPs(dev, pi.offsetY, s.offsetY) && GetPs(dev, pi.fade, s.fade);
		if (!GetPs(dev, pi.bias, s.bias))
			memset(s.bias, 0, sizeof(s.bias));
		IDirect3DBaseTexture9* tex = nullptr;
		if (ok && SUCCEEDED(dev->GetTexture(pi.shadowSampler, &tex)) && tex)
		{
			if (tex->GetType() == D3DRTYPE_TEXTURE)
			{
				g_frame.atlas = tex;
				g_frame.shadow = true;
				static bool logged = false;
				if (!logged)
				{
					logged = true;
					D3DSURFACE_DESC d = {};
					((IDirect3DTexture9*)tex)->GetLevelDesc(0, &d);
					LogInfo("Sun shadow map found: %ux%u format %d, cascades out to %.0f / %.0f / %.0f / %.0f m.", d.Width, d.Height,
						(int)d.Format, s.rangeMax[0], s.rangeMax[1], s.rangeMax[2], s.rangeMax[3]);
				}
			}
			else
				tex->Release();
		}
	}
	if (!g_frame.cloud && pi.cloudUV >= 0 && pi.cloudSampler >= 0 && GetPs(dev, pi.cloudUV, g_frame.cloudUV))
	{
		g_frame.cloud = true; // seen; the texture only matters while the scale is non-zero
		IDirect3DBaseTexture9* tex = nullptr;
		if ((g_frame.cloudUV[0] != 0 || g_frame.cloudUV[1] != 0) && SUCCEEDED(dev->GetTexture(pi.cloudSampler, &tex)) && tex)
		{
			if (tex->GetType() == D3DRTYPE_TEXTURE)
				g_frame.cloudTex = tex;
			else
				tex->Release();
		}
	}
}

// ---- Device resources --------------------------------------------------------------------------

static const core::DeviceFns& D = core::Orig();

static core::RenderTarget g_fog[2], g_shaft[2];
static UINT g_w, g_h, g_lw, g_lh, g_sw, g_sh, g_div;
static core::PassState g_state;
static IDirect3DVertexShader9* g_vs;
static IDirect3DPixelShader9 *g_psMarch, *g_psBlur, *g_psMask, *g_psRadial, *g_psComposite;
static IDirect3DVertexDeclaration9* g_decl;
static bool g_failed = false;
static const UINT kShaftsDiv = 4;

template <typename T> static void Rel(T*& p)
{
	if (p)
		p->Release();
	p = nullptr;
}

static void ReleaseTargets()
{
	for (int i = 0; i < 2; i++)
	{
		g_fog[i].Release();
		g_shaft[i].Release();
	}
	g_state.Release();
	g_w = g_h = 0;
}

static void Fail(const char* what, HRESULT hr)
{
	LogWarn("Volumetrics disabled: %s (0x%08lX).", what, hr);
	g_failed = true;
	ReleaseTargets();
}

static bool MakeTarget(IDirect3DDevice9* dev, UINT w, UINT h, core::RenderTarget& t)
{
	HRESULT hr = t.Create(dev, w, h, D3DFMT_A16B16G16R16F);
	if (FAILED(hr))
	{
		Fail("could not create its render targets", hr);
		return false;
	}
	dev->ColorFill(t.surf, NULL, 0); // nothing undefined (NaN) is ever sampled
	return true;
}

static bool EnsureResources(IDirect3DDevice9* dev, UINT w, UINT h)
{
	if (g_failed)
		return false;
	if (!g_vs)
	{
		static bool capsChecked = false;
		if (!capsChecked)
		{
			capsChecked = true;
			D3DCAPS9 caps = {};
			dev->GetDeviceCaps(&caps);
			if (caps.PixelShaderVersion < D3DPS_VERSION(3, 0) || caps.VertexShaderVersion < D3DVS_VERSION(3, 0))
			{
				Fail("the GPU lacks shader model 3.0", 0);
				return false;
			}
		}
		HRESULT hr;
		if (FAILED(hr = D.CreateVertexShader(dev, kVolFullscreenVs, &g_vs)) || FAILED(hr = D.CreatePixelShader(dev, kVolFogMarchPs, &g_psMarch)) ||
			FAILED(hr = D.CreatePixelShader(dev, kVolFogBlurPs, &g_psBlur)) || FAILED(hr = D.CreatePixelShader(dev, kVolShaftsMaskPs, &g_psMask)) ||
			FAILED(hr = D.CreatePixelShader(dev, kVolShaftsBlurPs, &g_psRadial)) ||
			FAILED(hr = D.CreatePixelShader(dev, kVolCompositePs, &g_psComposite)) || !(g_decl = core::FullscreenDecl(dev)))
		{
			Fail("could not create its shaders", hr);
			return false;
		}
	}
	UINT div = (UINT)g_set.resolution;
	if (g_state.Ready() && g_w == w && g_h == h && g_div == div)
		return true;
	ReleaseTargets();
	UINT lw = (w + div - 1) / div, lh = (h + div - 1) / div;
	UINT sw = (w + kShaftsDiv - 1) / kShaftsDiv, sh = (h + kShaftsDiv - 1) / kShaftsDiv;
	for (int i = 0; i < 2; i++)
		if (!MakeTarget(dev, lw, lh, g_fog[i]) || !MakeTarget(dev, sw, sh, g_shaft[i]))
			return false;
	HRESULT hr = g_state.Create(dev);
	if (FAILED(hr))
	{
		Fail("could not create a state block", hr);
		return false;
	}
	g_w = w;
	g_h = h;
	g_div = div;
	g_lw = lw;
	g_lh = lh;
	g_sw = sw;
	g_sh = sh;
	LogInfo("Render targets created for %ux%u (fog at %ux%u, shafts at %ux%u).", w, h, lw, lh, sw, sh);
	return true;
}

void Vol_OnLostDevice()
{
	ReleaseFrameRefs();
	ReleaseTargets();
}

// ---- Frame tracking ----------------------------------------------------------------------------
//
// Idle -> InPass when the game binds an R32F z/w target as RT1 next to its scene colour target;
// InPass -> Between when RT1 is unbound (also between the opaque batches); Between -> InPass again
// for the next batch. After the pass: the sky draws (viewport depth 1..1) come first; our passes
// run before the first other draw into the scene after the sky (Placement=0), or when the game
// moves on from the scene colour target (Placement=1, and the fallback when no sky was drawn).

enum Phase
{
	IDLE,
	IN_PASS,
	BETWEEN,
	DONE
};
static Phase g_phase = IDLE;
static bool g_skySeen = false;
static int g_passDraws = 0;
static IDirect3DSurface9* g_scene;    // the game's scene colour target during the pass
static IDirect3DSurface9* g_depthRt;  // the game's z/w target during the pass
static const char* g_status = "waiting for the game's 3D scene";
static int g_framesApplied = 0;

const char* Vol_Status() { return g_status; }

static bool IsZwTarget(IDirect3DSurface9* s, IDirect3DSurface9* rt0)
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
			LogWarn("The scene is multisampled (in-game anti-aliasing); volumetrics are not supported with it.");
		}
		g_status = "off: in-game anti-aliasing (MSAA) is on";
		return false;
	}
	return b.zwTexture;
}

static void Apply(IDirect3DDevice9* dev);

static bool Wanted() { return g_set.fogOn || g_set.shaftsOn || g_set.view; }

void Vol_BeforeSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s)
{
	if (index != 0 || s == g_scene)
		return;
	if (g_phase == BETWEEN)
		Apply(dev); // the game is done with the scene colour target
	else if (g_phase == IN_PASS)
		g_phase = IDLE; // the pass ended without unbinding its z/w target: leave the frame alone
}

void Vol_AfterSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s)
{
	if (index != 1)
		return;
	IDirect3DSurface9* rt0 = core::Scene().rt0;
	if (s)
	{
		if (g_phase == DONE)
		{
			if (s == g_depthRt)
			{
				static bool warned = false;
				if (!warned)
				{
					warned = true;
					LogWarn("The game went on drawing its opaque pass after the volumetrics were applied (logged once).");
				}
			}
			return;
		}
		if (!Wanted() || !IsZwTarget(s, rt0))
			return;
		if (g_phase == IDLE)
		{
			g_skySeen = false;
			g_passDraws = 0;
		}
		g_phase = IN_PASS;
		g_scene = rt0;
		g_depthRt = s;
	}
	else if (g_phase == IN_PASS)
		g_phase = BETWEEN;
}

void Vol_AfterSetVertexShader(IDirect3DVertexShader9* vs) { g_curVs = vs; }
void Vol_AfterSetPixelShader(IDirect3DPixelShader9* ps) { g_curPs = ps; }

void Vol_BeforeDraw(IDirect3DDevice9* dev)
{
	if (g_phase == IN_PASS)
	{
		g_passDraws++;
		if (!FrameComplete())
			CaptureFromDraw(dev);
		return;
	}
	if (g_phase != BETWEEN || core::Scene().rt0 != g_scene)
		return;
	if (core::IsSkyViewport(core::Scene().viewport))
		g_skySeen = true; // the sky (or the sun) behind everything
	else if (g_skySeen && g_set.placement == 0)
		Apply(dev);
}

void Vol_BeforeStretchRect(IDirect3DDevice9* dev, IDirect3DSurface9* src)
{
	if (g_phase == BETWEEN && src && src == g_scene)
		Apply(dev);
}

void Vol_BeforePresent(IDirect3DDevice9* dev)
{
	(void)dev;
	if (g_phase == IN_PASS || g_phase == BETWEEN)
	{
		static bool warned = false;
		if (!warned && g_phase == BETWEEN)
		{
			warned = true;
			LogWarn("A frame ended without a place to apply the volumetrics (logged once).");
		}
	}
	if (g_phase == IDLE && g_framesApplied == 0)
		g_status = Wanted() ? "waiting for the game's 3D scene" : "all effects off";
	g_phase = IDLE;
	g_scene = g_depthRt = nullptr;
	ResetFrame();
}

void Vol_OnDeviceReady(IDirect3DDevice9* dev)
{
	(void)dev;
	g_phase = IDLE;
	g_scene = g_depthRt = nullptr;
	g_curVs = nullptr;
	g_curPs = nullptr;
	ResetFrame();
}

// ---- The passes --------------------------------------------------------------------------------

static void Sampler(IDirect3DDevice9* dev, DWORD s, D3DTEXTUREFILTERTYPE filter, D3DTEXTUREADDRESS address)
{
	dev->SetSamplerState(s, D3DSAMP_MINFILTER, filter);
	dev->SetSamplerState(s, D3DSAMP_MAGFILTER, filter);
	dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	dev->SetSamplerState(s, D3DSAMP_ADDRESSU, address);
	dev->SetSamplerState(s, D3DSAMP_ADDRESSV, address);
	dev->SetSamplerState(s, D3DSAMP_BORDERCOLOR, 0);
	dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
	dev->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, 0);
	dev->SetSamplerState(s, D3DSAMP_MAXANISOTROPY, 1);
}

static void Apply(IDirect3DDevice9* dev)
{
	g_phase = DONE;
	if (!Wanted() || g_failed)
		return;
	const FrameData& F = g_frame;
	if (!F.cam || !F.sun)
	{
		static bool warned = false;
		if (!warned)
		{
			warned = true;
			LogWarn("Skipped a frame: %s not found in the game's shader constants (logged once).", !F.cam ? "camera" : "sun");
		}
		g_status = !F.cam ? "skipped: no camera found" : "skipped: no sun found";
		return;
	}
	if (!g_scene || !g_depthRt)
		return;
	D3DSURFACE_DESC sd;
	if (FAILED(g_scene->GetDesc(&sd)) || !EnsureResources(dev, sd.Width, sd.Height))
		return;
	IDirect3DTexture9* depthTex = nullptr;
	if (FAILED(g_depthRt->GetContainer(core::kIID_Texture9, (void**)&depthTex)) || !depthTex)
		return;
	IDirect3DTexture9* sceneTex = nullptr; // for the shafts' sky brightness; optional
	if (FAILED(g_scene->GetContainer(core::kIID_Texture9, (void**)&sceneTex)))
		sceneTex = nullptr;

	const core::CameraFloats& C = F.camera.f;
	const bool fog = g_set.fogOn || g_set.view == 1 || g_set.view == 2;
	const bool shadowed = F.shadow && F.light;

	// Sun on screen, for the shafts: fade them out as it leaves the screen or goes behind the camera.
	float sunView[3];
	const float* axes[3] = { C.right, C.up, C.forward };
	for (int k = 0; k < 3; k++)
		sunView[k] = axes[k][0] * F.sunDir[0] + axes[k][1] * F.sunDir[1] + axes[k][2] * F.sunDir[2];
	float sunUv[2] = { 0.5f, 0.5f }, shaftFade = 0.0f;
	if (sunView[2] > 0.02f)
	{
		float ndcX = C.P00 * sunView[0] / sunView[2] + C.P02, ndcY = C.P11 * sunView[1] / sunView[2] + C.P12;
		sunUv[0] = ndcX * 0.5f + 0.5f + 0.5f / sd.Width;
		sunUv[1] = 0.5f - ndcY * 0.5f + 0.5f / sd.Height;
		float edge = fmaxf(fabsf(ndcX), fabsf(ndcY));
		shaftFade = Clampf((sunView[2] - 0.02f) * 5.0f, 0.0f, 1.0f) * Clampf((3.0f - edge) * 0.5f, 0.0f, 1.0f);
	}
	const bool shafts = (g_set.shaftsOn || g_set.view == 3) && shaftFade > 0.0f && sceneTex;

	g_state.Save(dev);
	g_state.BeginPasses(dev, g_decl, g_vs);
	dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
	Sampler(dev, 0, D3DTEXF_POINT, D3DTADDRESS_CLAMP);  // depth
	Sampler(dev, 1, D3DTEXF_POINT, D3DTADDRESS_CLAMP);  // shadow atlas
	Sampler(dev, 2, D3DTEXF_LINEAR, D3DTADDRESS_WRAP);  // cloud shadows
	Sampler(dev, 3, D3DTEXF_POINT, D3DTADDRESS_CLAMP);  // fog
	Sampler(dev, 4, D3DTEXF_LINEAR, D3DTADDRESS_CLAMP); // scene colour
	Sampler(dev, 5, D3DTEXF_LINEAR, D3DTADDRESS_BORDER); // shafts
	for (DWORD i = 0; i < 8; i++)
		dev->SetTexture(i, nullptr);

	// Constants (see the register list at the top of volumetrics.hlsl).
	const double far_ = C.farZ;
	const float skyD = (float)(C.A + C.B / (0.99 * far_));
	const Shadow& S = F.sh;
	const bool clouds = F.cloudTex && (F.cloudUV[0] != 0 || F.cloudUV[1] != 0);
	const float sunI = g_set.sunIntensity, ambI = g_set.ambientIntensity;
	float c[30][4] = {};
	auto set4 = [&](int r, float x, float y, float z, float w) {
		c[r][0] = x;
		c[r][1] = y;
		c[r][2] = z;
		c[r][3] = w;
	};
	auto copy4 = [&](int r, const float* v) { memcpy(c[r], v, 16); };
	set4(0, (float)g_w, (float)g_h, 1.0f / g_w, 1.0f / g_h);
	set4(1, (float)g_lw, (float)g_lh, 1.0f / g_lw, 1.0f / g_lh);
	set4(2, C.A, C.B, C.P00, C.P11);
	set4(3, C.P02, C.P12, (float)g_div, skyD);
	set4(4, C.eye[0], C.eye[1], C.eye[2], g_set.maxDistance);
	set4(5, C.right[0], C.right[1], C.right[2], (float)g_set.steps);
	set4(6, C.up[0], C.up[1], C.up[2], (float)g_set.view);
	set4(7, C.forward[0], C.forward[1], C.forward[2], (float)kShaftsDiv);
	if (shadowed)
	{
		for (int r = 0; r < 3; r++)
			copy4(8 + r, F.light3x4[r]);
		set4(11, S.nearFar[0], S.nearFar[1], 1, 0);
		copy4(12, S.rangeMin);
		copy4(13, S.rangeMax);
		copy4(14, S.scaleX);
		copy4(15, S.scaleY);
		copy4(16, S.offsetX);
		copy4(17, S.offsetY);
		copy4(18, S.bias);
		set4(19, S.fade[0], S.fade[1], clouds ? 1.0f : 0.0f, 0);
	}
	if (clouds)
		copy4(20, F.cloudUV);
	set4(21, F.sunDir[0], F.sunDir[1], F.sunDir[2], g_set.anisotropy);
	set4(22, F.sunCol[0] * sunI, F.sunCol[1] * sunI, F.sunCol[2] * sunI, 0);
	if (F.fog)
		set4(23, F.fogColor[0] * ambI, F.fogColor[1] * ambI, F.fogColor[2] * ambI, 0);
	set4(24, fog ? fmaxf(g_set.density, 1e-7f) : 0.0f, g_set.heightFalloff, g_set.baseHeight, 0); // w: noise frame offset (0 until TAA-aware)
	set4(26, sunUv[0], sunUv[1], 0, 0);
	set4(27, g_set.shaftsThreshold, g_set.shaftsFalloff, 0, 0);
	const float shaftI = g_set.shaftsIntensity * shaftFade;
	if (shafts)
		set4(28, F.sunCol[0] * shaftI, F.sunCol[1] * shaftI, F.sunCol[2] * shaftI, 1);
	set4(29, (float)g_sw, (float)g_sh, 1.0f / g_sw, 1.0f / g_sh);
	dev->SetPixelShaderConstantF(0, &c[0][0], 30);

	dev->SetTexture(0, depthTex);
	if (fog)
	{
		// Ray march -> fog[0]; blur across -> fog[1]; blur down -> fog[0].
		dev->SetTexture(1, shadowed ? F.atlas : nullptr);
		dev->SetTexture(2, clouds ? F.cloudTex : nullptr);
		core::SetTarget(dev, g_fog[0].surf, g_lw, g_lh);
		D.SetPixelShader(dev, g_psMarch);
		core::DrawFullscreen(dev);
		dev->SetTexture(1, nullptr);
		dev->SetTexture(2, nullptr);
		D.SetPixelShader(dev, g_psBlur);
		const float across[4] = { 1, 0, 0, 0 }, down[4] = { 0, 1, 0, 0 };
		dev->SetPixelShaderConstantF(25, across, 1);
		core::SetTarget(dev, g_fog[1].surf, g_lw, g_lh);
		dev->SetTexture(3, g_fog[0].tex);
		core::DrawFullscreen(dev);
		dev->SetPixelShaderConstantF(25, down, 1);
		core::SetTarget(dev, g_fog[0].surf, g_lw, g_lh);
		dev->SetTexture(3, g_fog[1].tex);
		core::DrawFullscreen(dev);
	}
	if (shafts)
	{
		// Sky mask -> shafts[0]; radial blur, coarse steps -> shafts[1]; fine steps -> shafts[0].
		dev->SetTexture(4, sceneTex);
		core::SetTarget(dev, g_shaft[0].surf, g_sw, g_sh);
		D.SetPixelShader(dev, g_psMask);
		core::DrawFullscreen(dev);
		dev->SetTexture(4, nullptr);
		D.SetPixelShader(dev, g_psRadial);
		const float coarse[4] = { sunUv[0], sunUv[1], g_set.shaftsLength / 16.0f, g_set.shaftsDecay };
		const float fine[4] = { sunUv[0], sunUv[1], g_set.shaftsLength / 256.0f, 1.0f };
		dev->SetPixelShaderConstantF(26, coarse, 1);
		core::SetTarget(dev, g_shaft[1].surf, g_sw, g_sh);
		dev->SetTexture(5, g_shaft[0].tex);
		core::DrawFullscreen(dev);
		dev->SetPixelShaderConstantF(26, fine, 1);
		core::SetTarget(dev, g_shaft[0].surf, g_sw, g_sh);
		dev->SetTexture(5, g_shaft[1].tex);
		core::DrawFullscreen(dev);
	}

	// Composite: scene = scene * transmittance + scattered light + shafts (colour channels only).
	core::SetTarget(dev, g_scene, g_w, g_h);
	dev->SetTexture(3, g_fog[0].tex);
	dev->SetTexture(5, g_shaft[0].tex);
	dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
	dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCALPHA);
	dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
	dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
	D.SetPixelShader(dev, g_psComposite);
	core::DrawFullscreen(dev);

	g_state.Restore(dev);
	depthTex->Release();
	Rel(sceneTex);
	ReleaseFrameRefs();

	g_status = shadowed ? "running" : "running (no shadow map found: fog is not shadowed)";
	if (g_framesApplied++ == 0)
	{
		LogInfo("First frame applied: camera at (%.1f, %.1f, %.1f), near %.3f m, far %.0f m, horizontal fov %.1f deg%s.", C.eye[0], C.eye[1],
			C.eye[2], C.nearZ, C.farZ, 2.0 * atan(1.0 / C.P00) * 57.29578,
			F.eyeConstOk ? "" : " (no g_EyePosition to compare)");
		if (F.eyeConstOk)
			LogInfo("  g_EyePosition (%.1f, %.1f, %.1f) - should match the camera position.", F.eyeConst[0], F.eyeConst[1], F.eyeConst[2]);
		LogInfo("  sun direction (%.3f, %.3f, %.3f), colour (%.2f, %.2f, %.2f); fog colour %s (%.2f, %.2f, %.2f); shadow map %s; cloud "
				"shadows %s; %d opaque draws.",
			F.sunDir[0], F.sunDir[1], F.sunDir[2], F.sunCol[0], F.sunCol[1], F.sunCol[2], F.fog ? "" : "NOT FOUND", F.fog ? F.fogColor[0] : 0,
			F.fog ? F.fogColor[1] : 0, F.fog ? F.fogColor[2] : 0, shadowed ? "used" : F.shadow ? "found but no light matrix" : "NOT FOUND",
			clouds ? "used" : "off", g_passDraws);
		LogInfo("  applied %s.", g_skySeen && g_set.placement == 0 ? "after the sky, before transparents" : "when the game left the scene target");
	}
}

} // namespace vol
