// End-to-end test of the camera motion blur through the real hooks of AC2ShadowOverhaul.asi, on the
// mock d3d9.dll (tests/mockd3d).
//
// Built as AssassinsCreedIIGame.exe; loads scripts\AC2ShadowOverhaul.asi like Ultimate ASI Loader
// and plays AC2's frame sequence (depth prepass, shadow atlas, main pass with the z/w target as
// render target 1, transparents, glow into the back buffer, post-processing reading the scene
// colour, HUD), with the camera moving a little every frame. Checks:
//   - frame 1 (no camera to compare with): no motion blur passes
//   - from frame 2: right before the post draw (after TAA's output when TAA is on), prep, tile max,
//     neighbour max and the blur into the scene colour (colour channels only), the z/w target read
//   - the game's state at the post draw is its own again
//   - none while the camera stands still, on a camera cut, on the first frame after a Reset, or
//     while the all-effects key (F12) has everything off
//
// Usage: AssassinsCreedIIGame.exe [taa | off]
//   (default) motion blur alone
//   taa       with TAA: the passes come after TAA's resolve and output
//   off       [MotionBlur] StartEnabled=0: nothing is drawn
#include "e2e_common.h"
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <stdlib.h>

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

// Game-style camera constants: rows of VP * World and World, the camera at eyeX looking along +z.
static void CameraConstants(IDirect3DDevice9* dev, double eyeX)
{
	const double P00 = 1.317, P11 = 1.317 * 16 / 9, n = 0.1, f = 1779, A = f / (f - n), B = -n * f / (f - n);
	double V[4][4] = { { 1, 0, 0, -eyeX }, { 0, 1, 0, -15 }, { 0, 0, 1, -200 }, { 0, 0, 0, 1 } };
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

	struct Tags
	{
		size_t opaque, glow, post, hud;
	};
	Tags Frame(double eyeX)
	{
		Tags t;
		auto draw = [&]() {
			dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
			return log->draws.size() - 1;
		};
		dev->BeginScene();
		dev->SetVertexShader(sceneVs);
		dev->SetPixelShader(ps);
		CameraConstants(dev, eyeX);
		dev->SetRenderTarget(0, s8);
		draw();
		dev->SetRenderTarget(0, s4);
		draw();
		dev->SetRenderTarget(0, s2);
		dev->SetRenderTarget(1, s6);
		t.opaque = draw();
		draw();
		dev->SetRenderTarget(1, nullptr);
		draw();
		dev->SetRenderTarget(0, bb);
		t.glow = draw();
		dev->SetRenderTarget(0, s9);
		dev->SetVertexShader(fsVs);
		dev->SetPixelShader(postPs);
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

static bool g_taa = false, g_off = false;

int main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	g_taa = argc > 1 && !strcmp(argv[1], "taa");
	g_off = argc > 1 && !strcmp(argv[1], "off");
	printf("Motion blur end to end, %s\n", g_taa ? "with TAA" : g_off ? "switched off" : "alone");
	E2EEffects fx;
	fx.mb = true;
	fx.taa = g_taa;
	if (!E2E_WriteIni(fx))
		return 1;
	E2E_SetIni("MotionBlur", "StartEnabled", g_off ? "0" : "1"); // E2E_WriteIni leaves it off for the other suites
	HMODULE m = E2E_LoadPlugin();
	printf("loaded AC2ShadowOverhaul.asi: %s\n", m ? "ok" : "FAILED");
	if (!m)
		return 1;

	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
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
	std::vector<DWORD> scene = Compile(kShaders, "Scene", "vs_3_0"), fs = Compile(kShaders, "Fullscreen", "vs_3_0"),
					   psCode = Compile(kShaders, "Ps", "ps_3_0");
	g.dev->CreateVertexShader(scene.data(), &g.sceneVs);
	g.dev->CreateVertexShader(fs.data(), &g.fsVs);
	g.dev->CreatePixelShader(psCode.data(), &g.ps);
	g.dev->CreatePixelShader(psCode.data(), &g.postPs);

	auto checkFrame = [&](const Game::Tags& t, bool expectBlur, const char* what) {
		MockLog& L = *g.log;
		std::vector<size_t> passes;
		for (size_t i = t.opaque; i <= t.hud; i++)
			if (E2E_Effect(L.draws[i]) == FX_MB)
				passes.push_back(i);
		if (!expectBlur)
		{
			CHECK(passes.empty(), "%s: %zu motion blur passes (expected none)", what, passes.size());
			return;
		}
		CHECK(passes.size() == 4, "%s: %zu motion blur passes (expected 4)", what, passes.size());
		if (passes.size() != 4)
			return;
		CHECK(passes.front() > t.glow && passes.back() == t.post - 1, "%s: the passes aren't right before the post draw", what);
		const char* order[4] = { "mb-prep", "mb-tilemax", "mb-neighbormax", "mb-reconstruct" };
		for (int k = 0; k < 4; k++)
		{
			const char* n = E2E_PassName(L.draws[passes[k]]);
			CHECK(n && !strcmp(n, order[k]), "%s: pass %d is %s (expected %s)", what, k, n ? n : "?", order[k]);
		}
		CHECK(L.draws[passes[0]].tex[0] == g.t6, "%s: the prep pass doesn't read the z/w target", what);
		const MockDraw& rec = L.draws[passes.back()];
		CHECK(rec.rt[0] == g.s2 && rec.colorWrite == 7, "%s: the blur doesn't write the scene colour's colour channels", what);
		if (g_taa)
		{
			const char* before = E2E_PassName(L.draws[passes.front() - 1]);
			CHECK(before && !strcmp(before, "taa-output"), "%s: motion blur didn't follow TAA's output (%s)", what, before ? before : "a game draw");
		}
		const MockDraw& po = L.draws[t.post];
		CHECK(po.rt[0] == g.s9 && po.rt[1] == nullptr, "%s: render targets not restored", what);
		CHECK(po.tex[0] == g.t2, "%s: the scene texture binding not restored", what);
		CHECK(po.ps == g.postPs && po.vs == g.fsVs, "%s: shaders not restored", what);
		CHECK(po.colorWrite == 0xF, "%s: colour write mask not restored", what);
	};

	const bool on = !g_off;
	double eye = 93.5;
	printf("== frames, the camera moving\n");
	for (int f = 0; f < 5; f++)
	{
		eye += 0.05;
		char name[32];
		snprintf(name, sizeof(name), "frame %d", f + 1);
		checkFrame(g.Frame(eye), on && f > 0, name);
	}
	if (on)
		CHECK(E2E_LogCount("Motion blur: first frame blurred") == 1, "the first blurred frame wasn't logged");

	printf("== the camera standing still\n");
	checkFrame(g.Frame(eye), false, "still camera");
	eye += 0.05;
	checkFrame(g.Frame(eye), on, "moving again");

	printf("== a camera cut\n");
	eye += 40.0;
	checkFrame(g.Frame(eye), false, "camera cut");
	eye += 0.05;
	checkFrame(g.Frame(eye), on, "frame after the cut");

	printf("== Reset\n");
	CHECK(SUCCEEDED(g.dev->Reset(&pp)), "Reset failed");
	g.dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &g.bb);
	eye += 0.05;
	checkFrame(g.Frame(eye), false, "frame after Reset");
	eye += 0.05;
	checkFrame(g.Frame(eye), on, "second frame after Reset");

	printf("== the all-effects key (F12): off, then back\n");
	g_keys[VK_F12] = true;
	eye += 0.05;
	g.Frame(eye); // polled at this Present
	g_keys[VK_F12] = false;
	eye += 0.05;
	checkFrame(g.Frame(eye), false, "everything off (F12)");
	CHECK(E2E_LogCount("every effect off (vanilla)") == 1, "the all-effects key wasn't logged");
	g_keys[VK_F12] = true;
	eye += 0.05;
	g.Frame(eye);
	g_keys[VK_F12] = false;
	eye += 0.05;
	checkFrame(g.Frame(eye), on, "back on (F12)");
	CHECK(E2E_LogCount("every effect back as it was") == 1, "turning everything back on wasn't logged");

	CHECK(E2E_Log().find("Motion blur disabled") == std::string::npos, "motion blur disabled itself: see the log");
	printf(g_failures ? "FAILED (%d)\n" : "all passed\n", g_failures);
	return g_failures ? 1 : 0;
}
