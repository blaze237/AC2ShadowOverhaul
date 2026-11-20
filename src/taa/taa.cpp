// See taa.h for the overview, vsjitter.cpp for the vertex shader side, shaders/taa.hlsl for the passes.
#include "taa.h"
#include "vsjitter.h"
#include "common.h"
#include "shaders_gen.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unordered_map>
#include <map>

namespace taa
{

// ---- Settings ----------------------------------------------------------------------------------

static bool g_enabled = false;
static volatile bool g_on = true;
static float g_feedback = 0.95f;      // history weight when still
static float g_motionFeedback = 0.8f; // history weight in fast motion
static float g_motionPixels = 16.0f;  // motion (pixels per frame) counted as fast
static float g_depthTolerance = 0.05f;
static float g_sharpness = 0.25f;
static float g_clipGamma = 1.25f;
static float g_mipBias = -0.5f;
static float g_jitterScale = 1.0f;
static int g_debugView = 0;
// Strength k of the reversible c / (1 + k luma) the history is kept in (taa.hlsl Compress): damps
// fireflies while keeping the 0..1 range nearly linear (k = 1 darkened edges in tests).
static const float kHdrCompression = 0.25f;
// A history further than this from the frame's jitter-free estimate (in units of the local
// contrast) is trusted less: it was uncovered from behind something at the same depth (taa.hlsl).
static const float kDistrust = 0.6f;

bool Taa_Enabled() { return g_enabled; }
bool Taa_Active() { return g_enabled && g_on; }

static float Clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

TaaSettings Taa_GetSettings()
{
	TaaSettings t;
	t.on = g_on;
	t.feedback = g_feedback;
	t.motionFeedback = g_motionFeedback;
	t.sharpness = g_sharpness;
	t.mipBias = g_mipBias;
	t.jitterScale = g_jitterScale;
	t.clipBox = g_clipGamma;
	t.depthTolerance = g_depthTolerance;
	t.debugView = g_debugView;
	return t;
}

// Called on the render thread (the menu runs at Present). The mip bias goes on or off at the next
// render target change, which comes many times before the next scene draw.
void Taa_SetSettings(const TaaSettings& t)
{
	g_on = t.on;
	g_feedback = Clamp(t.feedback, 0.0f, 0.98f);
	g_motionFeedback = Clamp(t.motionFeedback, 0.0f, 0.98f);
	g_sharpness = Clamp(t.sharpness, 0.0f, 2.0f);
	g_mipBias = Clamp(t.mipBias, -2.0f, 1.0f);
	g_jitterScale = Clamp(t.jitterScale, 0.0f, 2.0f);
	g_clipGamma = Clamp(t.clipBox, 0.5f, 4.0f);
	g_depthTolerance = Clamp(t.depthTolerance, 0.001f, 1.0f);
	g_debugView = t.debugView < 0 ? 0 : t.debugView > 2 ? 2 : t.debugView;
}

bool Taa_Init()
{
	const char* S = "TAA";
	g_enabled = IniInt(S, "Enabled", 1) != 0;
	if (!g_enabled)
	{
		LogInfo("TAA: off ([TAA] Enabled=0).");
		return false;
	}
	g_on = IniInt(S, "StartEnabled", 1) != 0;
	g_feedback = Clamp(IniFloat(S, "Feedback", 0.95f), 0.0f, 0.98f);
	g_motionFeedback = Clamp(IniFloat(S, "MotionFeedback", 0.8f), 0.0f, 0.98f);
	g_sharpness = Clamp(IniFloat(S, "Sharpness", 0.25f), 0.0f, 2.0f);
	g_mipBias = Clamp(IniFloat(S, "MipBias", -0.5f), -2.0f, 1.0f);
	g_jitterScale = Clamp(IniFloat(S, "JitterScale", 1.0f), 0.0f, 2.0f);
	g_clipGamma = Clamp(IniFloat(S, "ClipBox", 1.25f), 0.5f, 4.0f);
	g_depthTolerance = Clamp(IniFloat(S, "DepthTolerance", 0.05f), 0.001f, 1.0f);
	g_debugView = IniInt(S, "DebugView", 0);
	LogInfo("TAA: feedback %.2f (moving %.2f), sharpness %.2f, mip bias %.2f, jitter %.2f, clip box %.2f, depth tolerance %.3f%s%s.",
		(double)g_feedback, (double)g_motionFeedback, (double)g_sharpness, (double)g_mipBias, (double)g_jitterScale, (double)g_clipGamma,
		(double)g_depthTolerance, g_on ? "" : " (starting off)", g_debugView ? ", debug view on" : "");
	return true;
}

// ---- Vertex shaders ----------------------------------------------------------------------------

struct VsMatrices
{
	int wvp = -1, world = -1;
};
static CRITICAL_SECTION g_vsLock;
static bool g_vsLockReady = false;
static std::unordered_map<void*, VsMatrices> g_vsInfo;
static std::map<std::string, int> g_skipReasons;
static int g_vsRewritten = 0, g_vsSkipped = 0;

static void InitLock()
{
	if (!g_vsLockReady)
	{
		InitializeCriticalSection(&g_vsLock);
		g_vsLockReady = true;
	}
}

bool Taa_RewriteVS(const DWORD* code, std::vector<DWORD>& out, VsJitterInfo& info)
{
	InitLock();
	std::string reason;
	bool ok = VsJitter_Rewrite(code, out, info, reason);
	EnterCriticalSection(&g_vsLock);
	if (ok)
		g_vsRewritten++;
	else
	{
		g_vsSkipped++;
		int& n = g_skipReasons[reason];
		if (n++ == 0)
			LogInfo("TAA: vertex shader left unjittered: %s (further ones with this reason are only counted).", reason.c_str());
	}
	int total = g_vsRewritten + g_vsSkipped;
	if (total == 1 || total % 200 == 0)
		LogInfo("TAA: %d vertex shaders jitterable, %d left alone.", g_vsRewritten, g_vsSkipped);
	LeaveCriticalSection(&g_vsLock);
	return ok && g_enabled;
}

void Taa_OnVertexShaderCreated(IDirect3DVertexShader9* vs, const VsJitterInfo& info)
{
	if (!vs)
		return;
	InitLock();
	EnterCriticalSection(&g_vsLock);
	if (info.wvpReg >= 0 && info.worldReg >= 0)
	{
		VsMatrices m;
		m.wvp = info.wvpReg;
		m.world = info.worldReg;
		g_vsInfo[vs] = m;
	}
	else
		g_vsInfo.erase(vs); // the address may have belonged to a released shader
	LeaveCriticalSection(&g_vsLock);
}

static IDirect3DVertexShader9* g_curVs;
void Taa_OnSetVertexShader(IDirect3DVertexShader9* vs) { g_curVs = vs; }

// ---- Camera ------------------------------------------------------------------------------------

typedef TaaCamera Camera;
static Camera g_cam, g_prevCam;

static bool Invert4(const double m[4][4], double out[4][4])
{
	double a[4][8];
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 8; c++)
			a[r][c] = c < 4 ? m[r][c] : (c - 4 == r ? 1.0 : 0.0);
	for (int c = 0; c < 4; c++)
	{
		int piv = c;
		for (int r = c + 1; r < 4; r++)
			if (fabs(a[r][c]) > fabs(a[piv][c]))
				piv = r;
		if (fabs(a[piv][c]) < 1e-12)
			return false;
		for (int k = 0; k < 8; k++)
		{
			double t = a[c][k];
			a[c][k] = a[piv][k];
			a[piv][k] = t;
		}
		double inv = 1.0 / a[c][c];
		for (int k = 0; k < 8; k++)
			a[c][k] *= inv;
		for (int r = 0; r < 4; r++)
			if (r != c)
			{
				double f = a[r][c];
				for (int k = 0; k < 8; k++)
					a[r][k] -= f * a[c][k];
			}
	}
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++)
			out[r][c] = a[r][c + 4];
	return true;
}

