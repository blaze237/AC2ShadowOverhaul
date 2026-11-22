// Combined end-to-end test: every effect of AC2ShadowOverhaul.asi on at once, through its one set of
// Direct3D hooks. Built as combined/AssassinsCreedIIGame.exe next to the mock d3d9.dll
// (tests/mockd3d); statically imports d3d9!Direct3DCreate9 like the game and loads
// scripts\AC2ShadowOverhaul.asi like Ultimate ASI Loader. Frames are shaped like AC2's: sun shadow
// atlas, opaque pass (scene colour + R32F z/w as RT1, two batches), the sky (viewport depth 1..1),
// a transparent draw, a bloom draw into a target of its own, then post-processing that reads the
// scene colour into the back buffer, Present.
//
// Usage: AssassinsCreedIIGame.exe <mode>
//   order    the frame order: ambient occlusion, then SSGI at the end of the opaque pass (before the
//            sky draw), the volumetrics after the sky (before the transparents), the TAA resolve
//            right before the first draw that reads the scene colour; nothing else is drawn. The
//            geometry prep AO and SSGI share runs once, before both; SSGI's radiance pass reads the
//            scene colour after the AO / contact shadow composite wrote it; contact shadows and the
//            volumetrics use the same sun
//   menus    only one menu is open at a time: F9 (shadows) then F3 (SSGI) hides the F9 menu, and so
//            on through F1 (TAA) and F11 (volumetrics); the arrows only reach the menu shown last
//   msaa     a multisampled scene colour and z/w target: the MSAA warning is logged, no effect draws
//            anything, and the game's draws never get TAA's jitter (c255 stays 0)
//   taa      the effects with TAA (core/scene.h): every frame, the projection terms AO, SSGI and the
//            volumetrics get (c3.xy of their passes) are the unjittered ones plus exactly the c255
//            the game's draws got that frame; the fog's noise offset (c24.w) steps through the
//            golden ratio sequence only while TAA runs, AO's (c3.zw) stays 0 and AO's blur and SSGI's
//            blend stay as without TAA; every effect pass runs with c255 = 0, no mip bias, and its own
//            vertex shader. Switched off in the F1 menu: no jitter, offsets 0, everything as without
//            TAA; switched back on: it all comes back
//   taaoff   [TAA] Enabled=0: no jitter, offsets 0, the passes as without TAA
//   noassist [TAA] AssistEffects=0: the jitter is still folded in (that is correctness), but the
//            noise offsets stay 0, the wide AO blur and SSGI's own blend stay
//   refusal  a dummy AC2SSGI.asi next to the plugin: it logs the refusal and installs no hooks (the
//            message box it shows on its own thread is closed by the test, if it appears)
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include "e2e_common.h"

static int g_fails = 0;
static void Check(bool ok, const char* fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	printf("%s  ", ok ? "PASS" : "FAIL");
	vprintf(fmt, a);
	printf("\n");
	va_end(a);
	if (!ok)
		g_fails++;
}

// ---- Shaders like the game's (only their constant tables matter to the plugin) --------------------

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
static std::vector<DWORD> Compile(const char* src, const char* target)
{
	static HMODULE c = LoadLibraryA("D3DCompiler_47.dll");
	auto compile = c ? (PFN_Compile)GetProcAddress(c, "D3DCompile") : nullptr;
	Blob *code = nullptr, *err = nullptr;
	if (!compile || FAILED(compile(src, strlen(src), "test", nullptr, nullptr, "main", target, 0, 0, &code, &err)))
	{
		printf("FAIL  compile failed: %s\n", err ? (const char*)err->vt->Ptr(err) : "no compiler");
		exit(1);
	}
	const DWORD* p = (const DWORD*)code->vt->Ptr(code);
	return std::vector<DWORD>(p, p + code->vt->Size(code) / 4);
}

static const char* kGameVs = R"(
float4x4 g_WorldViewProj : register(c0);
float4x4 g_World : register(c8);
float4 g_EyePosition : register(c12);
float4 g_WorldToLightProjVS[3] : register(c84);
struct VO { float4 pos : POSITION; float4 t0 : TEXCOORD0; };
VO main(float4 p : POSITION) {
	VO o;
	o.pos = mul(p, g_WorldViewProj);
	float4 w = mul(p, g_World);
	o.t0 = float4(dot(w, g_WorldToLightProjVS[0]), dot(w, g_WorldToLightProjVS[1]), dot(w, g_WorldToLightProjVS[2]), g_EyePosition.x);
	return o;
})";

static const char* kLitPs = R"(
struct SD { float3 m_Direction; float3 m_Color; float3 m_SpecularDirection; };
SD g_ShadowedDirect : register(c52);
float4 g_FogColor : register(c16);
float4 g_FogParams : register(c17);
float4 g_ShadowCascadesRangesMin : register(c64);
float4 g_ShadowCascadesRangesMax : register(c65);
float4 g_LightDirShadowBiasPerCascade : register(c72);
float4 g_ShadowFadeParams : register(c74);
float4 g_ShadowNearFar : register(c75);
float4 g_ShadowScalesX : register(c82);
float4 g_ShadowScalesY : register(c83);
float4 g_ShadowOffsetsX : register(c84);
float4 g_ShadowOffsetsY : register(c85);
float4 g_ShadowMapSize : register(c86);
float4 g_CloudUVScaleOffset : register(c115);
sampler2D g_CloudShadowSampler : register(s14);
sampler2D g_ShadowSampler : register(s15);
struct PO { float4 c0 : COLOR0; float4 c1 : COLOR1; };
PO main(float4 t : TEXCOORD0) {
	float4 s = tex2D(g_ShadowSampler, t.xy * g_ShadowScalesX.xy + g_ShadowOffsetsX.xy + t.zw * g_ShadowScalesY.xy + g_ShadowOffsetsY.xy);
	s += tex2D(g_CloudShadowSampler, t.xy * g_CloudUVScaleOffset.xy + g_CloudUVScaleOffset.zw);
	float4 acc = g_FogColor + g_FogParams + g_ShadowCascadesRangesMin * t.x + g_ShadowCascadesRangesMax * t.y + g_LightDirShadowBiasPerCascade * t.z +
		g_ShadowFadeParams * t.w + g_ShadowNearFar + g_ShadowMapSize;
	acc.xyz += g_ShadowedDirect.m_Direction * g_ShadowedDirect.m_Color;
	PO o;
	o.c0 = acc * s;
	o.c1 = t.z / t.w;
	return o;
})";
static const char* kOtherPs = "float4 k : register(c0); float4 main(float4 t : TEXCOORD0) : COLOR { return t * k; }";
static const char* kCasterPs = "float4 main(float4 t : TEXCOORD0) : COLOR { return t.z; }";

