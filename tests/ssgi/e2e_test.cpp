// End-to-end test of SSGI: built as AssassinsCreedIIGame.exe, statically imports d3d9!Direct3DCreate9
// like the game (the mock d3d9.dll next to it), loads scripts\AC2ShadowOverhaul.asi (like the ASI
// loader), and renders frames the way AC2 does:
//   opaque batches with RT0 = scene colour, RT1 = z/w (R32F), RT1 toggled off and on between them,
//   then RT1 unbound and the sky drawn, then post-processing into the back buffer, Present.
// Checks: the game's real pixel shaders (re/shaders/dump, if present) come out rewritten; the GI
// passes run once per frame right after the opaque pass and restore the game's state; s13 holds
// black in the first frame and our GI texture after it, but not outside the scene colour; c216-c218
// hold the reprojection rows for the camera's move between frames; the SSGI menu (F3) switches,
// changes and saves settings; a device Reset works.
//
// Usage: AssassinsCreedIIGame.exe [all]
//   (default) SSGI alone: ambient occlusion, contact shadows, volumetrics and TAA Enabled=0
//   all       every effect on: the AO passes (4) run too, before ours, the geometry prep they share
//             runs once (before both), and the game's shaders get both the AO output and the bounce light
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include "e2e_common.h"
#include "ssgi/gi.h"
#include "core/camera.h"
using namespace ssgi;
using namespace core;

static int g_fail = 0;
#define CHECK(cond, ...)                                                                                                              \
	do                                                                                                                                \
	{                                                                                                                                 \
		if (!(cond))                                                                                                                  \
		{                                                                                                                             \
			g_fail++;                                                                                                                 \
			printf("FAIL line %d: ", __LINE__);                                                                                       \
			printf(__VA_ARGS__);                                                                                                      \
			printf("\n");                                                                                                             \
		}                                                                                                                             \
	} while (0)

// ---- Shaders -----------------------------------------------------------------------------------

struct Blob
{
	struct
	{
		void* q;
		void* a;
		ULONG(__stdcall* Release)(void*);
		void*(__stdcall* Ptr)(void*);
		SIZE_T(__stdcall* Size)(void*);
	}* vt;
};
typedef HRESULT(WINAPI* PFN_Compile)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, Blob**, Blob**);

// A vertex shader with the game's camera constants (g_WorldViewProj c0-c3, g_World c8-c11).
static const char* kVs = "float4x4 g_WorldViewProj : register(c0); float4x4 g_World : register(c8);\n"
						 "float4 main(float4 p : POSITION) : POSITION { return mul(g_WorldViewProj, p) + mul(g_World, p) * 1e-9; }\n";

static std::vector<std::vector<DWORD>> LoadDump(const char* dir)
{
	std::vector<std::vector<DWORD>> out;
	char pattern[MAX_PATH];
	snprintf(pattern, sizeof(pattern), "%s\\*.cso", dir);
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return out;
	do
	{
		char path[MAX_PATH];
		snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
		FILE* f = fopen(path, "rb");
		std::vector<DWORD> code(fd.nFileSizeLow / 4 + 1, 0);
		fread(code.data(), 1, fd.nFileSizeLow, f);
		fclose(f);
		out.push_back(code);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	return out;
}

// ---- Camera ------------------------------------------------------------------------------------

static const UINT W = 1280, H = 720;
static Mat4 ViewProj(double ex, double ey, double ez)
{
	// Looking along +y (world z up), like the game's coordinate system. Rows: right, up, forward.
	const double A = 1779.0 / (1779.0 - 0.1), B = -0.1 * 1779.0 / (1779.0 - 0.1), p00 = 1.317, p11 = 1.317 * W / H;
	Mat4 V = { { { 1, 0, 0, -ex }, { 0, 0, 1, -ez }, { 0, 1, 0, -ey }, { 0, 0, 0, 1 } } };
	Mat4 P = { { { p00, 0, 0, 0 }, { 0, p11, 0, 0 }, { 0, 0, A, B }, { 0, 0, 1, 0 } } };
	return Mul(P, V);
}

static void SetCamera(IDirect3DDevice9* dev, const Mat4& vp)
{
	// One object with a world matrix (translation), WVP = VP * World.
	Mat4 world = { { { 1, 0, 0, 5 }, { 0, 1, 0, 7 }, { 0, 0, 1, 1 }, { 0, 0, 0, 1 } } };
	Mat4 wvp = Mul(vp, world);
	float a[16], b[16];
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++)
		{
			a[r * 4 + c] = (float)wvp.m[r][c];
			b[r * 4 + c] = (float)world.m[r][c];
		}
	dev->SetVertexShaderConstantF(0, a, 4);
	dev->SetVertexShaderConstantF(8, b, 4);
}