static void Mul4(const double a[4][4], const double b[4][4], double out[4][4])
{
	double t[4][4];
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++)
			t[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c] + a[r][3] * b[3][c];
	memcpy(out, t, sizeof(t));
}

// Registers hold matrix rows (clip.x = dot(position, c[wvp])). ViewProj = WVP * World^-1, which for
// a D3D perspective projection is P * V with P = [P00 0 P02 0; 0 P11 P12 0; 0 0 A B; 0 0 1 0].
bool Taa_CameraFromMatrices(const float wvp[16], const float world[16], Camera& cam, char* why, size_t whySize)
{
	double M[4][4], Wm[4][4], Wi[4][4];
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++)
		{
			M[r][c] = wvp[r * 4 + c];
			Wm[r][c] = world[r * 4 + c];
		}
	if (!Invert4(Wm, Wi))
	{
		snprintf(why, whySize, "world matrix not invertible");
		return false;
	}
	double(*VP)[4] = cam.VP;
	Mul4(M, Wi, VP);
	auto dot3 = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
	double s = sqrt(dot3(VP[3], VP[3]));
	if (fabs(s - 1.0) > 0.01)
	{
		snprintf(why, whySize, "not a standard perspective projection (|w row| = %g)", s);
		return false;
	}
	double f[3] = { VP[3][0] / s, VP[3][1] / s, VP[3][2] / s };
	double A = dot3(VP[2], f) / s, B = VP[2][3] - A * VP[3][3];
	double p02 = dot3(VP[0], f) / s, p12 = dot3(VP[1], f) / s;
	double r0[3], r1[3], res[3];
	for (int k = 0; k < 3; k++)
	{
		r0[k] = VP[0][k] - p02 * VP[3][k];
		r1[k] = VP[1][k] - p12 * VP[3][k];
		res[k] = VP[2][k] - A * VP[3][k];
	}
	double p00 = sqrt(dot3(r0, r0)), p11 = sqrt(dot3(r1, r1));
	double near_ = A != 0 ? -B / A : 0;
	if (sqrt(dot3(res, res)) > 1e-3 * fabs(A) || !(A > 0.5) || !(B < 0) || !(near_ > 0) || p00 < 1e-3 || p11 < 1e-3 ||
		fabs(dot3(r0, r1)) > 1e-3 * p00 * p11)
	{
		snprintf(why, whySize, "unexpected projection (A %g, B %g, P00 %g, P11 %g)", A, B, p00, p11);
		return false;
	}
	cam.A = A;
	cam.B = B;
	cam.P00 = p00;
	cam.P11 = p11;
	cam.P02 = p02;
	cam.P12 = p12;
	cam.ok = true;
	return true;
}

