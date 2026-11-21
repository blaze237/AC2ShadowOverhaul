// See taa.h for the overview, vsjitter.cpp for the vertex shader side, shaders/taa.hlsl for the passes.
#include "taa.h"
#include "vsjitter.h"
#include "common.h"
#include "shaders_gen.h"
#include "core/scene.h"
#include "core/fxpass.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <map>

namespace taa
{

static_assert(kJitterReg == core::kJitterRegister, "TAA's jitter register is the one the effects' passes clear");

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

static CRITICAL_SECTION g_vsLock;
static bool g_vsLockReady = false;
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

// ---- Camera ------------------------------------------------------------------------------------

// This frame's camera is core::Scene().camera (the first opaque draw with the camera matrices; they
// are unjittered: the jitter is added in the vertex shaders). g_camOk: it was there by the time the
// main pass drew into this frame's scene targets. g_prevCam: the camera of the history.
static bool g_camOk = false;
static core::Camera g_prevCam;

// ---- Device state ------------------------------------------------------------------------------

static const core::DeviceFns& D = core::Orig();

static bool g_failed = false; // TAA can't run on this device (logged once)
static core::RenderTarget g_hist[2];
static int g_histIndex = 0; // the one written next
static UINT g_histW, g_histH;
static core::PassState g_state;
static IDirect3DVertexShader9* g_vsFull;
static IDirect3DPixelShader9 *g_psResolve, *g_psOutput;
static IDirect3DVertexDeclaration9* g_decl;

// Per frame.
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
	for (int i = 0; i < 2; i++)
		g_hist[i].Release();
	g_state.Release();
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
		// Our own full-screen VS goes to the original CreateVertexShader (never jittered).
		if (FAILED(hr = D.CreateVertexShader(dev, kTaaFullscreenVs, &g_vsFull)) || FAILED(hr = dev->CreatePixelShader(kTaaResolvePs, &g_psResolve)) ||
			FAILED(hr = dev->CreatePixelShader(kTaaOutputPs, &g_psOutput)) || !(g_decl = core::FullscreenDecl(dev)))
		{
			Fail("could not create its shaders", hr);
			return false;
		}
	}
	if (g_hist[0].tex && g_histW == w && g_histH == h)
		return true;
	ReleaseResources();
	for (int i = 0; i < 2; i++)
		if (FAILED(hr = g_hist[i].Create(dev, w, h, D3DFMT_A16B16G16R16F)))
		{
			Fail("could not create its history targets", hr);
			return false;
		}
	if (FAILED(hr = g_state.Create(dev)))
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
	if (FAILED(s->GetContainer(core::kIID_Texture9, (void**)&tex)) || !tex)
		return false;
	tex->Release();
	return true;
}

static void UpdateJitter(IDirect3DDevice9* dev)
{
	bool on = g_jitterFrame && !g_resolved && Taa_Active() && IsJitterTarget(core::Scene().rt0);
	float v[4] = { 0, 0, 0, 0 };
	if (on)
	{
		v[0] = 2.0f * g_jitterPx[0] / (float)g_sceneW;
		v[1] = -2.0f * g_jitterPx[1] / (float)g_sceneH;
	}
	SetC255(dev, v);
	core::Scene_TaaJitter(on, v, g_jitterPx); // the camera read of the frame takes what the scene draws get
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
	const core::ZwBinding b = core::InspectRt1(s, rt0);
	if (!b.zw || !b.zwTexture)
		return false;
	if (b.colourMsaa || !b.colourTexture)
	{
		static bool warned = false;
		if (!warned)
		{
			warned = true;
			LogWarn("TAA: the scene colour is %s; TAA needs the in-game anti-aliasing (MSAA) switched off.",
				b.colourMsaa ? "multisampled" : "not a texture");
		}
		return false;
	}
	sceneTex = b.colourTexture;
	w = b.width;
	h = b.height;
	return true;
}

void Taa_AfterSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s)
{
	if (!g_enabled || g_failed)
		return;
	if (index == 0)
	{
		UpdateJitter(dev);
		return;
	}
	if (index != 1)
		return;
	if (!s || g_inScene || g_resolved || !Taa_Active())
		return;
	IDirect3DSurface9* rt0 = core::Scene().rt0;
	UINT w, h;
	IDirect3DTexture9* tex = nullptr;
	if (!IsMainPass(s, rt0, w, h, tex))
		return;
	g_inScene = true;
	g_sceneSurf = rt0;
	g_sceneTex = tex;
	g_depthSurf = s;
	g_sceneBound = 0;
	for (int i = 0; i < 21; i++)
		if (g_bound[i] == static_cast<IDirect3DBaseTexture9*>(tex))
			g_sceneBound |= 1u << i;
	if (w != g_sceneW || h != g_sceneH)
	{
		D3DSURFACE_DESC c = {};
		rt0->GetDesc(&c);
		LogInfo("TAA: scene found: %ux%u, colour format %d, z/w target R32F.", w, h, (int)c.Format);
		g_sceneW = w;
		g_sceneH = h;
		g_historyValid = false;
		UpdateJitter(dev);
	}
	// The other effects run between here and the resolve: tell them TAA resolves this frame.
	core::Scene_TaaFrame(g_historyValid, g_frame);
}