// ---- Camera (as tests/vol/e2e_test.cpp: the sun is high, the camera looks up towards it) -------------

struct V3
{
	float x, y, z;
};
static V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 Norm(V3 a)
{
	float l = sqrtf(Dot(a, a));
	return { a.x / l, a.y / l, a.z / l };
}
static void Mul(const float a[16], const float b[16], float out[16])
{
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++)
		{
			double s = 0;
			for (int k = 0; k < 4; k++)
				s += (double)a[r * 4 + k] * b[k * 4 + c];
			out[r * 4 + c] = (float)s;
		}
}

// ---- The game ------------------------------------------------------------------------------------

struct Game
{
	IDirect3DDevice9* dev = nullptr;
	MockLog* log = nullptr;
	UINT W = 640, H = 360;
	bool msaa = false; // the scene is drawn into multisampled targets and resolved with StretchRect
	IDirect3DTexture9 *sceneTex = nullptr, *zwTex = nullptr, *atlasTex = nullptr, *cloudTex = nullptr, *bloomTex = nullptr;
	IDirect3DSurface9 *sceneSurf = nullptr, *zwSurf = nullptr, *atlas = nullptr, *bloom = nullptr, *bb = nullptr;
	IDirect3DSurface9 *scene = nullptr, *zw = nullptr; // the targets the opaque pass draws into
	IDirect3DVertexShader9* gameVs = nullptr;
	IDirect3DPixelShader9 *litPs = nullptr, *skyPs = nullptr, *transPs = nullptr, *bloomPs = nullptr, *postPs = nullptr, *casterPs = nullptr;

	// Indices (in the draw log) of the game's draws in the last frame.
	struct Tags
	{
		size_t begin, shadow, firstOpaque, lastOpaque, sky, trans, bloom, post, end;
	};