// Rows of: this frame's view space (x right, y up, z forward, metres) -> previous frame's clip
// space = VP_prev * VP_cur^-1 * P_cur (P_cur takes view space to this frame's clip space).
bool Taa_ReprojectionMatrix(const Camera& cur, const Camera& prev, float out[4][4])
{
	double inv[4][4], P[4][4] = {}, M[4][4];
	if (!Invert4(cur.VP, inv))
		return false;
	P[0][0] = cur.P00;
	P[0][2] = cur.P02;
	P[1][1] = cur.P11;
	P[1][2] = cur.P12;
	P[2][2] = cur.A;
	P[2][3] = cur.B;
	P[3][2] = 1.0;
	Mul4(inv, P, M);
	Mul4(prev.VP, M, M);
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++)
			out[r][c] = (float)M[r][c];
	return true;
}

// ---- Device state ------------------------------------------------------------------------------

static TaaDeviceFns D;
static const GUID kIID_Texture9 = { 0x85c31227, 0x3de5, 0x4f00, { 0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5 } };
void Taa_SetDeviceFns(const TaaDeviceFns& fns) { D = fns; }

static bool g_failed = false; // TAA can't run on this device (logged once)
static IDirect3DTexture9* g_histTex[2];
static IDirect3DSurface9* g_histSurf[2];
static int g_histIndex = 0; // the one written next
static UINT g_histW, g_histH;
static IDirect3DStateBlock9* g_state;
static IDirect3DVertexShader9* g_vsFull;
static IDirect3DPixelShader9 *g_psResolve, *g_psOutput;
static IDirect3DVertexDeclaration9* g_decl;

