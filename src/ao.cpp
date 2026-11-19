// Ambient occlusion: settings, the opaque-pass tracking and the AO passes. See ao.h for the overview
// and aoshader.cpp for the shader rewrite.
//
// The frame (from F8 dumps): ... main opaque pass with RT0 = scene colour (FP16) and RT1 = z/w
// (R32F), toggled off and on between batches; then RT1 is unbound and the sky is drawn, then
// transparents. We bind our indirect-light target as RT2 whenever the game binds a z/w target as
// RT1, and run the AO passes at the first thing after the pass that isn't re-binding RT1.
//
// Camera: the opaque shaders' g_WorldViewProj and g_World give ViewProj = WVP * World^-1, which
// for a D3D perspective projection is P * V with P = [P00 0 P02 0; 0 P11 P12 0; 0 0 A B; 0 0 1 0].
// From the rows: row3 = (forward, ...), so A, B, P00, P11, P02, P12 come out directly; z/w = A + B / z.

#include "ao.h"
#include "common.h"
#include "shaders_gen.h"
#include <math.h>
#include <stdio.h>
#include <unordered_map>

// ---- Settings ----------------------------------------------------------------------------------

static bool g_enabled = false;
static volatile bool g_on = true;
static volatile float g_radius = 1.5f, g_strength = 0.5f;
static volatile int g_view = 0;
static int g_toggleKey = 0;

bool AO_Enabled() { return g_enabled; }
bool AO_Active() { return g_enabled && g_on; }
void AO_SetOn(bool on) { g_on = on; }
float AO_Radius() { return g_radius; }
void AO_SetRadius(float m) { g_radius = m < 0.25f ? 0.25f : m > 8.0f ? 8.0f : m; }
float AO_Strength() { return g_strength; }
void AO_SetStrength(float s) { g_strength = s < 0.0f ? 0.0f : s > 4.0f ? 4.0f : s; }
int AO_View() { return g_view; }
void AO_SetView(int v) { g_view = v < 0 ? 0 : v > 2 ? 2 : v; }

static DWORD WINAPI AoToggleThread(void*)
{
	bool wasDown = false;
	for (;;)
	{
		Sleep(30);
		bool down = (GetAsyncKeyState(g_toggleKey) & 0x8000) != 0;
		if (down && !wasDown && GameHasFocus())
		{
			g_on = !g_on;
			LogInfo("Ambient occlusion toggled %s.", g_on ? "ON" : "OFF");
			Beep(g_on ? 988 : 494, 120);
		}
		wasDown = down;
	}
	return 0;
}

bool AO_Init()
{
	const char* S = "AmbientOcclusion";
	g_enabled = IniInt(S, "Enabled", 1) != 0;
	if (!g_enabled)
	{
		LogInfo("Ambient occlusion: off ([AmbientOcclusion] Enabled=0).");
		return false;
	}
	AO_SetRadius(IniFloat(S, "Radius", 1.5f));
	AO_SetStrength(IniFloat(S, "Strength", 0.5f));
	AO_SetView(IniInt(S, "DebugView", 0));
	g_on = IniInt(S, "StartEnabled", 1) != 0;
	g_toggleKey = IniInt(S, "ToggleKey", VK_F5);
	if (g_toggleKey > 0 && g_toggleKey < 256)
	{
		HANDLE h = CreateThread(NULL, 0, AoToggleThread, NULL, 0, NULL);
		if (h)
			CloseHandle(h);
	}
	LogInfo("Ambient occlusion: radius %.2f m, strength %.2f, toggle key 0x%02X%s.", (double)g_radius, (double)g_strength, g_toggleKey,
		g_on ? "" : " (starting off)");
	return true;
}

// ---- Vertex shaders: where the camera matrices are --------------------------------------------

struct VsInfo
{
	int wvp = -1, world = -1;
};
static CRITICAL_SECTION g_vsLock;
static std::unordered_map<void*, VsInfo> g_vsInfo;
static bool g_vsLockReady = false;