	bool Create()
	{
		IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
		D3DPRESENT_PARAMETERS pp = {};
		pp.BackBufferWidth = W;
		pp.BackBufferHeight = H;
		pp.BackBufferFormat = D3DFMT_X8R8G8B8;
		pp.Windowed = TRUE;
		pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
		pp.EnableAutoDepthStencil = TRUE;
		pp.AutoDepthStencilFormat = D3DFMT_D24S8;
		if (!d3d || FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, g_wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)) || !dev)
			return false;
		PFN_MockGetLog getLog = MockExport<PFN_MockGetLog>("MockGetLog");
		log = getLog ? getLog() : nullptr;
		if (!log)
			return false;

		dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &sceneTex, nullptr);
		dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &zwTex, nullptr);
		dev->CreateTexture(4096, 1024, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &atlasTex, nullptr);
		dev->CreateTexture(256, 256, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cloudTex, nullptr);
		dev->CreateTexture(W / 2, H / 2, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &bloomTex, nullptr);
		sceneTex->GetSurfaceLevel(0, &sceneSurf);
		zwTex->GetSurfaceLevel(0, &zwSurf);
		atlasTex->GetSurfaceLevel(0, &atlas);
		bloomTex->GetSurfaceLevel(0, &bloom);
		dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
		scene = sceneSurf;
		zw = zwSurf;
		if (msaa)
		{
			dev->CreateRenderTarget(W, H, D3DFMT_A16B16G16R16F, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &scene, nullptr);
			dev->CreateRenderTarget(W, H, D3DFMT_R32F, D3DMULTISAMPLE_4_SAMPLES, 0, FALSE, &zw, nullptr);
		}

		auto vsCode = Compile(kGameVs, "vs_3_0");
		auto litCode = Compile(kLitPs, "ps_3_0");
		auto otherCode = Compile(kOtherPs, "ps_3_0");
		auto casterCode = Compile(kCasterPs, "ps_3_0");
		dev->CreateVertexShader(vsCode.data(), &gameVs);
		dev->CreatePixelShader(litCode.data(), &litPs);
		dev->CreatePixelShader(otherCode.data(), &skyPs);
		dev->CreatePixelShader(otherCode.data(), &transPs);
		dev->CreatePixelShader(otherCode.data(), &bloomPs);
		dev->CreatePixelShader(otherCode.data(), &postPs);
		dev->CreatePixelShader(casterCode.data(), &casterPs);

		const float nearZ = 0.1f, farZ = 1779.0f;
		const float A = farZ / (farZ - nearZ), B = -nearZ * farZ / (farZ - nearZ), P00 = 1.317f, P11 = 1.317f * 16.0f / 9.0f;
		const V3 eye = { 93.5417f, 211.7695f, 15.05303f };
		const V3 sunRaw = { -0.2789036f, -0.434417f, 0.856443f };
		const V3 sun = Norm(sunRaw);
		V3 flat = Norm(V3{ sun.x, sun.y, 0 });
		const float pitch = 0.75f;
		const V3 f = { flat.x * cosf(pitch), flat.y * cosf(pitch), sinf(pitch) };
		const V3 r = Norm(Cross(f, V3{ 0, 0, 1 }));
		const V3 u = Cross(r, f);
		const float view[16] = { r.x, r.y, r.z, -Dot(r, eye), u.x, u.y, u.z, -Dot(u, eye), f.x, f.y, f.z, -Dot(f, eye), 0, 0, 0, 1 };
		const float proj[16] = { P00, 0, 0, 0, 0, P11, 0, 0, 0, 0, A, B, 0, 0, 1, 0 };
		const float world[16] = { 0, -1, 0, 94.9995f, 1, 0, 0, 207.0036f, 0, 0, 1, 11.99986f, 0, 0, 0, 1 };
		float vp[16], wvp[16];
		Mul(proj, view, vp);
		Mul(vp, world, wvp);
		const float light[12] = { 0.8418254f, -0.5397499f, 0, 32.35132f, 0.4623303f, 0.7210772f, 0.516041f, -200.695f, -0.2785331f, -0.4344164f,
			0.8565639f, 104.8968f };
		const float eyeC[4] = { eye.x, eye.y, eye.z, 0 };
		dev->SetVertexShaderConstantF(0, wvp, 4);
		dev->SetVertexShaderConstantF(8, world, 4);
		dev->SetVertexShaderConstantF(12, eyeC, 1);
		dev->SetVertexShaderConstantF(84, light, 3);
		const float sunC[8] = { sunRaw.x, sunRaw.y, sunRaw.z, 0, 1.317883f, 1.111156f, 0.8398271f, 0 };
		const float fogColor[4] = { 0.549f, 0.592f, 0.647f, 1 }, fogParams[4] = { 10.94f, 0.0101f, 0.596f, 0 };
		const float rMin[4] = { 384, 96, 24, 0 }, rMax[4] = { 2400, 384, 96, 24 };
		const float bias[4] = { -0.00033f, -5.6e-05f, -1.25e-05f, -2.8e-06f }, fade[4] = { -2220, 0.005747f, 0, 0 },
					nearFar[4] = { -0.0002777778f, 0.3333333f, 0, 0 };
		const float scX[4] = { 5.198e-05f, 0.000325f, 0.0013f, 0.0052f }, scY[4] = { -0.000208f, -0.0013f, -0.0052f, -0.0208f };
		const float ofX[4] = { 0.125f, 0.375f, 0.625f, 0.875f }, ofY[4] = { 0.5f, 0.5f, 0.5f, 0.5f }, mapSize[4] = { 4096, 1024, 0, 0 };
		const float cloudUV[4] = { 0.001f, 0.001f, 0.25f, 0.5f };
		dev->SetPixelShaderConstantF(52, sunC, 2);
		dev->SetPixelShaderConstantF(16, fogColor, 1);
		dev->SetPixelShaderConstantF(17, fogParams, 1);
		dev->SetPixelShaderConstantF(64, rMin, 1);
		dev->SetPixelShaderConstantF(65, rMax, 1);
		dev->SetPixelShaderConstantF(72, bias, 1);
		dev->SetPixelShaderConstantF(74, fade, 1);
		dev->SetPixelShaderConstantF(75, nearFar, 1);
		dev->SetPixelShaderConstantF(82, scX, 1);
		dev->SetPixelShaderConstantF(83, scY, 1);
		dev->SetPixelShaderConstantF(84, ofX, 1);
		dev->SetPixelShaderConstantF(85, ofY, 1);
		dev->SetPixelShaderConstantF(86, mapSize, 1);
		dev->SetPixelShaderConstantF(115, cloudUV, 1);
		return true;
	}

	Tags Frame()
	{
		Tags t = {};
		auto draw = [&]() {
			dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
			return log->draws.size() - 1; // the game's draw comes after any passes the hooks run before it
		};
		t.begin = log->draws.size();
		dev->BeginScene();
		dev->SetVertexShader(gameVs);
		// Sun shadow atlas.
		dev->SetRenderTarget(0, atlas);
		dev->SetPixelShader(casterPs);
		t.shadow = draw();
		// Opaque pass in two batches.
		dev->SetRenderTarget(0, scene);
		D3DVIEWPORT9 vpScene = { 0, 0, W, H, 0.0f, 0.9999f };
		dev->SetViewport(&vpScene);
		dev->SetTexture(14, cloudTex);
		dev->SetTexture(15, atlasTex);
		dev->SetRenderTarget(1, zw);
		dev->SetPixelShader(litPs);
		t.firstOpaque = draw();
		for (int i = 0; i < 2; i++)
			draw();
		dev->SetRenderTarget(1, nullptr);
		dev->SetRenderTarget(1, zw);
		dev->SetPixelShader(litPs);
		t.lastOpaque = draw();
		dev->SetRenderTarget(1, nullptr);
		// Sky.
		D3DVIEWPORT9 vpSky = { 0, 0, W, H, 1.0f, 1.0f };
		dev->SetViewport(&vpSky);
		dev->SetPixelShader(skyPs);
		t.sky = draw();
		// Transparents.
		dev->SetViewport(&vpScene);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		dev->SetPixelShader(transPs);
		t.trans = draw();
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		if (msaa)
			dev->StretchRect(scene, nullptr, sceneSurf, nullptr, D3DTEXF_NONE); // resolve the multisampled scene
		// Bloom into a target of its own, without reading the scene colour.
		dev->SetRenderTarget(0, bloom);
		dev->SetPixelShader(bloomPs);
		t.bloom = draw();
		// Post-processing: the first read of the scene colour.
		dev->SetRenderTarget(0, bb);
		dev->SetTexture(0, sceneTex);
		dev->SetTexture(1, bloomTex);
		dev->SetPixelShader(postPs);
		t.post = draw();
		dev->SetTexture(0, nullptr);
		dev->SetTexture(1, nullptr);
		dev->EndScene();
		dev->Present(nullptr, nullptr, nullptr, nullptr);
		t.end = log->draws.size();
		return t;
	}
};

