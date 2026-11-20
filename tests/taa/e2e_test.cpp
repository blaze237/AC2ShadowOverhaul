// End-to-end test of TAA through the real hooks of AC2ShadowOverhaul.asi, on the mock d3d9.dll
// (tests/mockd3d).
//
// Built as AssassinsCreedIIGame.exe; loads scripts\AC2ShadowOverhaul.asi like Ultimate ASI Loader.
// Then it plays the frame sequence of AC2's real frame (from an F8 dump): depth prepass into a scene-sized
// texture, shadow atlas, main pass with the z/w target as render target 1, transparents, a glow
// pass into the back buffer, post-processing that reads the scene colour, HUD. Checks:
//   - the game's transforming vertex shader is created with the jitter, the full-screen one isn't
//   - c255 (the jitter) is set exactly on the scene draws, from the second frame on, changes every
//     frame and stays under a pixel; zero on the shadow map, back buffer, post and HUD draws
//   - the mip bias is applied during the scene and removed after
//   - the resolve runs right before the first draw that reads the scene colour, writes the scene
//     colour's colour channels only, and the game's state is back afterwards
//   - after a Reset, and after a frame that never reads its scene, the next frame isn't jittered
//
// Usage: AssassinsCreedIIGame.exe [all]
//   (default) TAA alone: ambient occlusion, contact shadows, SSGI and volumetrics Enabled=0
//   all       every effect on: the AO and SSGI passes run in the main pass too, before the resolve
#include "e2e_common.h"
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <algorithm>
#include <stdlib.h>
int Main();

static int g_failures = 0;
#define CHECK(cond, ...)                    \
	do                                      \
	{                                       \
		if (!(cond))                        \
		{                                   \
			printf("  FAIL: " __VA_ARGS__); \
			printf("\n");                   \
			g_failures++;                   \
		}                                   \
	} while (0)

typedef HRESULT(WINAPI* PFN_Compile)(const void*, SIZE_T, const char*, const D3D_SHADER_MACRO*, ID3DInclude*, const char*, const char*, UINT, UINT,
	ID3DBlob**, ID3DBlob**);

static std::vector<DWORD> Compile(const char* src, const char* entry, const char* profile)
{
	static PFN_Compile compile = (PFN_Compile)GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile");
	ID3DBlob *code = nullptr, *err = nullptr;
	if (!compile || FAILED(compile(src, strlen(src), "e2e", nullptr, nullptr, entry, profile, 0, 0, &code, &err)))
	{
		printf("compile %s failed: %s\n", entry, err ? (const char*)err->GetBufferPointer() : "no d3dcompiler_47");
		exit(2);
	}
	std::vector<DWORD> v((const DWORD*)code->GetBufferPointer(), (const DWORD*)code->GetBufferPointer() + code->GetBufferSize() / 4);
	code->Release();
	return v;
}

static const char* kShaders = R"(
float4x4 g_WorldViewProj : register(c0);
float4x4 g_World : register(c8);
struct SceneOut { float4 pos : POSITION; float4 world : TEXCOORD0; };
SceneOut Scene(float4 p : POSITION) { SceneOut o; o.pos = mul(p, g_WorldViewProj); o.world = mul(p, g_World); return o; }
float4 Fullscreen(float4 p : POSITION) : POSITION { return p; }
float4 Ps() : COLOR0 { return 1; }
)";

static bool UsesC255(const std::vector<DWORD>& code)
{
	for (size_t i = 1; i < code.size(); i++)
		if ((code[i] & 0x80000000u) && (code[i] & 0x7FF) == 255 && (((code[i] >> 28) & 7) | ((code[i] >> 8) & 0x18)) == 2)
			return true;
	return false;
}