static UINT TexWidth(IDirect3DBaseTexture9* t)
{
	if (!t)
		return 0;
	D3DSURFACE_DESC d = {};
	((IDirect3DTexture9*)t)->GetLevelDesc(0, &d);
	return d.Width;
}

int main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	bool all = argc > 1 && !strcmp(argv[1], "all");
	printf("SSGI end to end, %s\n", all ? "with every effect on" : "SSGI alone");
	E2EEffects fx;
	fx.ssgi = true;
	if (all)
		fx = E2EEffects::All();
	if (!E2E_WriteIni(fx))
		return 1;
	if (!E2E_LoadPlugin())
		return 1;
	PFN_MockGetGiStats getStats = MockExport<PFN_MockGetGiStats>("MockGetGiStats");
	PFN_MockGetLog getLog = MockExport<PFN_MockGetLog>("MockGetLog");
	MockGiStats* st = getStats ? getStats() : nullptr;
	MockLog* log = getLog ? getLog() : nullptr;
	if (!st || !log)
		return printf("not the mock d3d9.dll\n"), 1;

	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	D3DPRESENT_PARAMETERS pp = {};
	pp.Windowed = TRUE;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.BackBufferFormat = D3DFMT_X8R8G8B8;
	pp.BackBufferWidth = W;
	pp.BackBufferHeight = H;
	pp.EnableAutoDepthStencil = TRUE;
	pp.AutoDepthStencilFormat = D3DFMT_D24S8;
	IDirect3DDevice9* dev = nullptr;
	if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)) || !dev)
		return printf("CreateDevice failed\n"), 1;

	// Shaders.
	HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
	PFN_Compile compile = dc ? (PFN_Compile)GetProcAddress(dc, "D3DCompile") : nullptr;
	Blob* vsCode = nullptr;
	if (!compile || FAILED(compile(kVs, strlen(kVs), "vs", nullptr, nullptr, "main", "vs_3_0", 0, 0, &vsCode, nullptr)))
		return printf("cannot compile the test vertex shader\n"), 1;
	IDirect3DVertexShader9* vs = nullptr;
	dev->CreateVertexShader((const DWORD*)vsCode->vt->Ptr(vsCode), &vs);
	std::vector<std::vector<DWORD>> dump = LoadDump("..\\..\\..\\re\\shaders\\dump");
	std::vector<IDirect3DPixelShader9*> gamePs;
	for (auto& code : dump)
	{
		IDirect3DPixelShader9* ps = nullptr;
		if (SUCCEEDED(dev->CreatePixelShader(code.data(), &ps)))
			gamePs.push_back(ps);
	}
	printf("%zu game pixel shaders created: %d with the GI sampler, %d with an oC2 output\n", dump.size(), st->psWithS13, st->psWithOC2);
	if (!dump.empty())
	{
		// Soft shadows are part of the plugin: several variants of each shader are created (also with
		// FilterWidth=0, for the menu to switch live), all with our output.
		CHECK(st->psWithS13 > 28, "expected the 28 rewritable dumped shaders and their variants, got %d", st->psWithS13);
		if (all)
			CHECK(st->psWithOC2 >= 28, "the AO output is missing (%d shaders)", st->psWithOC2);
		else
			CHECK(st->psWithOC2 == 0, "AO is off, yet %d shaders got its output", st->psWithOC2);
	}
	else
		printf("(no shader dump found: shader rewrite not checked here)\n");

	// The game's targets.
	IDirect3DTexture9 *colourTex = nullptr, *zwTex = nullptr;
	dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &colourTex, nullptr);
	dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &zwTex, nullptr);
	IDirect3DSurface9 *colour = nullptr, *zw = nullptr, *bb = nullptr;
	colourTex->GetSurfaceLevel(0, &colour);
	zwTex->GetSurfaceLevel(0, &zw);
	dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);

	struct FrameResult
	{
		int up;                         // every fullscreen pass (ours and the other effects')
		E2EPassCount fx;                // the passes per effect
		UINT opaqueS13;                 // width of the texture at s13 during the opaque pass (1 = black)
		float rows[3][4];
		IDirect3DSurface9 *skyRt0, *skyRt1;
		UINT skyFreq1;
		UINT postS13;
		IDirect3DPixelShader9* skyPs;
		bool aoBeforeOurs;              // every AO pass came before our first one
		bool sharedFirst;               // the shared geometry prep came before both
	};
	IDirect3DPixelShader9* skyPsSet = gamePs.empty() ? nullptr : gamePs[0];
	auto frame = [&](const Mat4& vp) {
		FrameResult r = {};
		int up0 = st->upDraws;
		size_t d0 = log->draws.size();
		dev->BeginScene();
		dev->SetRenderTarget(0, colour);
		dev->SetRenderTarget(1, zw);
		dev->SetVertexShader(vs);
		SetCamera(dev, vp);
		for (int batch = 0; batch < 3; batch++)
		{
			if (batch)
				dev->SetRenderTarget(1, zw);
			for (int d = 0; d < 3; d++)
				dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
			r.opaqueS13 = TexWidth(st->tex13);
			memcpy(r.rows, st->c216, sizeof(r.rows));
			dev->SetRenderTarget(1, nullptr);
		}
		// Instanced foliage left a stream frequency set; then the sky.
		dev->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1);
		dev->SetPixelShader(skyPsSet);
		dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
		r.skyRt0 = st->rt[0];
		r.skyRt1 = st->rt[1];
		r.skyFreq1 = st->freq1;
		r.skyPs = st->ps;
		dev->SetStreamSourceFreq(1, 1);
		// Post-processing into the back buffer.
		dev->SetRenderTarget(0, bb);
		dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
		r.postS13 = TexWidth(st->tex13);
		dev->EndScene();
		dev->Present(nullptr, nullptr, nullptr, nullptr);
		r.up = st->upDraws - up0;
		r.fx = E2E_CountPasses(log, d0, log->draws.size());
		size_t lastAo = 0, firstOurs = (size_t)-1, firstAo = (size_t)-1, shared = (size_t)-1;
		for (size_t i = d0; i < log->draws.size(); i++)
		{
			E2EEffect e = E2E_Effect(log->draws[i]);
			if (e == FX_AO)
				lastAo = i;
			if (e == FX_AO && firstAo == (size_t)-1)
				firstAo = i;
			if (e == FX_SSGI && firstOurs == (size_t)-1)
				firstOurs = i;
			if (e == FX_SHARED)
				shared = i;
		}
		r.aoBeforeOurs = !r.fx.n[FX_AO] || lastAo < firstOurs;
		// The shared geometry prep comes before everything that reads it.
		r.sharedFirst = !r.fx.n[FX_SHARED] || (shared < firstAo && shared < firstOurs);
		return r;
	};
	const int ours = 5;              // radiance, trace, temporal, blur x2
	const int ao = all ? 4 : 0;      // GTAO, blur x2, composite
	// Our passes, and everything else: alone, nothing else draws; with every effect on, the AO
	// passes run too, before ours. Either way the geometry prep shared with AO runs once, first
	// (whichever effect runs first asks for it), and not at all when neither runs. The volumetrics
	// and TAA don't run in this frame (no sky is drawn with a 1..1 viewport and the scene colour is
	// never read as a texture).
	auto passesOk = [&](const FrameResult& r, int expectOurs) {
		const int shared = expectOurs || ao ? 1 : 0;
		return r.fx.n[FX_SSGI] == expectOurs && r.fx.n[FX_AO] == ao && r.fx.n[FX_SHARED] == shared && r.aoBeforeOurs && r.sharedFirst &&
			r.up == expectOurs + ao + shared;
	};
	auto describe = [&](const FrameResult& r) {
		char buf[180];
		snprintf(buf, sizeof(buf), "%d fullscreen passes (shared geometry prep %d, SSGI %d, AO %d, volumetrics %d, TAA %d)", r.up,
			r.fx.n[FX_SHARED], r.fx.n[FX_SSGI], r.fx.n[FX_AO], r.fx.n[FX_VOL], r.fx.n[FX_TAA]);
		return std::string(buf);
	};

	Mat4 vp0 = ViewProj(100, 200, 30), vp1 = ViewProj(100.2, 200.5, 30.05);
	FrameResult f0 = frame(vp0);
	printf("frame 1: %s, s13 width %u during the opaque pass\n", describe(f0).c_str(), f0.opaqueS13);
	CHECK(passesOk(f0, ours), "frame 1: %s, expected the shared prep once first, SSGI %d, AO %d (before SSGI)", describe(f0).c_str(), ours, ao);
	CHECK(f0.opaqueS13 == 1, "frame 1: s13 should be black (1x1), is %u wide", f0.opaqueS13);
	CHECK(f0.skyRt0 == colour && f0.skyRt1 == nullptr, "frame 1: targets not restored for the sky");
	CHECK(f0.skyFreq1 == (D3DSTREAMSOURCE_INSTANCEDATA | 1), "frame 1: stream frequency not restored (%u)", f0.skyFreq1);
	CHECK(f0.skyPs == skyPsSet, "frame 1: pixel shader not restored");
	CHECK(st->instancedUpDraws == 0, "fullscreen passes drawn with instancing on");

	FrameResult f1 = frame(vp1);
	float expect[3][4];
	ReprojectionRows(vp0, vp1, W, H, expect);
	float worst = 0;
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 4; c++)
			worst = fmaxf(worst, fabsf(f1.rows[r][c] - expect[r][c]) / (1e-3f + fabsf(expect[r][c])));
	printf("frame 2: %s, s13 width %u, reprojection rows off by %.2g (relative)\n", describe(f1).c_str(), f1.opaqueS13, worst);
	CHECK(passesOk(f1, ours), "frame 2: %s, expected the shared prep once first, SSGI %d, AO %d (before SSGI)", describe(f1).c_str(), ours, ao);
	CHECK(f1.opaqueS13 == (W + 1) / 2, "frame 2: s13 should be the GI texture, is %u wide", f1.opaqueS13);
	CHECK(worst < 1e-3f, "frame 2: c216-c218 don't hold the reprojection rows");
	CHECK(f1.postS13 != (W + 1) / 2, "frame 2: the GI texture stays bound outside the scene colour");
	CHECK(f1.skyRt0 == colour && f1.skyRt1 == nullptr && f1.skyFreq1 == (D3DSTREAMSOURCE_INSTANCEDATA | 1), "frame 2: state not restored");

	// The menu (F3): polled at Present, so frames are drawn while a key is held. Hidden at start, it
	// ignores the arrows; shown, 1-0 / Up / Down select and Left / Right change settings, which take
	// effect at once and are saved to the ini a second later.
	auto menuKey = [&](int vk, const char* expect) {
		int before = E2E_LogCount(expect);
		g_keys[vk] = true;
		for (int i = 0; i < 100 && E2E_LogCount(expect) == before; i++)
			frame(vp1);
		g_keys[vk] = false;
		frame(vp1);
		CHECK(E2E_LogCount(expect) > before, "menu: no \"%s\" in the log after key 0x%02X", expect, vk);
	};
	auto tap = [&](int vk) { // a key that logs nothing (selection)
		g_keys[vk] = true;
		frame(vp1);
		g_keys[vk] = false;
		frame(vp1);
	};
	{
		int changes = E2E_LogCount("Menu: ");
		g_keys[VK_RIGHT] = true;
		frame(vp1);
		frame(vp1);
		g_keys[VK_RIGHT] = false;
		CHECK(E2E_LogCount("Menu: ") == changes, "menu: hidden, but it reacted to the Right arrow");
		menuKey(VK_F3, "SSGI menu shown");

		// "1 SSGI" off: black, no passes of ours. On again: black for a frame (no stale light), then GI.
		tap('1');
		int offs = E2E_LogCount("Menu: SSGI = OFF"), ons = E2E_LogCount("Menu: SSGI = ON");
		g_keys[VK_LEFT] = true;
		frame(vp1);
		g_keys[VK_LEFT] = false;
		CHECK(E2E_LogCount("Menu: SSGI = OFF") == offs + 1, "menu: SSGI not switched off");
		FrameResult f2 = frame(vp1);
		printf("SSGI off: %s, s13 width %u\n", describe(f2).c_str(), f2.opaqueS13);
		CHECK(passesOk(f2, 0) && f2.opaqueS13 == 1, "switched off: %s, s13 %u wide", describe(f2).c_str(), f2.opaqueS13);
		g_keys[VK_RIGHT] = true;
		frame(vp1);
		g_keys[VK_RIGHT] = false;
		CHECK(E2E_LogCount("Menu: SSGI = ON") == ons + 1, "menu: SSGI not switched on");
		FrameResult f3 = frame(vp1), f4 = frame(vp1);
		CHECK(f3.opaqueS13 == 1 && f4.opaqueS13 == (W + 1) / 2, "switched on: s13 %u then %u wide", f3.opaqueS13, f4.opaqueS13);

		// Debug view (Down to "2 Debug view"): one more pass, into the scene colour.
		tap(VK_DOWN);
		menuKey(VK_RIGHT, "Menu: Debug view = GI light");
		FrameResult fm = frame(vp1);
		printf("debug view: %s\n", describe(fm).c_str());
		CHECK(passesOk(fm, ours + 1), "menu: debug view on, %s", describe(fm).c_str());
		menuKey(VK_LEFT, "Menu: Debug view = off");
		tap('3'); // Intensity
		// One step up from what the ini holds (0.05 below 1, 0.25 from 1).
		char v[32] = {};
		GetPrivateProfileStringA("SSGI", "Intensity", "1.0", v, sizeof(v), E2E_INI);
		float start = (float)atof(v), want = start < 0.999f ? start + 0.05f : start + 0.25f;
		char expect[64];
		snprintf(expect, sizeof(expect), "Menu: Intensity = %.2f", want);
		menuKey(VK_RIGHT, expect);
		Sleep(1100);
		frame(vp1);
		GetPrivateProfileStringA("SSGI", "Intensity", "", v, sizeof(v), E2E_INI);
		printf("menu: intensity %.2f -> saved as \"%s\"\n", start, v);
		CHECK(fabsf((float)atof(v) - want) < 1e-3f, "menu: intensity saved as \"%s\", expected %.2f", v, want);
		menuKey(VK_F3, "SSGI menu hidden");
	}

	// Camera cut: last frame's GI isn't used.
	FrameResult f6 = frame(ViewProj(400, -50, 10));
	CHECK(f6.opaqueS13 == 1, "camera cut: s13 should be black, is %u wide", f6.opaqueS13);

	// Device reset: our targets are recreated, the game's are new objects.
	dev->Reset(&pp);
	IDirect3DTexture9 *colourTex2 = nullptr, *zwTex2 = nullptr;
	dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &colourTex2, nullptr);
	dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &zwTex2, nullptr);
	colourTex2->GetSurfaceLevel(0, &colour);
	zwTex2->GetSurfaceLevel(0, &zw);
	dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
	FrameResult f7 = frame(vp1), f8 = frame(vp1);
	printf("after Reset: %s, then %s; s13 %u then %u wide\n", describe(f7).c_str(), describe(f8).c_str(), f7.opaqueS13, f8.opaqueS13);
	CHECK(passesOk(f7, ours) && f7.opaqueS13 == 1 && f8.opaqueS13 == (W + 1) / 2, "after Reset");

	printf(g_fail ? "%d FAILURES\n" : "end-to-end checks passed\n", g_fail);
	return g_fail ? 1 : 0;
}
