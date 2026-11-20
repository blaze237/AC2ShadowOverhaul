// End-to-end test of the volumetric fog and sun shafts through the plugin's Direct3D hooks and frame
// tracking. Built as e2e/AssassinsCreedIIGame.exe next to the mock d3d9.dll (tests/mockd3d), it
// statically imports d3d9!Direct3DCreate9 like the game, loads scripts\AC2ShadowOverhaul.asi (as
// Ultimate ASI Loader would), then renders frames shaped like AC2's: shadow atlas, opaque pass with
// the z/w target as RT1 in two batches, the sky (viewport depth 1..1), a transparent draw,
// post-processing into the back buffer (reading the scene colour).
//
// Checks: our passes run once per frame, in order, after the sky and before the transparents
// (Placement=0) or after the transparents (Placement=1); the camera, sun, light-space matrix and
// cascades reach the shaders exactly; the game's render targets, pixel shader, viewport and blend
// states are back when the game draws again. Alone, nothing else draws; with every effect on, the
// ambient occlusion and SSGI passes run after the opaque pass and before the sky (AO first), ours
// after the sky, and the TAA resolve after ours, before the post-processing reads the scene.
//
// Usage: AssassinsCreedIIGame.exe [placement1] [fogoff] [all]
//   (default) volumetrics alone: ambient occlusion, contact shadows, SSGI and TAA Enabled=0
//   all       every effect on
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include "e2e_common.h"

static int g_fails = 0;
static void Check(bool ok, const char* what)
{
	printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		g_fails++;
}
static bool Near(const float* a, const float* b, int n, float tol)
{
	for (int i = 0; i < n; i++)
		if (fabsf(a[i] - b[i]) > tol * (1.0f + fabsf(b[i])))
			return false;
	return true;
}

// ---- Shaders like the game's (only their constant tables matter to the mock) --------------------

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
		printf("compile failed: %s\n", err ? (const char*)err->vt->Ptr(err) : "no compiler");
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

// ---- Camera ------------------------------------------------------------------------------------

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

// ---- Classifying the recorded draws ------------------------------------------------------------

// One of our passes (march, blur, mask, radial, composite), or nullptr.
static const char* OurName(const MockDraw& d)
{
	return E2E_Effect(d) == FX_VOL ? E2E_PassName(d) : nullptr;
}