// Game-style camera constants: rows of VP * World and World (registers = matrix rows).
static void CameraConstants(IDirect3DDevice9* dev, int frame)
{
	const double P00 = 1.317, P11 = 1.317 * 16 / 9, n = 0.1, f = 1779, A = f / (f - n), B = -n * f / (f - n);
	double yaw = 0.01 * frame, cy = cos(yaw), sy = sin(yaw);
	double eye[3] = { 93.5, 15.0, 211.7 + 0.05 * frame };
	double r[3] = { cy, 0, -sy }, u[3] = { 0, 1, 0 }, fw[3] = { sy, 0, cy };
	auto dot = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
	double V[4][4] = { { r[0], r[1], r[2], -dot(r, eye) }, { u[0], u[1], u[2], -dot(u, eye) }, { fw[0], fw[1], fw[2], -dot(fw, eye) }, { 0, 0, 0, 1 } };
	double Pm[4][4] = { { P00, 0, 0, 0 }, { 0, P11, 0, 0 }, { 0, 0, A, B }, { 0, 0, 1, 0 } };
	double Wm[4][4] = { { 1, 0, 0, 95 }, { 0, 1, 0, 12 }, { 0, 0, 1, 207 }, { 0, 0, 0, 1 } };
	double VP[4][4], WVP[4][4];
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
		{
			VP[i][j] = 0;
			for (int k = 0; k < 4; k++)
				VP[i][j] += Pm[i][k] * V[k][j];
		}
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
		{
			WVP[i][j] = 0;
			for (int k = 0; k < 4; k++)
				WVP[i][j] += VP[i][k] * Wm[k][j];
		}
	float c[8][4];
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
		{
			c[i][j] = (float)WVP[i][j];
			c[4 + i][j] = (float)Wm[i][j];
		}
	dev->SetVertexShaderConstantF(0, c[0], 4);
	dev->SetVertexShaderConstantF(8, c[4], 4);
}

struct Game
{
	IDirect3DDevice9* dev;
	IDirect3DTexture9 *t8, *t4, *t2, *t6, *t9;
	IDirect3DSurface9 *s8, *s4, *s2, *s6, *s9, *bb;
	IDirect3DVertexShader9 *sceneVs, *fsVs;
	IDirect3DPixelShader9 *ps, *postPs;
	MockLog* log;
	UINT W = 1920, H = 1080;

	void Create()
	{
		auto tex = [&](UINT w, UINT h, D3DFORMAT f, IDirect3DTexture9** t, IDirect3DSurface9** s) {
			dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, f, D3DPOOL_DEFAULT, t, nullptr);
			(*t)->GetSurfaceLevel(0, s);
		};
		tex(W, H, D3DFMT_A16B16G16R16F, &t8, &s8);
		tex(4096, 1024, D3DFMT_R32F, &t4, &s4);
		tex(W, H, D3DFMT_A16B16G16R16F, &t2, &s2);
		tex(W, H, D3DFMT_R32F, &t6, &s6);
		tex(W, H, D3DFMT_A16B16G16R16F, &t9, &s9);
		dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
	}

	// One frame; returns the draw-log index of each tagged game draw.
	struct Tags
	{
		size_t prepass, shadow, opaque, transparent, glow, post, hud;
	};
	Tags Frame(int f, bool readScene = true)
	{
		Tags t;
		auto draw = [&]() {
			dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
			return log->draws.size() - 1; // the game's draw is the last one (the resolve runs inside the call, before it)
		};
		dev->BeginScene();
		dev->SetVertexShader(sceneVs);
		dev->SetPixelShader(ps);
		CameraConstants(dev, f);
		dev->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, 0);
		dev->SetRenderTarget(0, s8);
		t.prepass = draw();
		dev->SetRenderTarget(0, s4);
		t.shadow = draw();
		dev->SetRenderTarget(0, s2);
		dev->SetRenderTarget(1, s6);
		t.opaque = draw();
		draw();
		dev->SetRenderTarget(1, nullptr);
		t.transparent = draw();
		dev->SetRenderTarget(0, bb);
		t.glow = draw();
		dev->SetRenderTarget(0, s9);
		dev->SetVertexShader(fsVs);
		dev->SetPixelShader(postPs);
		if (readScene)
			dev->SetTexture(0, t2);
		t.post = draw();
		dev->SetTexture(0, nullptr);
		dev->SetRenderTarget(0, bb);
		t.hud = draw();
		dev->EndScene();
		dev->Present(nullptr, nullptr, nullptr, nullptr);
		return t;
	}
};

static bool Zero(const float c[4]) { return c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 0; }

static bool g_all = false;

int main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	g_all = argc > 1 && !strcmp(argv[1], "all");
	return Main();
}