// Per frame.
static IDirect3DSurface9* g_rt0;       // current render targets 0 and 1 (not referenced)
static IDirect3DSurface9* g_rt1;
static bool g_inScene = false;         // the main pass was seen this frame
static bool g_resolved = false;        // the resolve ran (or was attempted) this frame
static bool g_resolvedOk = false;      // ... and produced a new history
static IDirect3DSurface9* g_sceneSurf; // the game's scene colour / z/w targets this frame
static IDirect3DTexture9* g_sceneTex;
static IDirect3DSurface9* g_depthSurf;
static UINT g_sceneW, g_sceneH;        // size of the last scene seen (render targets of this size get jitter)
static IDirect3DBaseTexture9* g_bound[21]; // textures bound per sampler (0-15, then the 5 vertex samplers)
static DWORD g_sceneBound = 0;         // samplers where the scene colour is bound

// Across frames.
static bool g_historyValid = false;    // the last frame produced a history this frame can use
static bool g_jitterFrame = false;     // this frame is rendered with jitter
static float g_jitterPx[2];            // this frame's jitter in pixels (scene moved by this, y down)
static unsigned g_frame = 0;
static float g_c255[4];                // what we last put into c255
static bool g_c255Known = false;
static bool g_jitterOn = false;        // c255 currently holds the jitter (and the mip bias is applied)
static DWORD g_gameBias[16];           // the game's own D3DSAMP_MIPMAPLODBIAS per sampler

static void ReleaseResources()
{
	auto rel = [](auto*& p) {
		if (p)
			p->Release();
		p = nullptr;
	};
	for (int i = 0; i < 2; i++)
	{
		rel(g_histSurf[i]);
		rel(g_histTex[i]);
	}
	rel(g_state);
	g_histW = g_histH = 0;
	g_historyValid = false;
}

static void Fail(const char* what, HRESULT hr)
{
	LogWarn("TAA disabled: %s (0x%08lX).", what, hr);
	g_failed = true;
	ReleaseResources();
}

void Taa_OnDeviceCreated(IDirect3D9* d3d, UINT adapter, IDirect3DDevice9* dev)
{
	D3DCAPS9 caps = {};
	dev->GetDeviceCaps(&caps);
	D3DDISPLAYMODE mode = {};
	d3d->GetAdapterDisplayMode(adapter, &mode);
	bool filter = SUCCEEDED(d3d->CheckDeviceFormat(adapter, caps.DeviceType, mode.Format != D3DFMT_UNKNOWN ? mode.Format : D3DFMT_X8R8G8B8,
		D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE, D3DFMT_A16B16G16R16F));
	LogInfo("TAA: GPU has vertex shader %lu.%lu, pixel shader %lu.%lu (%lu instruction slots), float16 filtering %s.",
		(caps.VertexShaderVersion >> 8) & 0xFF, caps.VertexShaderVersion & 0xFF, (caps.PixelShaderVersion >> 8) & 0xFF,
		caps.PixelShaderVersion & 0xFF, caps.MaxPixelShader30InstructionSlots, filter ? "yes" : "NO");
	if (caps.PixelShaderVersion < D3DPS_VERSION(3, 0) || caps.VertexShaderVersion < D3DVS_VERSION(3, 0))
		Fail("the GPU lacks shader model 3", 0);
	else if (caps.MaxPixelShader30InstructionSlots < 1024)
		Fail("the GPU's pixel shaders are too short for the resolve (it needs about 640 instruction slots)", 0);
	else if (!filter)
		LogWarn("TAA: the GPU can't filter float16 textures; the history will look blocky in motion.");
}

static bool EnsureResources(IDirect3DDevice9* dev, UINT w, UINT h)
{
	if (g_failed)
		return false;
	HRESULT hr;
	if (!g_vsFull)
	{
		const D3DVERTEXELEMENT9 decl[] = { { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 }, D3DDECL_END() };
		// Our own full-screen VS goes to the original CreateVertexShader (never jittered).
		if (FAILED(hr = D.CreateVertexShader(dev, kTaaFullscreenVs, &g_vsFull)) || FAILED(hr = dev->CreatePixelShader(kTaaResolvePs, &g_psResolve)) ||
			FAILED(hr = dev->CreatePixelShader(kTaaOutputPs, &g_psOutput)) || FAILED(hr = dev->CreateVertexDeclaration(decl, &g_decl)))
		{
			Fail("could not create its shaders", hr);
			return false;
		}
	}
	if (g_histTex[0] && g_histW == w && g_histH == h)
		return true;
	ReleaseResources();
	for (int i = 0; i < 2; i++)
		if (FAILED(hr = D.CreateTexture(dev, w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &g_histTex[i], NULL)) ||
			FAILED(hr = g_histTex[i]->GetSurfaceLevel(0, &g_histSurf[i])))
		{
			Fail("could not create its history targets", hr);
			return false;
		}
	if (FAILED(hr = dev->CreateStateBlock(D3DSBT_ALL, &g_state)))
	{
		Fail("could not create a state block", hr);
		return false;
	}
	g_histW = w;
	g_histH = h;
	g_historyValid = false;
	LogInfo("TAA: history targets created for %ux%u.", w, h);
	return true;
}