int main(int argc, char** argv)
{
	bool placement1 = false, all = false, fogOff = false;
	for (int i = 1; i < argc; i++)
	{
		placement1 |= !strcmp(argv[i], "placement1");
		all |= !strcmp(argv[i], "all");
		fogOff |= !strcmp(argv[i], "fogoff");
	}
	printf("volumetrics end to end: placement %d%s, %s\n", placement1 ? 1 : 0, fogOff ? ", fog off" : "",
		all ? "with every effect on" : "volumetrics alone");
	// The ini the plugin reads: the shipped one, plus the variant's changes.
	E2EEffects fx;
	fx.vol = true;
	if (all)
		fx = E2EEffects::All();
	if (!E2E_WriteIni(fx))
		return 1;
	E2E_SetIni("VolumetricFog", "Placement", placement1 ? "1" : "0");
	E2E_SetIni("VolumetricFog", "StartEnabled", fogOff ? "0" : "1");
	if (!E2E_LoadPlugin())
		return 1;
	PFN_MockGetLog getLog = MockExport<PFN_MockGetLog>("MockGetLog");
	MockLog* log = getLog ? getLog() : nullptr;
	if (!log)
	{
		printf("FAIL  not the mock d3d9.dll\n");
		return 1;
	}

	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	D3DPRESENT_PARAMETERS pp = {};
	pp.BackBufferWidth = 640;
	pp.BackBufferHeight = 360;
	pp.BackBufferFormat = D3DFMT_X8R8G8B8;
	pp.Windowed = TRUE;
	pp.EnableAutoDepthStencil = TRUE;
	pp.AutoDepthStencilFormat = D3DFMT_D24S8;
	IDirect3DDevice9* dev = nullptr;
	if (FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, GetDesktopWindow(), D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)) || !dev)
	{
		printf("FAIL  CreateDevice\n");
		return 1;
	}
	const UINT W = pp.BackBufferWidth, H = pp.BackBufferHeight;

	// Game resources.
	IDirect3DTexture9 *sceneTex, *zwTex, *atlasTex, *cloudTex;
	IDirect3DSurface9 *scene, *zw, *atlas, *bb;
	dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &sceneTex, nullptr);
	dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &zwTex, nullptr);
	dev->CreateTexture(4096, 1024, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &atlasTex, nullptr);
	dev->CreateTexture(256, 256, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cloudTex, nullptr);
	sceneTex->GetSurfaceLevel(0, &scene);
	zwTex->GetSurfaceLevel(0, &zw);
	atlasTex->GetSurfaceLevel(0, &atlas);
	dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);

	auto vsCode = Compile(kGameVs, "vs_3_0");
	auto litCode = Compile(kLitPs, "ps_3_0");
	auto otherCode = Compile(kOtherPs, "ps_3_0");
	auto casterCode = Compile(kCasterPs, "ps_3_0");
	IDirect3DVertexShader9* gameVs;
	IDirect3DPixelShader9 *litPs, *skyPs, *transPs, *postPs, *casterPs;
	dev->CreateVertexShader(vsCode.data(), &gameVs);
	dev->CreatePixelShader(litCode.data(), &litPs);
	dev->CreatePixelShader(otherCode.data(), &skyPs);
	dev->CreatePixelShader(otherCode.data(), &transPs);
	dev->CreatePixelShader(otherCode.data(), &postPs);
	dev->CreatePixelShader(casterCode.data(), &casterPs);

	// Camera: the sun is high (from a real frame dump), the camera looks up towards it.
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

	const int kFrames = 3;
	size_t frameStart = 0;
	for (int frame = 0; frame < kFrames; frame++)
	{
		frameStart = log->draws.size();
		dev->BeginScene();
		dev->SetVertexShader(gameVs);
		// Sun shadow atlas.
		dev->SetRenderTarget(0, atlas);
		dev->SetPixelShader(casterPs);
		dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
		// Opaque pass in two batches.
		dev->SetRenderTarget(0, scene);
		D3DVIEWPORT9 vpScene = { 0, 0, W, H, 0.0f, 0.9999f };
		dev->SetViewport(&vpScene);
		dev->SetTexture(14, cloudTex);
		dev->SetTexture(15, atlasTex);
		dev->SetRenderTarget(1, zw);
		dev->SetPixelShader(litPs);
		for (int i = 0; i < 3; i++)
			dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
		dev->SetRenderTarget(1, nullptr);
		dev->SetRenderTarget(1, zw);
		dev->SetPixelShader(litPs);
		dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
		dev->SetRenderTarget(1, nullptr);
		// Sky.
		D3DVIEWPORT9 vpSky = { 0, 0, W, H, 1.0f, 1.0f };
		dev->SetViewport(&vpSky);
		dev->SetPixelShader(skyPs);
		dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
		// Transparents.
		dev->SetViewport(&vpScene);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		dev->SetPixelShader(transPs);
		dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		// Post-processing.
		dev->SetRenderTarget(0, bb);
		dev->SetTexture(0, sceneTex);
		dev->SetPixelShader(postPs);
		dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1);
		dev->EndScene();
		dev->Present(nullptr, nullptr, nullptr, nullptr);
	}

	// ---- The last frame ----------------------------------------------------------------------------
	std::vector<MockDraw> d(log->draws.begin() + frameStart, log->draws.end());
	int sky = -1, trans = -1, post = -1, lastOpaque = -1;
	std::vector<int> ours, others[FX_COUNT]; // others: the other effects' passes
	std::string sequence;
	IDirect3DPixelShader9* gamePs[] = { litPs, skyPs, transPs, postPs, casterPs };
	for (int i = 0; i < (int)d.size(); i++)
	{
		const char* name = OurName(d[i]);
		bool game = false;
		for (auto* p : gamePs)
			game |= d[i].ps == p;
		if (name)
		{
			ours.push_back(i);
			sequence += sequence.empty() ? name : std::string(" ") + name;
		}
		else if (!game && !(d[i].rt[1] == zw)) // the opaque shaders may be rewritten (AO, SSGI), so those are still the game's
			others[E2E_Effect(d[i])].push_back(i);
		if (d[i].vp.MinZ == 1.0f && !name && !d[i].up)
			sky = i;
		if (!d[i].up && d[i].rt[1] == zw)
			lastOpaque = i;
		if (!d[i].up && d[i].vp.MinZ < 0.5f && d[i].rt[0] == scene && d[i].rt[1] == nullptr && !name && sky >= 0 && trans < 0)
			trans = i;
		if (!d[i].up && d[i].rt[0] == bb && !name)
			post = i;
	}
	printf("last frame: %zu draws; opaque up to %d, sky %d, transparent %d, post %d; ours: %s\n", d.size(), lastOpaque, sky, trans, post,
		sequence.c_str());

	const char* expected = fogOff ? "mask radial radial composite" : "march blur blur mask radial radial composite";
	Check(sequence == expected, fogOff ? "passes: shafts and composite only (fog off)" : "passes: march, 2 blurs, mask, 2 radial blurs, composite");
	Check(sky >= 0 && trans > sky && post > trans, "the game's sky, transparent and post draws are all there, in order");
	if (!ours.empty())
	{
		if (placement1)
			Check(ours.front() > trans && ours.back() < post, "placement 1: after the transparents, before post-processing");
		else
			Check(ours.front() > sky && ours.back() < trans, "placement 0: after the sky, before the transparents");
		const MockDraw& comp = d[ours.back()];
		Check(comp.rt[0] == scene && comp.rt[1] == nullptr, "composite draws into the scene colour target, nothing on RT1");
		Check(comp.alphaBlend == TRUE && comp.srcBlend == D3DBLEND_ONE && comp.destBlend == D3DBLEND_SRCALPHA && comp.blendOp == D3DBLENDOP_ADD &&
				comp.colorWrite == 7,
			"composite blends scene * alpha + colour, colour channels only");
		const MockDraw& first = d[ours.front()];
		const float (*c)[4] = first.psConst;
		const float expProj[4] = { A, B, P00, P11 };
		Check(Near(c[2], expProj, 4, 1e-3f), "camera projection (A, B, P00, P11) recovered");
		Check(Near(c[4], &eyeC[0], 3, 1e-4f), "camera position recovered from the matrices");
		Check(Near(c[5], &r.x, 3, 1e-3f) && Near(c[6], &u.x, 3, 1e-3f) && Near(c[7], &f.x, 3, 1e-3f), "camera axes recovered");
		Check(Near(c[21], &sun.x, 3, 1e-4f), "sun direction (normalised) from g_ShadowedDirect");
		Check(first.tex[0] == zwTex, "the game's z/w texture is sampled as depth");
		if (!fogOff)
		{
			const float s11[4] = { nearFar[0], nearFar[1], 1, 0 };
			Check(Near(c[8], light, 4, 1e-5f) && Near(c[9], light + 4, 4, 1e-5f) && Near(c[10], light + 8, 4, 1e-5f) && Near(c[11], s11, 4, 1e-5f),
				"light-space matrix and shadow depth mapping from the game's constants");
			Check(Near(c[12], rMin, 4, 1e-5f) && Near(c[13], rMax, 4, 1e-5f) && Near(c[14], scX, 4, 1e-5f) && Near(c[15], scY, 4, 1e-5f) &&
					Near(c[16], ofX, 4, 1e-5f) && Near(c[17], ofY, 4, 1e-5f) && Near(c[18], bias, 4, 1e-5f) && fabsf(c[19][0] - fade[0]) < 1e-3f &&
					c[19][2] == 1.0f && Near(c[20], cloudUV, 4, 1e-5f),
				"cascades, bias, fade and cloud shadows from the game's constants");
			Check(first.tex[1] == atlasTex && first.tex[2] == cloudTex, "the game's shadow atlas and cloud shadow texture are sampled");
			Check(fabsf(c[24][0] - 0.0012f) < 1e-6f && fabsf(c[23][0] - fogColor[0] * 0.25f) < 1e-5f, "fog density and ambient colour from the ini / game");
		}
		// The game's state is back for its next draw.
		const MockDraw& next = d[placement1 ? post : trans];
		if (placement1)
			Check(next.ps == postPs && next.rt[0] == bb, "post draw: the game's shader and target");
		else
			Check(next.ps == transPs && next.rt[0] == scene && next.rt[1] == nullptr && next.vp.MinZ == 0.0f && fabsf(next.vp.MaxZ - 0.9999f) < 1e-6f &&
					next.alphaBlend == TRUE && next.srcBlend == D3DBLEND_SRCALPHA && next.destBlend == D3DBLEND_INVSRCALPHA,
				"transparent draw: the game's shader, targets, viewport and blending are restored");
	}
	int perFrame = 0;
	for (size_t i = 0; i < log->draws.size(); i++)
		if (OurName(log->draws[i]) && !strcmp(OurName(log->draws[i]), "composite"))
			perFrame++;
	Check(perFrame == kFrames, "one composite per frame");
	for (int e = 0; e < FX_COUNT; e++)
		if (!others[e].empty())
			printf("  %s draws this frame: %zu\n", e == FX_NONE ? "unknown" : kE2EEffectNames[e], others[e].size());
	Check(others[FX_NONE].empty(), "no draws other than the game's and the effects' passes");
	if (!all)
		Check(others[FX_AO].empty() && others[FX_SSGI].empty() && others[FX_TAA].empty(), "alone: no other effect's passes");
	else
	{
		auto within = [](const std::vector<int>& v, int after, int before) {
			bool ok = !v.empty();
			for (int i : v)
				ok &= i > after && i < before;
			return ok;
		};
		Check(others[FX_AO].size() == 4 && within(others[FX_AO], lastOpaque, sky),
			"ambient occlusion (4 passes) runs after the opaque pass and before the sky");
		Check(others[FX_SSGI].size() == 5 && within(others[FX_SSGI], lastOpaque, sky) && !others[FX_AO].empty() &&
				others[FX_SSGI].front() > others[FX_AO].back(),
			"SSGI (5 passes) runs after the ambient occlusion and before the sky");
		Check(!ours.empty() && within(others[FX_TAA], ours.back(), post),
			"the TAA resolve runs after ours, before the post-processing reads the scene");
	}
	printf("%s: %d failure(s)\n", g_fails ? "FAILED" : "ALL PASSED", g_fails);
	return g_fails ? 1 : 0;
}