// `ini` (section, key, value triples, nullptr-terminated) is written over the shipped settings.
static bool Setup(Game& g, bool msaa, const char* const* ini = nullptr)
{
	if (!E2E_WriteIni(E2EEffects::All()))
		return false;
	for (; ini && ini[0]; ini += 3)
		E2E_SetIni(ini[0], ini[1], ini[2]);
	if (!E2E_LoadPlugin())
		return false;
	g.msaa = msaa;
	if (!g.Create())
	{
		printf("FAIL  could not create the (mock) device\n");
		return false;
	}
	return true;
}

// ---- order ---------------------------------------------------------------------------------------

static int TestOrder()
{
	Game g;
	if (!Setup(g, false))
		return 1;
	Game::Tags t = {};
	for (int i = 0; i < 4; i++)
		t = g.Frame();
	const MockLog& L = *g.log;

	// Where each effect drew in the last frame.
	std::vector<size_t> at[FX_COUNT];
	std::string sequence;
	for (size_t i = t.begin; i < t.end; i++)
	{
		E2EEffect e = E2E_Effect(L.draws[i]);
		at[e].push_back(i);
		if (e != FX_NONE)
			sequence += std::string(sequence.empty() ? "" : " ") + E2E_PassName(L.draws[i]);
	}
	printf("last frame: %zu draws; opaque %zu-%zu, sky %zu, transparent %zu, bloom %zu, post %zu\n", t.end - t.begin, t.firstOpaque - t.begin,
		t.lastOpaque - t.begin, t.sky - t.begin, t.trans - t.begin, t.bloom - t.begin, t.post - t.begin);
	printf("passes: %s\n", sequence.c_str());
	auto between = [](const std::vector<size_t>& v, size_t after, size_t before) {
		bool ok = !v.empty();
		for (size_t i : v)
			ok &= i > after && i < before;
		return ok;
	};
	// The game's draws are all there, and nothing but them and the effects' passes.
	size_t game = t.end - t.begin - at[FX_SHARED].size() - at[FX_AO].size() - at[FX_SSGI].size() - at[FX_VOL].size() - at[FX_TAA].size();
	Check(game == 9, "the game's 9 draws are all there (%zu)", game);
	Check(at[FX_SHARED].size() == 1 && at[FX_AO].size() == 4 && at[FX_SSGI].size() == 5 && at[FX_VOL].size() == 7 && at[FX_TAA].size() == 2,
		"passes: shared geometry prep 1, AO 4, SSGI 5, volumetrics 7, TAA 2 (got %zu, %zu, %zu, %zu, %zu)", at[FX_SHARED].size(),
		at[FX_AO].size(), at[FX_SSGI].size(), at[FX_VOL].size(), at[FX_TAA].size());
	Check(between(at[FX_SHARED], t.lastOpaque, t.sky) && !at[FX_AO].empty() && at[FX_SHARED].front() < at[FX_AO].front(),
		"the geometry prep AO and SSGI share: once, after the opaque pass, before both");
	Check(between(at[FX_AO], t.lastOpaque, t.sky), "ambient occlusion: after the opaque pass, before the sky");
	Check(between(at[FX_SSGI], t.lastOpaque, t.sky) && !at[FX_AO].empty() && at[FX_SSGI].front() > at[FX_AO].back(),
		"SSGI: after the ambient occlusion (it traces the AO'd image), before the sky");
	Check(between(at[FX_VOL], t.sky, t.trans), "volumetrics: after the sky, before the transparents");
	Check(at[FX_TAA].size() == 2 && at[FX_TAA][0] == t.post - 2 && at[FX_TAA][1] == t.post - 1 && between(at[FX_TAA], t.bloom, t.post),
		"TAA: resolve and output right before the post draw (the first that reads the scene colour), after the bloom draw");
	// The first draw that reads the scene colour is the post draw, and every pass of every effect is
	// before it (so all of them are anti-aliased).
	size_t firstRead = t.end;
	for (size_t i = t.begin; i < t.end && firstRead == t.end; i++)
		for (IDirect3DBaseTexture9* tex : L.draws[i].tex)
			if (tex == g.sceneTex && E2E_Effect(L.draws[i]) == FX_NONE)
				firstRead = i;
	Check(firstRead == t.post, "the first game draw that reads the scene colour is the post draw (%zu)", firstRead - t.begin);
	bool allBefore = true;
	for (int e = FX_SHARED; e < FX_COUNT; e++)
		for (size_t i : at[e])
			allBefore &= i < firstRead;
	Check(allBefore, "every effect pass comes before the first read of the scene colour");
	// The game's own state at its draws after the passes.
	const MockDraw &sky = L.draws[t.sky], &trans = L.draws[t.trans], &post = L.draws[t.post];
	Check(sky.ps == g.skyPs && sky.rt[0] == g.scene && sky.rt[1] == nullptr && sky.vp.MinZ == 1.0f, "sky draw: the game's shader, targets and viewport");
	Check(trans.ps == g.transPs && trans.rt[0] == g.scene && trans.vp.MinZ == 0.0f && fabsf(trans.vp.MaxZ - 0.9999f) < 1e-6f &&
			trans.alphaBlend == TRUE && trans.srcBlend == D3DBLEND_SRCALPHA && trans.destBlend == D3DBLEND_INVSRCALPHA,
		"transparent draw: the game's shader, target, viewport and blending");
	Check(post.ps == g.postPs && post.rt[0] == g.bb && post.tex[0] == g.sceneTex && post.tex[1] == g.bloomTex && post.colorWrite == 0xF,
		"post draw: the game's shader, target, textures and colour mask");
	Check(E2E_LogCount("(MSAA) is on") == 0 && E2E_LogCount("multisampled") == 0, "no MSAA warning");

	// SSGI's bounce comes from the occluded light: its radiance pass (the only SSGI pass that reads the
	// lit colour) samples the scene colour after the AO composite (AO and contact shadows) has drawn
	// into it, and nothing draws into the scene colour in between.
	{
		size_t composite = 0, radiance = 0;
		for (size_t i = t.begin; i < t.end; i++)
		{
			const char* name = E2E_PassName(L.draws[i]);
			if (name && !strcmp(name, "ao-composite") && L.draws[i].rt[0] == g.scene)
				composite = i;
			if (name && !strcmp(name, "gi-radiance"))
				radiance = i;
		}
		bool ok = composite && radiance > composite;
		bool reads = false;
		for (IDirect3DBaseTexture9* tex : radiance ? L.draws[radiance].tex : L.draws[t.begin].tex)
			reads |= radiance && tex == g.sceneTex;
		for (size_t i = composite + 1; ok && i < radiance; i++)
			ok &= L.draws[i].rt[0] != g.scene;
		Check(ok && reads, "SSGI reads the scene colour (its radiance pass) after the AO / contact shadow composite wrote it");
	}

	// One sun for everything (core/scene.h reads g_ShadowedDirect once): the contact shadows' view-space
	// direction (GTAO pass, c6) is the volumetrics' world direction (march pass, c21) seen through the
	// volumetrics' camera axes (c5-c7), and both are the game's g_ShadowedDirect, normalised.
	const MockDraw *gtao = nullptr, *march = nullptr;
	for (size_t i = t.begin; i < t.end; i++)
	{
		const char* name = E2E_PassName(L.draws[i]);
		if (name && !strcmp(name, "gtao"))
			gtao = &L.draws[i];
		if (name && !strcmp(name, "march"))
			march = &L.draws[i];
	}
	bool oneSun = gtao && march;
	if (oneSun)
	{
		const V3 sun = Norm(V3{ -0.2789036f, -0.434417f, 0.856443f }); // as Game::Create sets it
		const float* w = march->psConst[21];
		oneSun = fabsf(w[0] - sun.x) < 1e-5f && fabsf(w[1] - sun.y) < 1e-5f && fabsf(w[2] - sun.z) < 1e-5f;
		for (int k = 0; k < 3; k++)
		{
			const float* axis = march->psConst[5 + k];
			float v = axis[0] * w[0] + axis[1] * w[1] + axis[2] * w[2];
			oneSun &= fabsf(gtao->psConst[6][k] - v) < 1e-4f;
		}
		oneSun &= gtao->psConst[6][3] > 0; // contact shadows on (their length)
		printf("sun: contact shadows (view) (%.4f, %.4f, %.4f), volumetrics (world) (%.4f, %.4f, %.4f)\n", gtao->psConst[6][0],
			gtao->psConst[6][1], gtao->psConst[6][2], w[0], w[1], w[2]);
	}
	Check(oneSun, "contact shadows and volumetrics use the same sun direction (the game's g_ShadowedDirect)");
	return 0;
}