// ---- Jitter ------------------------------------------------------------------------------------

static float Halton(unsigned i, unsigned base)
{
	float f = 1.0f, r = 0.0f;
	for (; i; i /= base)
	{
		f /= (float)base;
		r += f * (float)(i % base);
	}
	return r;
}

static void SetC255(IDirect3DDevice9* dev, const float v[4])
{
	if (g_c255Known && memcmp(v, g_c255, sizeof(g_c255)) == 0)
		return;
	memcpy(g_c255, v, sizeof(g_c255));
	g_c255Known = true;
	D.SetVertexShaderConstantF(dev, kJitterReg, v, 1);
}

static void ApplyMipBias(IDirect3DDevice9* dev, bool on)
{
	for (DWORD s = 0; s < 16; s++)
	{
		float b;
		memcpy(&b, &g_gameBias[s], 4);
		b += on ? g_mipBias : 0.0f;
		DWORD v;
		memcpy(&v, &b, 4);
		D.SetSamplerState(dev, s, D3DSAMP_MIPMAPLODBIAS, v);
	}
}

// Jitter goes on scene-sized render target textures until the resolve (the depth prepass and main
// pass targets; not the back buffer, shadow maps or half-res targets).
static bool IsJitterTarget(IDirect3DSurface9* s)
{
	if (!s || !g_sceneW)
		return false;
	D3DSURFACE_DESC d = {};
	if (FAILED(s->GetDesc(&d)) || d.Width != g_sceneW || d.Height != g_sceneH)
		return false;
	IDirect3DTexture9* tex = nullptr;
	if (FAILED(s->GetContainer(kIID_Texture9, (void**)&tex)) || !tex)
		return false;
	tex->Release();
	return true;
}

static void UpdateJitter(IDirect3DDevice9* dev)
{
	bool on = g_jitterFrame && !g_resolved && Taa_Active() && IsJitterTarget(g_rt0);
	float v[4] = { 0, 0, 0, 0 };
	if (on)
	{
		v[0] = 2.0f * g_jitterPx[0] / (float)g_sceneW;
		v[1] = -2.0f * g_jitterPx[1] / (float)g_sceneH;
	}
	SetC255(dev, v);
	if (on != g_jitterOn)
	{
		g_jitterOn = on;
		ApplyMipBias(dev, on);
	}
}

DWORD Taa_SamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value)
{
	if (type != D3DSAMP_MIPMAPLODBIAS || sampler >= 16)
		return value;
	g_gameBias[sampler] = value;
	if (!g_jitterOn || g_mipBias == 0.0f)
		return value;
	float b;
	memcpy(&b, &value, 4);
	b += g_mipBias;
	memcpy(&value, &b, 4);
	return value;
}

void Taa_AfterVertexConstants(IDirect3DDevice9* dev, UINT start, UINT count)
{
	if (g_c255Known && start <= (UINT)kJitterReg && start + count > (UINT)kJitterReg)
	{
		g_c255Known = false; // the game wrote over it: put ours back
		float v[4];
		memcpy(v, g_c255, sizeof(v));
		SetC255(dev, v);
	}
}

// ---- Frame tracking ----------------------------------------------------------------------------

static int SamplerSlot(DWORD stage)
{
	if (stage < 16)
		return (int)stage;
	if (stage >= D3DDMAPSAMPLER && stage <= D3DVERTEXTEXTURESAMPLER3)
		return 16 + (int)(stage - D3DDMAPSAMPLER);
	return -1;
}