static void InitLock()
{
	if (!g_vsLockReady)
	{
		InitializeCriticalSection(&g_vsLock);
		g_vsLockReady = true;
	}
}

// Minimal vs_3_0 constant table reader (the shared one in sm3.h is for ps_3_0 code).
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

void AO_OnCreateVertexShader(IDirect3DVertexShader9* vs, const DWORD* code)
{
	if (!g_enabled || !vs)
		return;
	InitLock();
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

struct Camera
{
	bool ok = false;
	float A, B, P00, P11, P02, P12;
};
static Camera g_cam;
static bool g_camWarned = false;

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

// Camera from one draw's matrices (registers hold matrix rows: clip.x = dot(position, c[wvp])).
static bool CameraFromMatrices(const float wvp[16], const float world[16], Camera& cam, char* why, size_t whySize)
{
	double M[4][4], Wm[4][4], Wi[4][4], VP[4][4];
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
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++)
			VP[r][c] = M[r][0] * Wi[0][c] + M[r][1] * Wi[1][c] + M[r][2] * Wi[2][c] + M[r][3] * Wi[3][c];
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
	if (sqrt(dot3(res, res)) > 1e-3 * fabs(A) || !(A > 0.5) || !(B < 0) || !(near_ > 0) || p00 < 1e-3 || p11 < 1e-3 || fabs(dot3(r0, r1)) > 1e-3 * p00 * p11)
	{
		snprintf(why, whySize, "unexpected projection (A %g, B %g, P00 %g, P11 %g)", A, B, p00, p11);
		return false;
	}
	cam.A = (float)A;
	cam.B = (float)B;
	cam.P00 = (float)p00;
	cam.P11 = (float)p11;
	cam.P02 = (float)p02;
	cam.P12 = (float)p12;
	cam.ok = true;
	return true;
}

static void CaptureCamera(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs)
{
	if (!vs || !g_vsLockReady)
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
			LogInfo("Ambient occlusion: camera found (near %.3f m, far %.0f m, horizontal fov %.1f deg).", -cam.B / cam.A,
				cam.B / (1.0f - cam.A), 2.0 * atan(1.0 / cam.P00) * 57.29578);
		}
		g_cam = cam;
	}
	else if (!g_camWarned)
	{
		g_camWarned = true;
		LogWarn("Ambient occlusion: could not read the camera from the shader constants (%s).", why);
	}
}

// ---- Device resources --------------------------------------------------------------------------

static AoDeviceFns D;
static const GUID kIID_Texture9 = { 0x85c31227, 0x3de5, 0x4f00, { 0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5 } };
void AO_SetDeviceFns(const AoDeviceFns& fns) { D = fns; }

static IDirect3DTexture9* g_indirectTex;
static IDirect3DSurface9* g_indirectSurf;
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
		if (caps.NumSimultaneousRTs < 3 || !(caps.PrimitiveMiscCaps & D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS) ||
			caps.PixelShaderVersion < D3DPS_VERSION(3, 0))
		{
			Fail("the GPU lacks 3 render targets with independent formats or pixel shader 3.0", 0);
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
	for (int i = 0; i < 2; i++)
	{
		if (FAILED(hr = D.CreateTexture(dev, hw, hh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_G16R16F, D3DPOOL_DEFAULT, &g_aoTex[i], NULL)) &&
			FAILED(hr = D.CreateTexture(dev, hw, hh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &g_aoTex[i], NULL)))
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
			LogWarn("Ambient occlusion: the scene is multisampled (in-game anti-aliasing); AO is not supported with it yet.");
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
		if (!g_inPass || index != 2)
			return;
		if (!s)
			D.SetRenderTarget(dev, 2, g_indirectSurf); // the game reset slot 2: keep ours bound
		else
		{
			static bool warned = false;
			if (!warned)
			{
				warned = true;
				LogWarn("Ambient occlusion: the game bound its own render target 2 during the opaque pass; AO skipped there.");
			}
			g_inPass = false;
			g_done = true;
		}
		return;
	}
	if (s)
	{
		UINT w, h;
		if (g_done || !(g_on || g_view) || !IsOpaquePass(s, rt0, w, h) || !EnsureResources(dev, w, h))
			return;
		if (!g_cleared)
		{
			D.ColorFill(dev, g_indirectSurf, NULL, 0);
			g_cleared = true;
			g_passDraws = 0;
		}
		D.SetRenderTarget(dev, 2, g_indirectSurf);
		g_inPass = true;
		g_pending = false;
		g_depthSurf = s;
		g_colourSurf = rt0;
	}
	else if (g_inPass)
	{
		D.SetRenderTarget(dev, 2, nullptr);
		g_inPass = false;
		if (g_passDraws > 0)
			g_pending = true;
	}
}