static void Resolve(IDirect3DDevice9* dev);

void Taa_BeforeDraw(IDirect3DDevice9* dev)
{
	if (!g_inScene || g_resolved)
		return;
	const core::SceneState& scene = core::Scene();
	if (g_sceneBound && scene.rt0 != g_sceneSurf)
		Resolve(dev);
	else if (!g_camOk && scene.rt0 == g_sceneSurf && scene.rt1 == g_depthSurf)
		g_camOk = scene.camera.ok; // set by the first draw into the scene targets with the camera matrices
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
	g_camOk = false;
	UpdateJitter(dev); // the back buffer is bound now: off until the next scene target
}

const char* Taa_Status()
{
	if (!g_enabled)
		return "disabled in the ini";
	if (g_failed)
		return "unavailable (see AC2ShadowOverhaul.log)";
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
	g_sceneBound = 0;
	memset(g_bound, 0, sizeof(g_bound));
	g_jitterFrame = false;
	g_jitterOn = false;
	g_c255Known = false; // Reset sets all constants to 0
	memset(g_gameBias, 0, sizeof(g_gameBias));
	g_camOk = g_prevCam.ok = false;
	g_sceneW = g_sceneH = 0;
}

// ---- The passes --------------------------------------------------------------------------------

static void Resolve(IDirect3DDevice9* dev)
{
	g_resolved = true;
	UpdateJitter(dev); // off from here on
	if (!g_camOk)
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

	const core::Camera& cam = core::Scene().camera;
	float reproj[4][4];
	bool valid = g_historyValid && g_prevCam.ok && core::ReprojectionMatrix(cam, g_prevCam, reproj);
	if (!valid)
		memset(reproj, 0, sizeof(reproj));

	g_state.Save(dev);
	g_state.BeginPasses(dev, g_decl, g_vsFull);
	for (DWORD s = 0; s < 4; s++)
	{
		core::SetSamplerClamp(dev, s, s == 1 ? D3DTEXF_LINEAR : D3DTEXF_POINT);
		D.SetSamplerState(dev, s, D3DSAMP_MIPMAPLODBIAS, 0); // not through the hook (it adds our mip bias)
	}

	IDirect3DTexture9* depthTex = nullptr;
	if (FAILED(g_depthSurf->GetContainer(core::kIID_Texture9, (void**)&depthTex)))
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
		{ (float)cam.A, (float)cam.B, (float)cam.P00, (float)cam.P11 },
		{ (float)cam.P02, (float)cam.P12, 0, 0 },
		{ 1.0f - g_feedback, 1.0f - g_motionFeedback, g_motionPixels, g_depthTolerance },
		{ g_sharpness, g_clipGamma, kHdrCompression, kDistrust },
	};
	dev->SetPixelShaderConstantF(0, &c[0][0], 10);

	// Resolve: scene + previous history -> new history.
	core::SetTarget(dev, g_hist[cur].surf, g_sceneW, g_sceneH);
	D.SetTexture(dev, 0, g_sceneTex);
	D.SetTexture(dev, 1, g_hist[prev].tex);
	D.SetTexture(dev, 2, depthTex);
	D.SetTexture(dev, 3, g_hist[prev].tex);
	dev->SetPixelShader(g_psResolve);
	core::DrawFullscreen(dev);

	// Output: new history (sharpened) -> scene colour, colour channels only.
	core::SetTarget(dev, g_sceneSurf, g_sceneW, g_sceneH);
	D.SetTexture(dev, 0, nullptr);
	D.SetTexture(dev, 1, g_hist[cur].tex);
	D.SetTexture(dev, 2, nullptr);
	D.SetTexture(dev, 3, nullptr);
	D.SetSamplerState(dev, 1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
	D.SetSamplerState(dev, 1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
	dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
	dev->SetPixelShader(g_psOutput);
	core::DrawFullscreen(dev);

	g_state.Restore(dev);
	if (depthTex)
		depthTex->Release();
	// The state block put back c255 and the sampler biases as they were when captured.
	g_c255Known = false;
	g_jitterOn = true;
	UpdateJitter(dev);

	g_histIndex = prev;
	g_prevCam = cam;
	g_resolvedOk = true;
	static bool first = true;
	if (first)
	{
		first = false;
		LogInfo("TAA: first frame resolved.");
	}
}

} // namespace taa