void Taa_OnSetTexture(DWORD stage, IDirect3DBaseTexture9* tex)
{
	int slot = SamplerSlot(stage);
	if (slot < 0)
		return;
	g_bound[slot] = tex;
	if (g_sceneTex && tex == static_cast<IDirect3DBaseTexture9*>(g_sceneTex))
		g_sceneBound |= 1u << slot;
	else
		g_sceneBound &= ~(1u << slot);
}

// The game's z/w target bound as render target 1 next to a scene colour target of the same size.
static bool IsMainPass(IDirect3DSurface9* s, IDirect3DSurface9* rt0, UINT& w, UINT& h, IDirect3DTexture9*& sceneTex)
{
	if (!s || !rt0)
		return false;
	D3DSURFACE_DESC d = {}, c = {};
	if (FAILED(s->GetDesc(&d)) || FAILED(rt0->GetDesc(&c)))
		return false;
	if (d.Format != D3DFMT_R32F || !(d.Usage & D3DUSAGE_RENDERTARGET) || c.Width != d.Width || c.Height != d.Height)
		return false;
	IDirect3DTexture9* tex = nullptr;
	if (FAILED(s->GetContainer(kIID_Texture9, (void**)&tex)) || !tex)
		return false;
	tex->Release();
	tex = nullptr;
	if (c.MultiSampleType != D3DMULTISAMPLE_NONE || FAILED(rt0->GetContainer(kIID_Texture9, (void**)&tex)) || !tex)
	{
		static bool warned = false;
		if (!warned)
		{
			warned = true;
			LogWarn("TAA: the scene colour is %s; TAA needs the in-game anti-aliasing (MSAA) switched off.",
				c.MultiSampleType != D3DMULTISAMPLE_NONE ? "multisampled" : "not a texture");
		}
		return false;
	}
	tex->Release(); // the game holds it
	sceneTex = tex;
	w = d.Width;
	h = d.Height;
	return true;
}

void Taa_AfterSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s)
{
	if (!g_enabled || g_failed)
		return;
	if (index == 0)
	{
		g_rt0 = s;
		UpdateJitter(dev);
		return;
	}
	if (index != 1)
		return;
	g_rt1 = s;
	if (!s || g_inScene || g_resolved || !Taa_Active())
		return;
	UINT w, h;
	IDirect3DTexture9* tex = nullptr;
	if (!IsMainPass(s, g_rt0, w, h, tex))
		return;
	g_inScene = true;
	g_sceneSurf = g_rt0;
	g_sceneTex = tex;
	g_depthSurf = s;
	g_sceneBound = 0;
	for (int i = 0; i < 21; i++)
		if (g_bound[i] == static_cast<IDirect3DBaseTexture9*>(tex))
			g_sceneBound |= 1u << i;
	if (w != g_sceneW || h != g_sceneH)
	{
		D3DSURFACE_DESC c = {};
		g_rt0->GetDesc(&c);
		LogInfo("TAA: scene found: %ux%u, colour format %d, z/w target R32F.", w, h, (int)c.Format);
		g_sceneW = w;
		g_sceneH = h;
		g_historyValid = false;
		UpdateJitter(dev);
	}
}

static void CaptureCamera(IDirect3DDevice9* dev)
{
	if (!g_curVs || !g_vsLockReady)
		return;
	VsMatrices m;
	EnterCriticalSection(&g_vsLock);
	auto it = g_vsInfo.find(g_curVs);
	if (it != g_vsInfo.end())
		m = it->second;
	LeaveCriticalSection(&g_vsLock);
	if (m.wvp < 0)
		return;
	float wvp[16], world[16];
	if (FAILED(dev->GetVertexShaderConstantF(m.wvp, wvp, 4)) || FAILED(dev->GetVertexShaderConstantF(m.world, world, 4)))
		return;
	char why[160];
	Camera cam;
	if (Taa_CameraFromMatrices(wvp, world, cam, why, sizeof(why)))
	{
		static bool logged = false;
		if (!logged)
		{
			logged = true;
			LogInfo("TAA: camera found (near %.3f m, far %.0f m, horizontal fov %.1f deg).", -cam.B / cam.A, cam.B / (1.0 - cam.A),
				2.0 * atan(1.0 / cam.P00) * 57.29578);
		}
		g_cam = cam;
	}
	else
	{
		static bool warned = false;
		if (!warned)
		{
			warned = true;
			LogWarn("TAA: could not read the camera from the shader constants (%s).", why);
		}
	}
}