// ---- menus ---------------------------------------------------------------------------------------

static int TestMenus()
{
	Game g;
	if (!Setup(g, false))
		return 1;
	g.Frame();
	// Hold a key until the log has one more `expect` line (keys are polled at Present), release it.
	auto press = [&](int vk, const char* expect) {
		int before = expect ? E2E_LogCount(expect) : 0;
		g_keys[vk] = true;
		g.Frame();
		for (int i = 0; i < 20 && expect && E2E_LogCount(expect) == before; i++)
			g.Frame();
		g_keys[vk] = false;
		g.Frame();
		g.Frame();
		return !expect || E2E_LogCount(expect) == before + 1;
	};
	struct Menu
	{
		int key;
		const char *shown, *hidden;
	} const shadows = { VK_F9, "Shadow menu shown", "Shadow menu hidden" }, ssgi = { VK_F3, "SSGI menu shown", "SSGI menu hidden" },
			taa = { VK_F1, "TAA menu shown", "TAA menu hidden" }, vol = { VK_F11, "Volumetrics menu shown", "Volumetrics menu hidden" };
	auto hiddenLines = [&] {
		return E2E_LogCount(shadows.hidden) + E2E_LogCount(ssgi.hidden) + E2E_LogCount(taa.hidden) + E2E_LogCount(vol.hidden);
	};

	Check(press(shadows.key, shadows.shown), "F9 opens the shadow menu");
	Check(press(ssgi.key, ssgi.shown), "F3 opens the SSGI menu");
	// The arrows reach the SSGI menu only: one setting changes ("2 Debug view"), nothing else.
	int changes = E2E_LogCount("Menu: ");
	press(VK_DOWN, nullptr);
	bool changed = press(VK_RIGHT, "Menu: Debug view = GI light");
	Check(changed && E2E_LogCount("Menu: ") == changes + 1, "Down, Right: only the SSGI menu reacts (%d menu lines, expected 1)",
		E2E_LogCount("Menu: ") - changes);
	press(VK_LEFT, "Menu: Debug view = off");
	// F9 again opens the shadow menu (it was hidden by F3: "shown", not "hidden"), which hides the SSGI
	// menu in turn; and so on through all four.
	Check(press(shadows.key, shadows.shown) && hiddenLines() == 0, "F9 again: the shadow menu opens (F3 had hidden it)");
	Check(press(ssgi.key, ssgi.shown) && hiddenLines() == 0, "F3 again: the SSGI menu opens (F9 had hidden it)");
	Check(press(taa.key, taa.shown) && hiddenLines() == 0, "F1 opens the TAA menu");
	Check(press(vol.key, vol.shown) && hiddenLines() == 0, "F11 opens the volumetrics menu");
	Check(press(taa.key, taa.shown) && hiddenLines() == 0, "F1 again: the TAA menu opens (F11 had hidden it)");
	Check(press(vol.key, vol.shown) && hiddenLines() == 0, "F11 again: the volumetrics menu opens (F1 had hidden it)");
	Check(press(vol.key, vol.hidden), "F11 once more closes it");
	// With every menu closed the arrows do nothing.
	changes = E2E_LogCount("Menu: ");
	press(VK_RIGHT, nullptr);
	Check(E2E_LogCount("Menu: ") == changes, "all menus closed: the arrows do nothing");
	return 0;
}

// ---- msaa ----------------------------------------------------------------------------------------

