// See motionblur.h for the overview, shaders/motionblur.hlsl for the passes.
#include "motionblur.h"
#include "common.h"
#include "shaders_gen.h"
#include "core/scene.h"
#include "core/camera.h"
#include "core/fxpass.h"
#include "core/gputimer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace mb
{

// ---- Settings ----------------------------------------------------------------------------------

static bool g_enabled = false;
static MbSettings g_set = { true, 1.0f, 0.5f, 32, 16, 0 };

// Reconstruction: depths within this fraction of each other blend.
static const float kSoftDepth = 0.05f;

static float Clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static int ClampI(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

bool Mb_Enabled() { return g_enabled; }
bool Mb_Active() { return g_enabled && g_set.on; }
MbSettings Mb_GetSettings() { return g_set; }

void Mb_SetSettings(const MbSettings& s)
{
	g_set.on = s.on;
	g_set.strength = Clamp(s.strength, 0.0f, 2.0f);
	g_set.shutter = Clamp(s.shutter, 0.05f, 1.0f);
	g_set.maxBlur = ClampI(s.maxBlur, 8, 64);
	g_set.samples = ClampI(s.samples, 4, 32);
	g_set.debugView = ClampI(s.debugView, 0, 1);
}

bool Mb_Init()
{
	const char* S = "MotionBlur";
	g_enabled = IniInt(S, "Enabled", 1) != 0;
	if (!g_enabled)
	{
		LogInfo("Motion blur: off ([MotionBlur] Enabled=0).");
		return false;
	}
	MbSettings s;
	s.on = IniInt(S, "StartEnabled", 1) != 0;
	s.strength = IniFloat(S, "Strength", 1.0f);
	s.shutter = IniFloat(S, "Shutter", 0.5f);
	s.maxBlur = IniInt(S, "MaxBlur", 32);
	s.samples = IniInt(S, "Samples", 16);
	s.debugView = IniInt(S, "DebugView", 0);
	Mb_SetSettings(s);
	LogInfo("Motion blur: strength %.2f, shutter %.2f, max %d px (1080p), %d samples%s%s.", (double)g_set.strength, (double)g_set.shutter,
		g_set.maxBlur, g_set.samples, g_set.on ? "" : " (starting off)", g_set.debugView ? ", debug view on" : "");
	return true;
}

// ---- Device state ------------------------------------------------------------------------------

static const core::DeviceFns& D = core::Orig();

static bool g_canRead = true; // not a pure device (the camera can be read from the constants)
static bool g_failed = false;
static core::RenderTarget g_prep, g_tile, g_nmax, g_copy;
static UINT g_w = 0, g_h = 0, g_tilesX = 0, g_tilesY = 0, g_tileK = 0;
static D3DFORMAT g_copyFmt = D3DFMT_UNKNOWN;
static core::PassState g_state;
static IDirect3DVertexShader9* g_vsFull;
static IDirect3DPixelShader9 *g_psPrep, *g_psTileMax, *g_psNeighborMax, *g_psReconstruct;
static IDirect3DVertexDeclaration9* g_decl;

// Per frame.
static bool g_applied = false;               // the passes ran (or were skipped) this frame
static IDirect3DBaseTexture9* g_bound[21];   // textures bound per sampler (0-15, then the 5 vertex samplers)
static unsigned g_lastCamFrame = 0;          // the frame (core::Scene().frame) that last had a camera
static bool g_lastCamKnown = false;

// Status for the menu: -1 waiting for a scene, 0 camera still, 1 blurring.
static int g_lastState = -1;

static void ReleaseResources()
{
	g_prep.Release();
	g_tile.Release();
	g_nmax.Release();
	g_copy.Release();
	g_state.Release();
	g_w = g_h = g_tilesX = g_tilesY = g_tileK = 0;
}

static void Fail(const char* what, HRESULT hr)
{
	LogWarn("Motion blur disabled: %s (0x%08lX).", what, hr);
	g_failed = true;
	ReleaseResources();
}

void Mb_OnDeviceCreated(IDirect3DDevice9* dev, DWORD flags)
{
	g_canRead = !(flags & D3DCREATE_PUREDEVICE);
	if (!g_enabled)
		return;
	D3DCAPS9 caps = {};
	dev->GetDeviceCaps(&caps);
	if (caps.PixelShaderVersion < D3DPS_VERSION(3, 0))
		Fail("the GPU lacks shader model 3", 0);
}

static bool EnsureResources(IDirect3DDevice9* dev, UINT w, UINT h, UINT K, D3DFORMAT sceneFmt)
{
	if (g_failed)
		return false;
	HRESULT hr = S_OK;
	if (!g_vsFull)
	{
		if (FAILED(hr = D.CreateVertexShader(dev, kMbFullscreenVs, &g_vsFull)) || FAILED(hr = dev->CreatePixelShader(kMbPrepPs, &g_psPrep)) ||
			FAILED(hr = dev->CreatePixelShader(kMbTileMaxPs, &g_psTileMax)) || FAILED(hr = dev->CreatePixelShader(kMbNeighborMaxPs, &g_psNeighborMax)) ||
			FAILED(hr = dev->CreatePixelShader(kMbReconstructPs, &g_psReconstruct)) || !(g_decl = core::FullscreenDecl(dev)))
		{
			Fail("could not create its shaders", hr);
			return false;
		}
	}
	if (g_prep.tex && g_w == w && g_h == h && g_tileK == K && g_copyFmt == sceneFmt)
		return true;
	ReleaseResources();
	const UINT tx = (w + K - 1) / K, ty = (h + K - 1) / K;
	if (FAILED(hr = g_prep.Create(dev, w, h, D3DFMT_A16B16G16R16F)) || FAILED(hr = g_tile.Create(dev, tx, ty, D3DFMT_A16B16G16R16F)) ||
		FAILED(hr = g_nmax.Create(dev, tx, ty, D3DFMT_A16B16G16R16F)) || FAILED(hr = g_copy.Create(dev, w, h, sceneFmt)))
	{
		Fail("could not create its render targets", hr);
		return false;
	}
	if (FAILED(hr = g_state.Create(dev)))
	{
		Fail("could not create a state block", hr);
		return false;
	}
	g_w = w;
	g_h = h;
	g_tileK = K;
	g_tilesX = tx;
	g_tilesY = ty;
	g_copyFmt = sceneFmt;
	LogInfo("Motion blur: targets created for %ux%u (tiles of %u px, %ux%u).", w, h, K, tx, ty);
	return true;
}

// ---- The passes --------------------------------------------------------------------------------

static bool SameMatrix(const core::Mat4& a, const core::Mat4& b) { return memcmp(&a, &b, sizeof(a)) == 0; }

static void Apply(IDirect3DDevice9* dev)
{
	g_applied = true;
	const core::SceneState& S = core::Scene();
	if (!Mb_Active() || g_failed || !g_canRead || !S.camera.ok || !S.rasterCamera.ok || !S.sceneColour || !S.zw)
		return;
	// The camera's motion: known if the last frame had a camera too (not loading, not a cut).
	float reproj[4][4];
	const bool camValid = S.prevCamera.ok && g_lastCamKnown && g_lastCamFrame + 1 == S.frame && !core::Scene_CameraCut() &&
		core::ReprojectionMatrix(S.camera, S.prevCamera, reproj);
	const bool still = camValid && SameMatrix(S.camera.viewProj, S.prevCamera.viewProj);
	if (!camValid || still || g_set.strength <= 0.0f)
	{
		g_lastState = 0;
		if (!g_set.debugView || !camValid)
			return; // nothing would blur
	}
	D3DSURFACE_DESC sd = {};
	if (FAILED(S.sceneColour->GetDesc(&sd)))
		return;
	const UINT W = sd.Width, H = sd.Height;
	// Tiles as big as the longest half-streak (the gather reaches from -v to +v).
	const UINT K = (UINT)ClampI((int)floorf(g_set.maxBlur * 0.5f * (float)H / 1080.0f + 0.5f), 4, 64);
	if (!EnsureResources(dev, W, H, K, sd.Format))
		return;
	IDirect3DTexture9* zwTex = nullptr;
	if (FAILED(S.zw->GetContainer(core::kIID_Texture9, (void**)&zwTex)) || !zwTex)
		return;

	g_state.Save(dev);
	core::GpuTimer_Begin(dev, core::GT_MB);
	g_state.BeginPasses(dev, g_decl, g_vsFull);
	for (DWORD s = 0; s < 3; s++)
		core::SetSamplerClamp(dev, s, D3DTEXF_POINT);

	const core::Camera& rc = S.rasterCamera;
	const float jx = S.taa.jittered ? S.taa.jitterNdc[0] : 0.0f, jy = S.taa.jittered ? S.taa.jitterNdc[1] : 0.0f;
	const float c[10][4] = {
		{ (float)W, (float)H, 1.0f / W, 1.0f / H },
		{ rc.f.A, rc.f.B, rc.f.P00, rc.f.P11 },
		{ rc.f.P02, rc.f.P12, jx, jy },
		{ reproj[0][0], reproj[0][1], reproj[0][2], reproj[0][3] },
		{ reproj[1][0], reproj[1][1], reproj[1][2], reproj[1][3] },
		{ reproj[2][0], reproj[2][1], reproj[2][2], reproj[2][3] },
		{ reproj[3][0], reproj[3][1], reproj[3][2], reproj[3][3] },
		{ g_set.strength, g_set.shutter, 0.0f, (float)K },
		{ (float)g_set.samples, kSoftDepth, (float)g_set.debugView, 0.0f },
		{ (float)g_tilesX, (float)g_tilesY, 1.0f / g_tilesX, 1.0f / g_tilesY },
	};
	dev->SetPixelShaderConstantF(0, &c[0][0], 10);

	// Every pixel's velocity.
	core::SetTarget(dev, g_prep.surf, W, H);
	D.SetTexture(dev, 0, zwTex);
	dev->SetPixelShader(g_psPrep);
	core::DrawFullscreen(dev);

	// Tiles.
	core::SetTarget(dev, g_tile.surf, g_tilesX, g_tilesY);
	D.SetTexture(dev, 0, g_prep.tex);
	dev->SetPixelShader(g_psTileMax);
	core::DrawFullscreen(dev);
	core::SetTarget(dev, g_nmax.surf, g_tilesX, g_tilesY);
	D.SetTexture(dev, 0, g_tile.tex);
	dev->SetPixelShader(g_psNeighborMax);
	core::DrawFullscreen(dev);

	// The blur, into the scene colour (colour channels only, like TAA's output).
	D.SetTexture(dev, 0, nullptr);
	D.StretchRect(dev, S.sceneColour, nullptr, g_copy.surf, nullptr, D3DTEXF_POINT);
	core::SetTarget(dev, S.sceneColour, W, H);
	D.SetTexture(dev, 0, g_copy.tex);
	D.SetTexture(dev, 1, g_prep.tex);
	D.SetTexture(dev, 2, g_nmax.tex);
	dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
	dev->SetPixelShader(g_psReconstruct);
	core::DrawFullscreen(dev);
	for (DWORD s = 0; s < 3; s++)
		D.SetTexture(dev, s, nullptr);

	core::GpuTimer_End(core::GT_MB);
	g_state.Restore(dev);
	zwTex->Release();
	if (!still && g_set.strength > 0.0f)
		g_lastState = 1;
	static bool first = true;
	if (first)
	{
		first = false;
		LogInfo("Motion blur: first frame blurred.");
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

void Mb_OnSetTexture(DWORD stage, IDirect3DBaseTexture9* tex)
{
	int slot = SamplerSlot(stage);
	if (slot >= 0)
		g_bound[slot] = tex;
}

// The first read of the scene colour after the main pass (the same trigger as TAA's resolve, which
// runs just before this in the same call).
void Mb_BeforeDraw(IDirect3DDevice9* dev)
{
	if (!g_enabled || g_applied)
		return;
	const core::SceneState& S = core::Scene();
	if (!S.passSeen || !S.sceneColourTexture || S.rt0 == S.sceneColour)
		return;
	for (int i = 0; i < 21; i++)
		if (g_bound[i] == static_cast<IDirect3DBaseTexture9*>(S.sceneColourTexture))
		{
			Apply(dev);
			return;
		}
}

void Mb_BeforeCopyFrom(IDirect3DDevice9* dev, IDirect3DSurface9* src)
{
	if (!g_enabled || g_applied)
		return;
	const core::SceneState& S = core::Scene();
	if (S.passSeen && src && src == S.sceneColour)
		Apply(dev);
}

void Mb_OnPresent(IDirect3DDevice9*)
{
	if (!g_enabled)
		return;
	const core::SceneState& S = core::Scene();
	if (S.camera.ok)
	{
		g_lastCamFrame = S.frame; // Scene_EndFrame (after this) moves it to prevCamera
		g_lastCamKnown = true;
	}
	g_applied = false;
}

void Mb_OnLostDevice()
{
	ReleaseResources();
	g_applied = false;
	memset(g_bound, 0, sizeof(g_bound));
	g_lastCamKnown = false;
}

const char* Mb_Status()
{
	if (!g_enabled)
		return "disabled in the ini";
	if (g_failed || !g_canRead)
		return "unavailable (see AC2ShadowOverhaul.log)";
	if (!g_set.on)
		return "off";
	if (g_lastState < 0)
		return "waiting for a 3D scene";
	return g_lastState ? "working (camera moving)" : "working (camera still: nothing to blur)";
}

} // namespace mb