static void Resolve(IDirect3DDevice9* dev);

void Taa_BeforeDraw(IDirect3DDevice9* dev)
{
	if (!g_inScene || g_resolved)
		return;
	if (g_sceneBound && g_rt0 != g_sceneSurf)
		Resolve(dev);
	else if (!g_cam.ok && g_rt0 == g_sceneSurf && g_rt1 == g_depthSurf)
		CaptureCamera(dev);
}

void Taa_BeforeCopyFrom(IDirect3DDevice9* dev, IDirect3DSurface9* src)
{
	if (g_inScene && !g_resolved && src && src == g_sceneSurf)
		Resolve(dev);
}

void Taa_OnPresent(IDirect3DDevice9* dev)
{
	if (!g_enabled || g_failed)
		return;
	if (g_inScene && !g_resolved)
	{
		static bool warned = false;
		if (!warned)
		{
			warned = true;
			LogWarn("TAA: a frame drew the scene but never read it back; no TAA on such frames (logged once).");
		}
	}
	// The history is usable next frame only if this frame made one.
	g_historyValid = g_resolvedOk;
	g_jitterFrame = g_resolvedOk && Taa_Active() && g_jitterScale > 0.0f;
	g_frame++;
	unsigned phase = (g_frame & 7) + 1;
	g_jitterPx[0] = (Halton(phase, 2) - 0.5f) * g_jitterScale;
	g_jitterPx[1] = (Halton(phase, 3) - 0.5f) * g_jitterScale;
	g_inScene = g_resolved = g_resolvedOk = false;
	g_sceneSurf = nullptr;
	g_sceneTex = nullptr;
	g_depthSurf = nullptr;
	g_sceneBound = 0;
	g_cam.ok = false;
	UpdateJitter(dev); // the back buffer is bound now: off until the next scene target
}

const char* Taa_Status()
{
	if (!g_enabled)
		return "disabled in the ini";
	if (g_failed)
		return "unavailable (see AC2TAA.log)";
	if (!g_on)
		return "off";
	if (g_historyValid)
		return "working";
	return "waiting for a 3D scene";
}

void Taa_OnLostDevice()
{
	ReleaseResources();
	g_inScene = g_resolved = g_resolvedOk = false;
	g_sceneSurf = nullptr;
	g_sceneTex = nullptr;
	g_depthSurf = nullptr;
	g_rt0 = g_rt1 = nullptr;
	g_sceneBound = 0;
	memset(g_bound, 0, sizeof(g_bound));
	g_jitterFrame = false;
	g_jitterOn = false;
	g_c255Known = false; // Reset sets all constants to 0
	memset(g_gameBias, 0, sizeof(g_gameBias));
	g_cam.ok = g_prevCam.ok = false;
	g_sceneW = g_sceneH = 0;
}

// ---- The passes --------------------------------------------------------------------------------

static void Fullscreen(IDirect3DDevice9* dev)
{
	static const float tri[3][4] = { { -1, -1, 0, 1 }, { -1, 3, 0, 1 }, { 3, -1, 0, 1 } };
	D.DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0]));
}