static int TestMsaa()
{
	Game g;
	if (!Setup(g, true))
		return 1;
	size_t first = g.log->draws.size();
	for (int i = 0; i < 4; i++)
		g.Frame();
	E2EPassCount c = E2E_CountPasses(g.log, first, g.log->draws.size());
	std::string log = E2E_Log();
	size_t at = log.find("In-game anti-aliasing (MSAA) is on, so these are off: ");
	std::string line = at == std::string::npos ? "" : log.substr(at, log.find('\n', at) - at);
	printf("log: %s\n", line.empty() ? "(no MSAA line)" : line.c_str());
	Check(!line.empty(), "the MSAA warning is logged");
	bool all = true;
	for (const char* name : { "ambient occlusion", "contact shadows", "SSGI", "volumetric fog", "sun shafts", "TAA" })
		all &= line.find(name) != std::string::npos;
	Check(all, "it names every effect that is on (AO, contact shadows, SSGI, fog, shafts, TAA)");
	Check(c.Effects() == 0, "no effect passes (AO %d, SSGI %d, volumetrics %d, TAA %d)", c.n[FX_AO], c.n[FX_SSGI], c.n[FX_VOL], c.n[FX_TAA]);
	bool still = true;
	for (size_t i = first; i < g.log->draws.size(); i++)
		still &= g.log->draws[i].c255[0] == 0 && g.log->draws[i].c255[1] == 0;
	Check(still, "the game's draws never get TAA's jitter (c255 = 0 throughout)");
	return 0;
}

// ---- taa / taaoff / noassist -------------------------------------------------------------------------

// What one frame's draws and passes got (copies: the mock's draw log grows and moves).
struct TaaFrame
{
	float c255[2] = {};             // at the first opaque draw
	bool sceneSame = true;          // every scene draw (opaque, sky, transparent) got that c255
	MockDraw draws[4] = {};         // the GTAO, march, SSGI trace and SSGI temporal passes
	const MockDraw *gtao = nullptr, *march = nullptr, *trace = nullptr, *temporal = nullptr; // into draws[], if seen
	TaaFrame() = default;
	TaaFrame(const TaaFrame& o) { *this = o; }
	TaaFrame& operator=(const TaaFrame& o)
	{
		memcpy(c255, o.c255, sizeof(c255));
		sceneSame = o.sceneSame;
		memcpy(draws, o.draws, sizeof(draws));
		const MockDraw* const* from[4] = { &o.gtao, &o.march, &o.trace, &o.temporal };
		const MockDraw** to[4] = { &gtao, &march, &trace, &temporal };
		for (int k = 0; k < 4; k++)
			*to[k] = *from[k] ? &draws[k] : nullptr;
		aoBlur = o.aoBlur;
		passesClean = o.passesClean;
		gameBias = o.gameBias;
		taaPasses = o.taaPasses;
		return *this;
	}
	const char* aoBlur = nullptr;   // which AO blur ran
	bool passesClean = true;        // every effect pass: c255 = 0, sampler 0's mip bias 0, not the game's vertex shader
	float gameBias = 0;             // sampler 0's mip bias at the first opaque draw
	int taaPasses = 0;
};

static float BiasOf(DWORD v)
{
	float f;
	memcpy(&f, &v, 4);
	return f;
}

static TaaFrame Inspect(const Game& g, const Game::Tags& t)
{
	const MockLog& L = *g.log;
	TaaFrame f;
	const MockDraw& opaque = L.draws[t.firstOpaque];
	f.c255[0] = opaque.c255[0];
	f.c255[1] = opaque.c255[1];
	f.gameBias = BiasOf(opaque.lodBias0);
	for (size_t i : { t.firstOpaque, t.lastOpaque, t.sky, t.trans })
		f.sceneSame &= L.draws[i].c255[0] == f.c255[0] && L.draws[i].c255[1] == f.c255[1];
	for (size_t i = t.begin; i < t.end; i++)
	{
		const MockDraw& d = L.draws[i];
		const char* name = E2E_PassName(d);
		if (!name)
			continue;
		f.passesClean &= d.c255[0] == 0 && d.c255[1] == 0 && d.c255[2] == 0 && d.c255[3] == 0 && BiasOf(d.lodBias0) == 0.0f && d.vs != g.gameVs;
		const char* const kept[4] = { "gtao", "march", "gi-trace", "gi-temporal" };
		const MockDraw** slot[4] = { &f.gtao, &f.march, &f.trace, &f.temporal };
		for (int k = 0; k < 4; k++)
			if (!strcmp(name, kept[k]))
			{
				f.draws[k] = d;
				*slot[k] = &f.draws[k];
			}
		else if (!f.aoBlur && !strncmp(name, "ao-blur", 7))
			f.aoBlur = name;
		f.taaPasses += E2E_Effect(d) == FX_TAA;
	}
	return f;
}

// (b - a) mod 1 is `step` (mod 1): consecutive terms of the sequence n * step mod 1.
static bool NextInSequence(float a, float b, double step)
{
	double d = (double)b - a - step;
	d -= floor(d);
	return d < 1e-4 || d > 1 - 1e-4;
}

static const double kGolden = 0.6180339887498949;