int Main()
{
	printf("TAA end to end, %s\n", g_all ? "with every effect on" : "TAA alone");
	E2EEffects fx;
	fx.taa = true;
	if (g_all)
		fx = E2EEffects::All();
	if (!E2E_WriteIni(fx))
		return 1;
	HMODULE m = E2E_LoadPlugin();
	printf("loaded AC2ShadowOverhaul.asi: %s\n", m ? "ok" : "FAILED");
	if (!m)
		return 1;
	const bool withSo = g_all;

	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION); // through the game exe's import, as the plugins patched it
	D3DPRESENT_PARAMETERS pp = {};
	pp.BackBufferWidth = 1920;
	pp.BackBufferHeight = 1080;
	pp.BackBufferFormat = D3DFMT_X8R8G8B8;
	pp.Windowed = TRUE;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.EnableAutoDepthStencil = TRUE;
	pp.AutoDepthStencilFormat = D3DFMT_D24S8;
	Game g;
	if (FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, g_wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g.dev)))
		return printf("CreateDevice failed\n"), 1;
	PFN_MockGetLog getLog = MockExport<PFN_MockGetLog>("MockGetLog");
	g.log = getLog ? getLog() : nullptr;
	if (!g.log)
		return printf("not the mock d3d9.dll\n"), 1;
	g.Create();

	printf("== shaders\n");
	std::vector<DWORD> scene = Compile(kShaders, "Scene", "vs_3_0"), fs = Compile(kShaders, "Fullscreen", "vs_3_0");
	size_t before = g.log->vsCode.size();
	g.dev->CreateVertexShader(scene.data(), &g.sceneVs);
	g.dev->CreateVertexShader(fs.data(), &g.fsVs);
	std::vector<DWORD> psCode = Compile(kShaders, "Ps", "ps_3_0");
	g.dev->CreatePixelShader(psCode.data(), &g.ps);
	g.dev->CreatePixelShader(psCode.data(), &g.postPs);
	CHECK(g.log->vsCode.size() >= before + 2, "vertex shaders not created");
	if (g.log->vsCode.size() >= before + 2)
	{
		CHECK(UsesC255(g.log->vsCode[before]), "the scene vertex shader reached the driver without the jitter");
		CHECK(!UsesC255(g.log->vsCode[before + 1]), "the full-screen vertex shader was jittered");
	}

	printf("== frames%s\n", withSo ? " (with every effect on)" : "");
	const float px = 2.0f / 1920; // one pixel in NDC
	float lastJitter[2] = { 0, 0 };
	float bias = -0.5f;
	DWORD biasBits;
	memcpy(&biasBits, &bias, 4);
	auto checkFrame = [&](const Game::Tags& t, bool expectJitter, const char* what) {
		MockLog& L = *g.log;
		const MockDraw &pre = L.draws[t.prepass], &sh = L.draws[t.shadow], &op = L.draws[t.opaque], &tr = L.draws[t.transparent],
					   &gl = L.draws[t.glow], &po = L.draws[t.post], &hu = L.draws[t.hud];
		if (expectJitter)
		{
			CHECK(!Zero(op.c255), "%s: main pass not jittered", what);
			CHECK(memcmp(pre.c255, op.c255, 16) == 0 && memcmp(tr.c255, op.c255, 16) == 0, "%s: prepass / transparents jittered differently", what);
			CHECK(fabsf(op.c255[0]) <= 0.5f * px * 1.001f && fabsf(op.c255[1]) <= 0.5f * px * 16 / 9 * 1.001f && op.c255[2] == 0 && op.c255[3] == 0,
				"%s: jitter out of range (%g, %g)", what, op.c255[0], op.c255[1]);
			CHECK(op.c255[0] != lastJitter[0] || op.c255[1] != lastJitter[1], "%s: same jitter as the frame before", what);
			CHECK(op.lodBias0 == biasBits, "%s: mip bias not applied in the main pass (0x%08lX)", what, op.lodBias0);
			lastJitter[0] = op.c255[0];
			lastJitter[1] = op.c255[1];
		}
		else
			CHECK(Zero(op.c255) && Zero(pre.c255), "%s: jittered although it shouldn't be", what);
		CHECK(Zero(sh.c255), "%s: shadow map jittered", what);
		CHECK(Zero(gl.c255) && Zero(po.c255) && Zero(hu.c255), "%s: back buffer / post / HUD jittered", what);
		CHECK(po.lodBias0 == 0 && hu.lodBias0 == 0, "%s: mip bias left on after the scene", what);
		// The resolve: between the glow pass and the post draw, a draw into the scene colour writing
		// colour only (the output pass), right after one into another target (the resolve).
		// Our output pass: colour-only into the scene colour, right after a draw into a full-size
		// target of the plugin's own (the history). (AC2ShadowOverhaul's AO composite also writes the
		// scene colour colour-only, after the opaque pass, but after its half-size AO targets.)
		auto isOutput = [&](size_t i) {
			if (i == 0 || L.draws[i].rt[0] != g.s2 || L.draws[i].colorWrite != 7)
				return false;
			IDirect3DSurface9* prev = L.draws[i - 1].rt[0];
			D3DSURFACE_DESC d = {};
			prev->GetDesc(&d);
			return prev != g.s2 && prev != g.s8 && prev != g.s9 && prev != g.bb && d.Width == g.W && d.Height == g.H;
		};
		int outputs = 0, aoComposites = 0;
		for (size_t i = t.glow + 1; i < t.post; i++)
			outputs += isOutput(i);
		CHECK(outputs == 1, "%s: %d resolves before the post draw (expected 1)", what, outputs);
		for (size_t i = t.prepass; i < t.glow; i++)
		{
			CHECK(!isOutput(i), "%s: resolved too early (draw %zu)", what, i - t.prepass);
			aoComposites += L.draws[i].rt[0] == g.s2 && L.draws[i].colorWrite == 7;
		}
		if (aoComposites && !strcmp(what, "frame 2"))
			printf("  the AO composite ran in the main pass too (%d draw)\n", aoComposites);
		// The same, by the passes' shaders: exactly one resolve then one output, between the glow pass
		// and the post draw. With every effect on, the AO (4) and SSGI (5) passes run at the end of
		// the main pass (before the transparents), AO first; nothing else draws.
		E2EPassCount early = E2E_CountPasses(&L, t.prepass, t.glow), late = E2E_CountPasses(&L, t.glow + 1, t.post);
		CHECK(early.n[FX_TAA] == 0 && late.n[FX_TAA] == 2 && t.post >= 2 && E2E_PassName(L.draws[t.post - 2]) &&
				!strcmp(E2E_PassName(L.draws[t.post - 2]), "taa-resolve") && E2E_PassName(L.draws[t.post - 1]) &&
				!strcmp(E2E_PassName(L.draws[t.post - 1]), "taa-output"),
			"%s: expected the resolve and output passes right before the post draw (%d before the glow pass, %d after)", what, early.n[FX_TAA],
			late.n[FX_TAA]);
		CHECK(late.Effects() == late.n[FX_TAA] && E2E_CountPasses(&L, t.post, t.hud + 1).Effects() == 0, "%s: other passes after the main pass", what);
		E2EPassCount main = E2E_CountPasses(&L, t.opaque, t.transparent);
		int wantAo = g_all ? 4 : 0, wantGi = g_all ? 5 : 0;
		CHECK(main.n[FX_AO] == wantAo && main.n[FX_SSGI] == wantGi && early.Effects() == wantAo + wantGi,
			"%s: %d AO and %d SSGI passes at the end of the main pass (expected %d and %d), %d effect passes before the glow pass", what,
			main.n[FX_AO], main.n[FX_SSGI], wantAo, wantGi, early.Effects());
		if (g_all)
		{
			size_t lastAo = 0, firstGi = (size_t)-1;
			for (size_t i = t.opaque; i < t.transparent; i++)
			{
				E2EEffect e = E2E_Effect(L.draws[i]);
				if (e == FX_AO)
					lastAo = i;
				if (e == FX_SSGI && firstGi == (size_t)-1)
					firstGi = i;
			}
			CHECK(lastAo < firstGi, "%s: SSGI ran before the AO", what);
		}
		// The game's state at its post draw is its own again.
		CHECK(po.rt[0] == g.s9 && po.rt[1] == nullptr, "%s: render targets not restored", what);
		CHECK(po.tex[0] == g.t2, "%s: the scene texture binding not restored", what);
		CHECK(po.ps == g.postPs && po.vs == g.fsVs, "%s: shaders not restored", what);
		CHECK(po.colorWrite == 0xF, "%s: colour write mask not restored", what);
	};

	Game::Tags t = g.Frame(0);
	checkFrame(t, false, "frame 1 (scene size not known yet)");
	for (int f = 1; f < 6; f++)
	{
		char name[32];
		snprintf(name, sizeof(name), "frame %d", f + 1);
		t = g.Frame(f);
		checkFrame(t, true, name);
	}

	printf("== a frame that never reads its scene (a menu), then normal frames\n");
	t = g.Frame(6, false);
	t = g.Frame(7);
	checkFrame(t, false, "frame after the menu frame");
	t = g.Frame(8);
	checkFrame(t, true, "second frame after the menu frame");

	printf("== Reset\n");
	int resets = g.log->resets;
	CHECK(SUCCEEDED(g.dev->Reset(&pp)), "Reset failed");
	CHECK(g.log->resets == resets + 1, "Reset didn't reach the driver");
	g.dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &g.bb);
	t = g.Frame(9);
	checkFrame(t, false, "frame after Reset");
	t = g.Frame(10);
	checkFrame(t, true, "second frame after Reset");

	printf(g_failures ? "FAILED (%d)\n" : "all passed\n", g_failures);
	return g_failures ? 1 : 0;
}