static void Resolve(IDirect3DDevice9* dev)
{
	g_resolved = true;
	UpdateJitter(dev); // off from here on
	if (!g_cam.ok)
	{
		static bool warned = false;
		if (!warned)
		{
			warned = true;
			LogWarn("TAA: skipped a frame without camera matrices (logged once).");
		}
		return;
	}
	if (!EnsureResources(dev, g_sceneW, g_sceneH))
		return;

	float reproj[4][4];
	bool valid = g_historyValid && g_prevCam.ok && Taa_ReprojectionMatrix(g_cam, g_prevCam, reproj);
	if (!valid)
		memset(reproj, 0, sizeof(reproj));

	// Save everything we touch: state block for states/shaders/constants/textures/viewport, and the
	// render targets, depth buffer and vertex input by hand (state blocks don't hold them, and the
	// game's instanced foliage needs its stream frequencies back).
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
		const DWORD filter = s == 1 ? D3DTEXF_LINEAR : D3DTEXF_POINT;
		D.SetSamplerState(dev, s, D3DSAMP_MINFILTER, filter);
		D.SetSamplerState(dev, s, D3DSAMP_MAGFILTER, filter);
		D.SetSamplerState(dev, s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		D.SetSamplerState(dev, s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		D.SetSamplerState(dev, s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		D.SetSamplerState(dev, s, D3DSAMP_SRGBTEXTURE, FALSE);
		D.SetSamplerState(dev, s, D3DSAMP_MAXMIPLEVEL, 0);
		D.SetSamplerState(dev, s, D3DSAMP_MIPMAPLODBIAS, 0);
	}
	D.SetDepthStencilSurface(dev, nullptr);
	for (DWORD i = 1; i < 4; i++)
		if (oldRt[i])
			D.SetRenderTarget(dev, i, nullptr);
	dev->SetVertexDeclaration(g_decl);
	D.SetVertexShader(dev, g_vsFull);

	IDirect3DTexture9* depthTex = nullptr;
	if (FAILED(g_depthSurf->GetContainer(kIID_Texture9, (void**)&depthTex)))
		depthTex = nullptr;
	const int cur = g_histIndex, prev = cur ^ 1;
	const float W = (float)g_sceneW, H = (float)g_sceneH;
	const float jx = g_jitterFrame ? g_jitterPx[0] : 0.0f, jy = g_jitterFrame ? g_jitterPx[1] : 0.0f;
	const float c[10][4] = {
		{ W, H, 1.0f / W, 1.0f / H },
		{ jx, jy, valid ? 1.0f : 0.0f, (float)g_debugView },
		{ reproj[0][0], reproj[0][1], reproj[0][2], reproj[0][3] },
		{ reproj[1][0], reproj[1][1], reproj[1][2], reproj[1][3] },
		{ reproj[2][0], reproj[2][1], reproj[2][2], reproj[2][3] },
		{ reproj[3][0], reproj[3][1], reproj[3][2], reproj[3][3] },
		{ (float)g_cam.A, (float)g_cam.B, (float)g_cam.P00, (float)g_cam.P11 },
		{ (float)g_cam.P02, (float)g_cam.P12, 0, 0 },
		{ 1.0f - g_feedback, 1.0f - g_motionFeedback, g_motionPixels, g_depthTolerance },
		{ g_sharpness, g_clipGamma, kHdrCompression, kDistrust },
	};
	dev->SetPixelShaderConstantF(0, &c[0][0], 10);

	// Resolve: scene + previous history -> new history.
	D.SetRenderTarget(dev, 0, g_histSurf[cur]);
	D3DVIEWPORT9 vp = { 0, 0, g_sceneW, g_sceneH, 0.0f, 1.0f };
	dev->SetViewport(&vp);
	D.SetTexture(dev, 0, g_sceneTex);
	D.SetTexture(dev, 1, g_histTex[prev]);
	D.SetTexture(dev, 2, depthTex);
	D.SetTexture(dev, 3, g_histTex[prev]);
	dev->SetPixelShader(g_psResolve);
	Fullscreen(dev);

	// Output: new history (sharpened) -> scene colour, colour channels only.
	D.SetRenderTarget(dev, 0, g_sceneSurf);
	dev->SetViewport(&vp);
	D.SetTexture(dev, 0, nullptr);
	D.SetTexture(dev, 1, g_histTex[cur]);
	D.SetTexture(dev, 2, nullptr);
	D.SetTexture(dev, 3, nullptr);
	D.SetSamplerState(dev, 1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
	D.SetSamplerState(dev, 1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
	dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
	dev->SetPixelShader(g_psOutput);
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
	if (depthTex)
		depthTex->Release();
	// The state block put back c255 and the sampler biases as they were when captured.
	g_c255Known = false;
	g_jitterOn = true;
	UpdateJitter(dev);

	g_histIndex = prev;
	g_prevCam = g_cam;
	g_resolvedOk = true;
	static bool first = true;
	if (first)
	{
		first = false;
		LogInfo("TAA: first frame resolved.");
	}
}

} // namespace taa