// The frames f (indices into `fr`, f > 0) with everything as with TAA running and assisting.
static void CheckAssisted(const std::vector<TaaFrame>& fr, size_t from, const char* what)
{
	bool jitter = true, folded = true, aoSeq = true, fogSeq = true, blur = true, blend = true, clean = true, bias = true, complete = true;
	float worst = 0;
	for (size_t f = from; f < fr.size(); f++)
	{
		const TaaFrame& a = fr[f];
		const TaaFrame& p = fr[f - 1];
		complete &= a.gtao && a.march && a.trace && a.temporal && a.aoBlur && p.gtao && p.march && a.taaPasses == 2;
		if (!complete)
			break;
		jitter &= (a.c255[0] != 0 || a.c255[1] != 0) && a.sceneSame && (a.c255[0] != p.c255[0] || a.c255[1] != p.c255[1]);
		for (const MockDraw* d : { a.gtao, a.march, a.trace })
			for (int k = 0; k < 2; k++)
				worst = fmaxf(worst, fabsf(d->psConst[3][k] - a.c255[k]));
		aoSeq &= a.gtao->psConst[3][2] == 0.0f && a.gtao->psConst[3][3] == 0.0f;
		fogSeq &= NextInSequence(p.march->psConst[24][3], a.march->psConst[24][3], kGolden);
		blur &= !strcmp(a.aoBlur, "ao-blur");
		blend &= a.temporal->psConst[9][0] == 0.1f && a.temporal->psConst[9][3] == 1.0f / 0.1f;
		clean &= a.passesClean;
		bias &= a.gameBias == -0.5f;
	}
	folded = worst < 2e-6f; // the game's camera here is centred: P02 = P12 = 0 up to the float matrices' rounding
	Check(complete, "%s: every frame has the AO, SSGI, volumetrics and TAA passes", what);
	Check(jitter, "%s: the game's scene draws get TAA's jitter, the same in all of a frame's draws, a new one each frame", what);
	Check(folded, "%s: AO, SSGI and the volumetrics get P02 / P12 = the unjittered ones + the c255 the game's draws got that frame "
				  "(worst difference %.2g)",
		what, worst);
	Check(aoSeq, "%s: AO's noise offsets (c3.zw) stay 0 (TAA can't follow moving characters)", what);
	Check(fogSeq, "%s: the fog's noise offset (c24.w) steps through the golden ratio sequence frame by frame", what);
	Check(blur, "%s: AO keeps its normal blur", what);
	Check(blend, "%s: SSGI's minimum temporal blend stays the ini's (0.1)", what);
	Check(bias, "%s: the game's scene draws get TAA's mip bias (-0.5)", what);
	Check(clean, "%s: every effect pass draws with c255 = 0, no mip bias on sampler 0, and its own vertex shader", what);
}

// Frames [from, end) as without TAA. `jitterFolded`: the jitter is still there and folded in (AssistEffects=0).
static void CheckUnassisted(const std::vector<TaaFrame>& fr, size_t from, const char* what, bool taaRuns, bool jitterFolded)
{
	bool jitter = true, offsets = true, blur = true, blend = true, clean = true, complete = true;
	float worst = 0;
	for (size_t f = from; f < fr.size(); f++)
	{
		const TaaFrame& a = fr[f];
		complete &= a.gtao && a.march && a.trace && a.temporal && a.aoBlur && a.taaPasses == (taaRuns ? 2 : 0);
		if (!complete)
			break;
		jitter &= jitterFolded ? (a.c255[0] != 0 || a.c255[1] != 0) && a.sceneSame : a.c255[0] == 0 && a.c255[1] == 0;
		for (const MockDraw* d : { a.gtao, a.march, a.trace })
			for (int k = 0; k < 2; k++)
				worst = fmaxf(worst, fabsf(d->psConst[3][k] - a.c255[k]));
		offsets &= a.gtao->psConst[3][2] == 0 && a.gtao->psConst[3][3] == 0 && a.march->psConst[24][3] == 0;
		blur &= !strcmp(a.aoBlur, "ao-blur");
		blend &= a.temporal->psConst[9][0] == 0.1f && a.temporal->psConst[9][3] == 1.0f / 0.1f;
		clean &= a.passesClean;
	}
	Check(complete, "%s: every frame has the AO, SSGI and volumetrics passes%s", what, taaRuns ? " and TAA's" : ", none of TAA");
	Check(jitter && worst < 2e-6f, "%s: %s (worst difference of P02 / P12 from the c255 the game got %.2g)", what,
		jitterFolded ? "the scene is jittered and the effects get the jitter folded in" : "no jitter (c255 = 0), the effects get the unjittered P02 / P12",
		worst);
	Check(offsets, "%s: the noise offsets are 0 (AO c3.zw, fog c24.w): the same pattern every frame as without TAA", what);
	Check(blur, "%s: AO uses its usual blur", what);
	Check(blend, "%s: SSGI's minimum temporal blend is the ini's (0.1)", what);
	Check(clean, "%s: every effect pass draws with c255 = 0, no mip bias on sampler 0, and its own vertex shader", what);
}

static int TestTaa(const char* mode)
{
	const bool off = !strcmp(mode, "taaoff"), noAssist = !strcmp(mode, "noassist");
	const char* const iniOff[] = { "TAA", "Enabled", "0", nullptr };
	const char* const iniNoAssist[] = { "TAA", "AssistEffects", "0", nullptr };
	Game g;
	if (!Setup(g, false, off ? iniOff : noAssist ? iniNoAssist : nullptr))
		return 1;
	std::vector<TaaFrame> fr;
	for (int i = 0; i < 10; i++)
		fr.push_back(Inspect(g, g.Frame()));
	if (getenv("TAA_DEBUG")) // the constants frame by frame, for debugging
		for (const TaaFrame& f : fr)
			printf("  c255 %.6f %.6f | gtao c3 %.6f %.6f %.6f %.6f | march c3 %.6f %.6f c24w %.6f | trace c3 %.6f %.6f | blend %.6f %.6f | %s\n",
				f.c255[0], f.c255[1], f.gtao ? f.gtao->psConst[3][0] : -1, f.gtao ? f.gtao->psConst[3][1] : -1,
				f.gtao ? f.gtao->psConst[3][2] : -1, f.gtao ? f.gtao->psConst[3][3] : -1, f.march ? f.march->psConst[3][0] : -1,
				f.march ? f.march->psConst[3][1] : -1, f.march ? f.march->psConst[24][3] : -1, f.trace ? f.trace->psConst[3][0] : -1,
				f.trace ? f.trace->psConst[3][1] : -1, f.temporal ? f.temporal->psConst[9][0] : -1, f.temporal ? f.temporal->psConst[9][3] : -1,
				f.aoBlur ? f.aoBlur : "-");
	// TAA jitters from its third frame on (it needs one resolved frame first).
	if (off)
	{
		CheckUnassisted(fr, 1, "TAA disabled in the ini", false, false);
		return 0;
	}
	if (noAssist)
	{
		CheckUnassisted(fr, 3, "AssistEffects=0", true, true);
		return 0;
	}
	CheckAssisted(fr, 3, "TAA on");

	// Switched off in the F1 menu (its first item), then on again.
	auto press = [&](int vk, const char* expect) {
		int before = E2E_LogCount(expect);
		g_keys[vk] = true;
		g.Frame();
		for (int i = 0; i < 20 && E2E_LogCount(expect) == before; i++)
			g.Frame();
		g_keys[vk] = false;
		g.Frame();
		return E2E_LogCount(expect) == before + 1;
	};
	bool menu = press(VK_F1, "TAA menu shown") && press(VK_LEFT, "Menu: TAA = OFF");
	Check(menu, "F1, Left: TAA switched off in the menu");
	fr.clear();
	for (int i = 0; i < 6; i++)
		fr.push_back(Inspect(g, g.Frame()));
	CheckUnassisted(fr, 1, "TAA switched off", false, false);
	Check(press(VK_RIGHT, "Menu: TAA = ON"), "Right: TAA switched on again");
	fr.clear();
	for (int i = 0; i < 8; i++)
		fr.push_back(Inspect(g, g.Frame()));
	CheckAssisted(fr, 3, "TAA on again");
	return 0;
}