bool AO_BeforeDraw(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs)
{
	if (g_pending)
	{
		Composite(dev);
		return true;
	}
	if (g_inPass)
	{
		g_passDraws++;
		if (!g_cam.ok)
			CaptureCamera(dev, vs);
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
		g_inPass = false;
	}
	g_done = g_cleared = g_pending = false;
	g_cam.ok = false;
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

static void Composite(IDirect3DDevice9* dev)
{
	g_pending = false;
	g_done = true;
	if (!g_cam.ok)
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

	const DWORD rs[][2] = {
		{ D3DRS_ZENABLE, FALSE }, { D3DRS_ZWRITEENABLE, FALSE }, { D3DRS_STENCILENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE },
		{ D3DRS_ALPHABLENDENABLE, FALSE }, { D3DRS_SEPARATEALPHABLENDENABLE, FALSE }, { D3DRS_CULLMODE, D3DCULL_NONE },
		{ D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_COLORWRITEENABLE, 0xF }, { D3DRS_SRGBWRITEENABLE, FALSE },
		{ D3DRS_FOGENABLE, FALSE }, { D3DRS_CLIPPLANEENABLE, 0 }, { D3DRS_FILLMODE, D3DFILL_SOLID },
		{ D3DRS_TWOSIDEDSTENCILMODE, FALSE }, { D3DRS_POINTSPRITEENABLE, FALSE },
	};
	for (const auto& r : rs)
		dev->SetRenderState((D3DRENDERSTATETYPE)r[0], r[1]);
	for (DWORD s = 0; s < 3; s++)
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

	const float view = (float)g_view;
	const float c[6][4] = {
		{ (float)g_w, (float)g_h, 1.0f / g_w, 1.0f / g_h },
		{ (float)g_hw, (float)g_hh, 1.0f / g_hw, 1.0f / g_hh },
		{ g_cam.A, g_cam.B, g_cam.P00, g_cam.P11 },
		{ g_cam.P02, g_cam.P12, 0, 0 },
		{ g_radius, 0.5f * g_cam.P00 * g_w, 0.1f * g_h, 0.6f },
		{ g_strength, view, 1, 0 },
	};
	dev->SetPixelShaderConstantF(0, &c[0][0], 6);

	// GTAO -> ao[0]; blur across -> ao[1]; blur down -> ao[0]
	D.SetTexture(dev, 0, depthTex);
	SetTarget(dev, g_aoSurf[0], g_hw, g_hh);
	D.SetPixelShader(dev, g_psGtao);
	Fullscreen(dev);
	D.SetPixelShader(dev, g_psBlur);
	SetTarget(dev, g_aoSurf[1], g_hw, g_hh);
	D.SetTexture(dev, 1, g_aoTex[0]);
	Fullscreen(dev);
	const float down[4] = { g_strength, view, 0, 1 };
	dev->SetPixelShaderConstantF(5, down, 1);
	SetTarget(dev, g_aoSurf[0], g_hw, g_hh);
	D.SetTexture(dev, 1, g_aoTex[1]);
	Fullscreen(dev);

	// Composite: scene colour -= (1 - AO) x indirect (colour channels only).
	SetTarget(dev, g_colourSurf, g_w, g_h);
	D.SetTexture(dev, 1, g_aoTex[0]);
	D.SetTexture(dev, 2, g_indirectTex);
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