// ---- refusal -------------------------------------------------------------------------------------

struct FoundBox
{
	HWND wnd;
	char text[1024];
};
static BOOL CALLBACK FindBox(HWND w, LPARAM p)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(w, &pid);
	char title[128] = {};
	GetWindowTextA(w, title, sizeof(title));
	if (pid == GetCurrentProcessId() && !strcmp(title, "AC2 Shadow Overhaul"))
	{
		FoundBox* f = (FoundBox*)p;
		f->wnd = w;
		GetDlgItemTextA(w, 0xFFFF, f->text, sizeof(f->text)); // the message box's text
		return FALSE;
	}
	return TRUE;
}

static int TestRefusal()
{
	if (!E2E_WriteIni(E2EEffects::All()))
		return 1;
	// A leftover of the separate SSGI plugin next to ours (its content doesn't matter: only the name).
	const char* dummy = "scripts\\AC2SSGI.asi";
	if (FILE* f = fopen(dummy, "wb"))
	{
		fputs("not a plugin", f);
		fclose(f);
	}
	void** slot = E2E_ImportSlot(GetModuleHandleA(nullptr), "d3d9.dll", "Direct3DCreate9");
	void* before = slot ? *slot : nullptr;
	HMODULE m = E2E_LoadPlugin();
	void* after = slot ? *slot : nullptr;

	// The message box comes up on its own thread: find it and close it (it must not hold the test up).
	FoundBox box = {};
	for (int i = 0; i < 50 && !box.wnd; i++)
	{
		EnumWindows(FindBox, (LPARAM)&box);
		if (!box.wnd)
			Sleep(100);
	}
	if (box.wnd)
	{
		printf("message box: \"%.80s...\"\n", box.text);
		Check(strstr(box.text, "AC2SSGI.asi") != nullptr, "the message box names AC2SSGI.asi");
		PostMessageA(box.wnd, WM_CLOSE, 0, 0);
		for (int i = 0; i < 30 && IsWindow(box.wnd); i++)
			Sleep(100);
		printf("message box %s\n", IsWindow(box.wnd) ? "still open (left to the process exit)" : "closed");
	}
	else
		printf("(the message box was not seen within 5 s - no interactive desktop? - only the log is checked)\n");

	std::string log = E2E_Log();
	Check(m != nullptr, "the plugin loads (and does nothing)");
	Check(log.find("Found AC2SSGI.asi next to AC2ShadowOverhaul.asi") != std::string::npos && log.find("doing nothing until then") != std::string::npos,
		"the refusal is logged");
	Check(slot && before == after, "the game's Direct3DCreate9 import is left alone (no hooks)");
	Check(log.find("Direct3D hooks installed") == std::string::npos && log.find("Frame dump") == std::string::npos &&
			log.find("SSGI:") == std::string::npos && log.find("TAA:") == std::string::npos && log.find("Volumetric") == std::string::npos,
		"no effect or hook set up");

	// A frame draws nothing but the game's draws.
	Game g;
	if (!g.Create())
	{
		Check(false, "the (mock) device can still be created");
		return 0;
	}
	size_t first = g.log->draws.size();
	g.Frame();
	g.Frame();
	E2EPassCount c = E2E_CountPasses(g.log, first, g.log->draws.size());
	Check(c.Effects() == 0 && g.log->draws.size() - first == 2 * 9, "frames: only the game's draws (%zu draws, %d effect passes)",
		g.log->draws.size() - first, c.Effects());
	Check(E2E_Log().find("Device:") == std::string::npos, "the device was not hooked (nothing logged about it)");
	DeleteFileA(dummy);
	return 0;
}

int main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	const char* mode = argc > 1 ? argv[1] : "";
	printf("combined end to end: %s\n", mode);
	int r;
	if (!strcmp(mode, "order"))
		r = TestOrder();
	else if (!strcmp(mode, "menus"))
		r = TestMenus();
	else if (!strcmp(mode, "msaa"))
		r = TestMsaa();
	else if (!strcmp(mode, "refusal"))
		r = TestRefusal();
	else if (!strcmp(mode, "taa") || !strcmp(mode, "taaoff") || !strcmp(mode, "noassist"))
		r = TestTaa(mode);
	else
		return printf("usage: AssassinsCreedIIGame.exe order|menus|msaa|refusal|taa|taaoff|noassist\n"), 2;
	if (r)
		return r;
	printf("%s: %d failure(s)\n", g_fails ? "FAILED" : "ALL PASSED", g_fails);
	return g_fails ? 1 : 0;
}
