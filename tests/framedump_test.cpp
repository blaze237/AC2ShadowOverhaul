// End-to-end test for the frame dump: built as AssassinsCreedIIGame.exe, statically imports
// d3d9!Direct3DCreate9 like the game, loads AC2ShadowOverhaul.asi, renders a few frames with extra
// render targets / depth buffers, and taps the hotkey (F8) via SendInput.
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <vector>
#include <string.h>
#include <stdint.h>
#include <string>
#include "../src/shaders_gen.h"
#include "mockd3d/mock_api.h"
#include <math.h>

// D3DCompiler (any version with D3DCompile / D3DDisassemble) for test shaders.
struct Blob { struct { void* q; void* a; ULONG(__stdcall* Release)(void*); void*(__stdcall* Ptr)(void*); SIZE_T(__stdcall* Size)(void*); }* vt; };
typedef HRESULT(WINAPI* PFN_Compile)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, Blob**, Blob**);
typedef HRESULT(WINAPI* PFN_Disasm)(const void*, SIZE_T, UINT, const char*, Blob**);
static HMODULE Compiler()
{
	static HMODULE c = LoadLibraryA("D3DCompiler_43.dll");
	if (!c)
		c = LoadLibraryA("D3DCompiler_47.dll");
	return c;
}
static Blob* Compile(const char* src, const char* target)
{
	auto compile = Compiler() ? (PFN_Compile)GetProcAddress(Compiler(), "D3DCompile") : nullptr;
	Blob* code = nullptr;
	if (!compile || FAILED(compile(src, strlen(src), "test", nullptr, nullptr, "main", target, 0, 0, &code, nullptr)))
		return nullptr;
	return code;
}

// Text of the newest file in scripts\ matching `pattern`.
static std::string ReadDump(const char* pattern)
{
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pattern, &fd);
	std::string best;
	if (h == INVALID_HANDLE_VALUE)
		return "";
	do
		if (best.empty() || strcmp(fd.cFileName, best.c_str()) > 0)
			best = fd.cFileName;
	while (FindNextFileA(h, &fd));
	FindClose(h);
	std::string text;
	FILE* f = fopen(("scripts\\" + best).c_str(), "rb");
	if (!f)
		return "";
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		text.append(buf, n);
	fclose(f);
	return text;
}

// No interactive desktop here, so fake the menu keys by replacing the asi's GetAsyncKeyState import.
// The mock device can't back a real D3DX font, so hide D3DXCreateFontA from the asi.
static FARPROC WINAPI FakeGetProcAddress(HMODULE m, LPCSTR name)
{
	if ((uintptr_t)name > 0xFFFF && strcmp(name, "D3DXCreateFontA") == 0)
		return nullptr;
	return GetProcAddress(m, name);
}
static volatile bool g_keys[256];
static SHORT WINAPI FakeGetAsyncKeyState(int vk)
{
	return vk > 0 && vk < 256 && g_keys[vk] ? (SHORT)0x8000 : 0;
}

// The menu reads its keys at Present: hold a key over one Present, release it over the next.
static void MenuPress(IDirect3DDevice9* dev, int vk)
{
	g_keys[vk] = true;
	dev->Present(NULL, NULL, NULL, NULL);
	g_keys[vk] = false;
	dev->Present(NULL, NULL, NULL, NULL);
}

static bool PatchAsiImport(HMODULE mod, const char* func, void* repl)
{
	unsigned char* base = (unsigned char*)mod;
	auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
	auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++)
	{
		auto names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
		auto iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
		for (; names->u1.AddressOfData; names++, iat++)
		{
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
				continue;
			if (strcmp((const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name, func) == 0)
			{
				DWORD old;
				VirtualProtect(&iat->u1.Function, 4, PAGE_READWRITE, &old);
				iat->u1.Function = (uintptr_t)repl;
				return true;
			}
		}
	}
	return false;
}

static HWND g_wnd;
static HWND WINAPI FakeGetForegroundWindow()
{
	return g_wnd;
}

int main()
{
	HMODULE asi = LoadLibraryA("scripts\\AC2ShadowOverhaul.asi");
	printf("asi %p\n", (void*)asi);
	if (!asi)
		return 1;
	printf("key hook %d\n", PatchAsiImport(asi, "GetAsyncKeyState", (void*)FakeGetAsyncKeyState));
	printf("font hook %d\n", PatchAsiImport(asi, "GetProcAddress", (void*)FakeGetProcAddress));

	HWND wnd = CreateWindowA("STATIC", "fd", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, NULL, NULL, NULL, NULL);
	g_wnd = wnd;
	printf("focus hook %d\n", PatchAsiImport(asi, "GetForegroundWindow", (void*)FakeGetForegroundWindow));
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	D3DADAPTER_IDENTIFIER9 id = {};
	d3d->GetAdapterIdentifier(0, 0, &id);
	printf("adapters %u: %s, wnd %p\n", d3d->GetAdapterCount(), id.Description, (void*)wnd);
	D3DPRESENT_PARAMETERS pp = {};
	pp.Windowed = TRUE;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.BackBufferFormat = D3DFMT_X8R8G8B8;
	pp.BackBufferWidth = 320;
	pp.BackBufferHeight = 240;
	pp.EnableAutoDepthStencil = TRUE;
	pp.AutoDepthStencilFormat = D3DFMT_D24S8;
	IDirect3DDevice9* dev = nullptr;
	HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
	printf("CreateDevice 0x%08lX\n", hr);
	if (FAILED(hr))
		return 2;

	IDirect3DTexture9 *scene = nullptr, *depthTex = nullptr;
	IDirect3DSurface9* depth2 = nullptr;
	IDirect3DTexture9* linDepth = nullptr;
	dev->CreateTexture(320, 240, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &linDepth, NULL);
	dev->CreateTexture(320, 240, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &scene, NULL);
	hr = dev->CreateTexture(320, 240, 1, D3DUSAGE_DEPTHSTENCIL, (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'), D3DPOOL_DEFAULT, &depthTex, NULL);
	printf("INTZ 0x%08lX\n", hr);
	dev->CreateDepthStencilSurface(320, 240, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &depth2, NULL);
	IDirect3DSurface9 *sceneSurf = nullptr, *bb = nullptr, *autoDs = nullptr;
	scene->GetSurfaceLevel(0, &sceneSurf);
	dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
	dev->GetDepthStencilSurface(&autoDs);

	struct V { float x, y, z, rhw; } tri[3] = { { 10, 10, 0.5f, 1 }, { 200, 10, 0.5f, 1 }, { 10, 200, 0.5f, 1 } };
	dev->SetFVF(D3DFVF_XYZRHW);
	static const DWORD psCode[] = { 0xFFFF0300, 0x0000FFFF };
	IDirect3DPixelShader9* ps = nullptr;
	dev->CreatePixelShader(psCode, &ps);

	// A shadow atlas (4 cascades wide) and a real ps_3_0 shader sampling it at s15.
	IDirect3DTexture9* atlas = nullptr;
	dev->CreateTexture(1024, 256, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &atlas, NULL);
	IDirect3DPixelShader9* shadowPs = nullptr;
	{
		const char* src =
			"sampler2D g_ShadowMap : register(s15);\n"
			"float4 g_ShadowCascadesRangesMax : register(c40);\n"
			"float4 main(float4 uv : TEXCOORD0) : COLOR {\n"
			"  float d = tex2D(g_ShadowMap, uv.xy).r;\n"
			"  float lit = uv.z <= d ? 1.0 : 0.3;\n"
			"  return float4(lit * g_ShadowCascadesRangesMax.xyz, 1); }\n";
		if (Blob* code = Compile(src, "ps_3_0"))
			dev->CreatePixelShader((const DWORD*)code->vt->Ptr(code), &shadowPs);
		printf("shadow test shader compiled: %s\n", shadowPs ? "yes" : "NO");
	}
	// A vertex shader with a named camera matrix, for the dump's constant export.
	IDirect3DVertexShader9* vs = nullptr;
	if (Blob* code = Compile("float4x4 g_ViewProj : register(c0);\nfloat4 g_EyePosition : register(c8);\n"
							 "float4 main(float4 p : POSITION) : POSITION { return mul(p, g_ViewProj) + g_EyePosition; }\n",
			"vs_3_0"))
		dev->CreateVertexShader((const DWORD*)code->vt->Ptr(code), &vs);
	const float viewProj[16] = { 1, 2, 3, 4, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }, eye[4] = { 10, 20, 30, 1 };
	dev->SetVertexShaderConstantF(0, viewProj, 4);
	dev->SetVertexShaderConstantF(8, eye, 1);
	printf("vertex shader compiled: %s\n", vs ? "yes" : "NO");

	// The compiled depth copy shader must be ps_2_0 and do what it says.
	{
		auto disasm = Compiler() ? (PFN_Disasm)GetProcAddress(Compiler(), "D3DDisassemble") : nullptr;
		Blob* text = nullptr;
		std::string t;
		if (disasm && SUCCEEDED(disasm(kDepthToColourPs, sizeof(kDepthToColourPs), 0, nullptr, &text)) && text)
			t.assign((const char*)text->vt->Ptr(text), text->vt->Size(text));
		bool ok = t.find("ps_2_0") != std::string::npos && t.find("dcl t0.xy") != std::string::npos &&
				  t.find("dcl_2d s0") != std::string::npos && t.find("texld r0, t0, s0") != std::string::npos &&
				  t.find("mov oC0, r0") != std::string::npos;
		printf("depth copy shader disassembles as expected: %s\n", ok ? "yes" : "NO");
		if (!ok)
			printf("%s\n", t.c_str());
	}

	for (int frame = 0; frame < 6; frame++)
	{
		// The menu (shown at start): Up wraps to its last item, "Frame dump"; Right asks for a dump,
		// which starts after this frame's Present.
		g_keys[VK_UP] = frame == 0;
		g_keys[VK_RIGHT] = frame == 1;
		dev->BeginScene();
		dev->SetVertexShader(vs);
		dev->SetRenderTarget(0, sceneSurf);
		dev->SetDepthStencilSurface(depth2);
		dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
		for (int i = 0; i < 5; i++)
			dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(V));
		dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(V));
		dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		dev->SetRenderTarget(0, bb);
		dev->SetDepthStencilSurface(autoDs);
		dev->SetTexture(0, scene);
		dev->SetTexture(1, depthTex);
		dev->SetTexture(8, linDepth);
		dev->SetTexture(15, atlas);
		dev->SetPixelShader(ps);
		dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(V));
		dev->SetTexture(0, NULL);
		dev->SetTexture(1, NULL);
		dev->SetTexture(15, atlas);
		dev->SetPixelShader(shadowPs);
		dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(V));
		dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(V));
		dev->SetTexture(15, NULL);
		dev->SetPixelShader(NULL);
		dev->StretchRect(sceneSurf, NULL, bb, NULL, D3DTEXF_LINEAR);
		dev->EndScene();
		dev->Present(NULL, NULL, NULL, NULL);
		Sleep(30);
	}
	// Frame dump: depth buffer readback compared with the R32F "T6" target, and the constant export.
	{
		std::string main = ReadDump("scripts\\AC2ShadowOverhaul_FrameDump_????????_??????.txt");
		size_t at = main.find("vs T");
		size_t miss = main.find("MISSING from the depth buffer ", at == std::string::npos ? 0 : at);
		float missing = miss == std::string::npos ? -1 : (float)atof(main.c_str() + miss + strlen("MISSING from the depth buffer "));
		// The mock hides a quarter-by-quarter box from the depth copy: 6.25% of the image, 9.4% of the geometry (lower 2/3).
		printf("frame dump: depth buffer compared with T6, %.1f%% missing (expected ~9.4%%): %s\n", missing,
			missing > 8.0f && missing < 11.0f ? "yes" : "NO");
		std::string vsText = ReadDump("scripts\\AC2ShadowOverhaul_FrameDump_*_vs.txt");
		printf("frame dump: vertex shader constants exported (g_ViewProj = 1, 2, 3, 4): %s\n",
			vsText.find("g_ViewProj") != std::string::npos && vsText.find("c0   = (1, 2, 3, 4)") != std::string::npos ? "yes" : "NO");
		printf("frame dump: GPU capabilities logged: %s\n", main.find("INTZ depth textures: yes") != std::string::npos ? "yes" : "NO");
	}

	// Ambient occlusion: one opaque pass (scene colour + R32F z/w as render target 1) drawn with a
	// lit shader in the game's style and a vertex shader carrying g_WorldViewProj / g_World.
	{
		PFN_MockGetStats getStats = MockExport<PFN_MockGetStats>("MockGetStats");
		MockAoStats* stats = getStats();
		const char* litSrc =
			"float4 g_FogColor : register(c16);\n"
			"float4 g_AmbientCube[6] : register(c24);\n"
			"struct Light { float3 m_Direction; float3 m_Color; };\n"
			"Light g_ShadowedDirect : register(c52);\n"
			"sampler2D g_Diffuse : register(s1);\n"
			"sampler2D g_WorldLightMapSampler : register(s12);\n"
			"sampler2D g_ShadowSampler : register(s15);\n"
			"struct Out { float4 c0 : COLOR0; float4 c1 : COLOR1; };\n"
			"Out main(float2 uv : TEXCOORD0, float3 n : TEXCOORD1, float4 sh : TEXCOORD2, float fog : TEXCOORD3, float2 zw : TEXCOORD4) {\n"
			"  float3 albedo = tex2D(g_Diffuse, uv).rgb;\n"
			"  float shadow = tex2D(g_ShadowSampler, sh.xy).x >= sh.z ? 1 : 0;\n"
			"  float3 amb = n.x * n.x * g_AmbientCube[0].rgb + n.y * n.y * g_AmbientCube[2].rgb + n.z * n.z * g_AmbientCube[4].rgb;\n"
			"  float3 lm = tex2D(g_WorldLightMapSampler, uv * 0.5).rgb;\n"
			"  float3 indirect = lerp(amb, lm, 0.3) * albedo * n.x;\n" // the game scales indirect albedo by a vertex AO term
			"  float3 sun = saturate(dot(n, g_ShadowedDirect.m_Direction)) * g_ShadowedDirect.m_Color * albedo;\n"
			"  float3 lit = sun * shadow + indirect;\n"
			"  Out o; o.c0 = float4(lerp(lit, g_FogColor.rgb, fog), 1); o.c1 = float4(zw.x / zw.y, 0, 0, 1); return o; }\n";
		Blob* litCode = Compile(litSrc, "ps_3_0");
		IDirect3DPixelShader9* litPs = nullptr;
		UINT litGot = 0;
		if (litCode)
		{
			dev->CreatePixelShader((const DWORD*)litCode->vt->Ptr(litCode), &litPs);
			litPs->GetFunction(nullptr, &litGot);
		}
		std::string logNow;
		if (FILE* f = fopen("scripts\\AC2ShadowOverhaul.log", "rb"))
		{
			char b[4096];
			size_t k;
			while ((k = fread(b, 1, sizeof(b), f)) > 0)
				logNow.append(b, k);
			fclose(f);
		}
		bool litMatched = logNow.find("1 opaque shaders given the indirect light output") != std::string::npos;
		printf("ao: lit shader given the indirect light output (%u -> %u bytes): %s\n", litCode ? (unsigned)litCode->vt->Size(litCode) : 0,
			litGot, litCode && litGot > litCode->vt->Size(litCode) && litMatched ? "yes" : "NO");
		IDirect3DVertexShader9* aoVs = nullptr;
		if (Blob* code = Compile("float4x4 g_WorldViewProj : register(c0);\nfloat4x4 g_World : register(c8);\n"
								 "float4 main(float4 p : POSITION) : POSITION { return mul(p, g_WorldViewProj) + mul(p, g_World); }\n",
				"vs_3_0"))
			dev->CreateVertexShader((const DWORD*)code->vt->Ptr(code), &aoVs);
		// Camera: near 0.1, far 1000, 90 degree horizontal fov (P00 = 1), 4:3, looking down +z from the
		// origin; the object is at (5, 6, 7). Registers hold matrix rows.
		const float n_ = 0.1f, f_ = 1000.0f, A = f_ / (f_ - n_), B = -n_ * f_ / (f_ - n_);
		const float world[16] = { 1, 0, 0, 5, 0, 1, 0, 6, 0, 0, 1, 7, 0, 0, 0, 1 };
		const float P[16] = { 1, 0, 0, 0, 0, 4.0f / 3, 0, 0, 0, 0, A, B, 0, 0, 1, 0 };
		float wvp[16];
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 4; c++)
				wvp[r * 4 + c] = P[r * 4 + 0] * world[0 * 4 + c] + P[r * 4 + 1] * world[1 * 4 + c] + P[r * 4 + 2] * world[2 * 4 + c] +
								 P[r * 4 + 3] * world[3 * 4 + c];
		IDirect3DTexture9* zTex = nullptr;
		IDirect3DSurface9 *zSurf = nullptr, *rt2 = nullptr, *cur = nullptr;
		dev->CreateTexture(320, 240, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &zTex, NULL);
		zTex->GetSurfaceLevel(0, &zSurf);

		dev->BeginScene();
		dev->SetRenderTarget(0, sceneSurf);
		dev->SetRenderTarget(1, zSurf);
		IDirect3DSurface9* rt3 = nullptr;
		bool bound = SUCCEEDED(dev->GetRenderTarget(2, &rt2)) && rt2 && SUCCEEDED(dev->GetRenderTarget(3, &rt3)) && rt3 && rt3 != rt2;
		dev->SetVertexShader(aoVs);
		dev->SetVertexShaderConstantF(0, wvp, 4);
		dev->SetVertexShaderConstantF(8, world, 4);
		dev->SetPixelShader(litPs);
		const float sunDir[4] = { 0.0f, 0.6f, 0.8f, 0.0f }, sunColour[4] = { 1.2f, 1.1f, 0.9f, 0.0f };
		dev->SetPixelShaderConstantF(52, sunDir, 1);
		dev->SetPixelShaderConstantF(53, sunColour, 1);
		for (int i = 0; i < 3; i++)
			dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(V));
		// Between batches the game unbinds render target 1, resets 2 and 3, and rebinds 1: the pass
		// goes on (AO must not run here, and our target must stay bound).
		dev->SetRenderTarget(1, NULL);
		dev->SetRenderTarget(2, NULL);
		dev->SetRenderTarget(3, NULL);
		int midBefore = stats->draws;
		dev->SetRenderTarget(1, zSurf);
		IDirect3DSurface9* cur3 = nullptr;
		bool rebound = SUCCEEDED(dev->GetRenderTarget(2, &cur)) && cur == rt2 && SUCCEEDED(dev->GetRenderTarget(3, &cur3)) && cur3 == rt3;
		dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(V));
		printf("ao: pass continues across the game's render target resets between batches: %s\n",
			rebound && stats->draws - midBefore == 1 ? "yes" : "NO");
		dev->SetRenderTarget(1, NULL);
		bool unbound = (FAILED(dev->GetRenderTarget(2, &cur)) || !cur) && (FAILED(dev->GetRenderTarget(3, &cur)) || !cur);
		int before = stats->draws;
		dev->SetPixelShader(ps);
		// The draw that ends the pass may be instanced foliage: AO must draw with instancing off and
		// hand the game its stream frequencies back.
		dev->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | 4);
		dev->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1);
		dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1); // AO runs first
		UINT f0 = 0, f1 = 0;
		dev->GetStreamSourceFreq(0, &f0);
		dev->GetStreamSourceFreq(1, &f1);
		printf("ao: passes drawn with instancing off, game's instancing restored after: %s\n",
			stats->instancedUpDraws == 0 && f0 == (D3DSTREAMSOURCE_INDEXEDDATA | 4) && f1 == (D3DSTREAMSOURCE_INSTANCEDATA | 1) ? "yes" : "NO");
		dev->SetStreamSourceFreq(0, 1);
		dev->SetStreamSourceFreq(1, 1);
		int aoDraws = stats->draws - before - 1;
		printf("ao: indirect and sun light targets bound as render targets 2 and 3 during the pass: %s\n", bound && unbound ? "yes" : "NO");
		printf("ao: 5 passes drawn before the sky (geometry prep, GTAO, 2 blurs, composite) (%d): %s\n", aoDraws, aoDraws == 5 ? "yes" : "NO");
		IDirect3DSurface9* back = nullptr;
		dev->GetRenderTarget(0, &back);
		printf("ao: game's render target and state restored after the passes: %s\n",
			back == sceneSurf && stats->stateCaptures == 1 && stats->stateApplies == 1 && stats->lastPs == ps ? "yes" : "NO");
		dev->EndScene();
		dev->Present(NULL, NULL, NULL, NULL);
		std::string log;
		if (FILE* f = fopen("scripts\\AC2ShadowOverhaul.log", "rb"))
		{
			char buf[4096];
			size_t n;
			while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
				log.append(buf, n);
			fclose(f);
		}
		bool camera = log.find("camera found (near 0.100 m, far 1000 m, horizontal fov 90.0 deg)") != std::string::npos;
		printf("ao: camera read from g_WorldViewProj / g_World: %s\n", camera ? "yes" : "NO");
		if (!camera)
		{
			size_t at = log.find("Ambient occlusion: c");
			printf("%s\n", at == std::string::npos ? "(no camera line)" : log.substr(at, 160).c_str());
		}
		printf("ao: first frame applied (logged): %s\n", log.find("Ambient occlusion: first frame applied") != std::string::npos ? "yes" : "NO");
		printf("contact shadows: sun direction read from g_ShadowedDirect: %s\n",
			log.find("Main pass: sun direction found (0.000, 0.600, 0.800)") != std::string::npos ? "yes" : "NO");

		// The menu turns AO (6) and contact shadows (9) off: next frame nothing is bound or drawn.
		MenuPress(dev, '6');
		MenuPress(dev, VK_LEFT);
		MenuPress(dev, '9');
		MenuPress(dev, VK_LEFT);
		dev->BeginScene();
		dev->SetRenderTarget(0, sceneSurf);
		dev->SetRenderTarget(1, zSurf);
		bool boundOff = SUCCEEDED(dev->GetRenderTarget(2, &cur)) && cur;
		dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(V));
		dev->SetRenderTarget(1, NULL);
		before = stats->draws;
		dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(V));
		dev->EndScene();
		dev->Present(NULL, NULL, NULL, NULL);
		printf("ao: menu AO + contact shadows off: nothing bound or drawn: %s\n", !boundOff && stats->draws - before == 1 ? "yes" : "NO");
		MenuPress(dev, '6');
		MenuPress(dev, VK_RIGHT);
		MenuPress(dev, '9');
		MenuPress(dev, VK_RIGHT);
		char ini[MAX_PATH];
		GetFullPathNameA("scripts\\AC2ShadowOverhaul.ini", MAX_PATH, ini, nullptr);
		Sleep(1100);
		dev->Present(NULL, NULL, NULL, NULL); // saves them, a second after the last change
		printf("ao: menu on/off saved to the ini: %s\n",
			GetPrivateProfileIntA("AmbientOcclusion", "StartEnabled", -1, ini) == 1 &&
					GetPrivateProfileIntA("ContactShadows", "StartEnabled", -1, ini) == 1
				? "yes"
				: "NO");
		dev->SetRenderTarget(0, bb);
	}

	pp.BackBufferWidth = 400;
	sceneSurf->Release();
	scene->Release();
	depthTex->Release();
	depth2->Release();
	bb->Release();
	// Soft shadows: a real game shader goes through the hooked CreatePixelShader.
	do
	{
		FILE* f = fopen("../../re/shaders/dump/81DFCBE3.cso", "rb");
		std::vector<DWORD> game(4096, 0);
		size_t bytes = f ? fread(game.data(), 1, game.size() * 4, f) : 0;
		if (f)
			fclose(f);
		if (!bytes)
		{
			printf("soft shadows: skipped (put an exported shadow shader at re/shaders/dump/81DFCBE3.cso)\n");
			break;
		}
		IDirect3DPixelShader9 *gp = nullptr, *cur = nullptr;
		dev->CreatePixelShader(game.data(), &gp);
		UINT got = 0;
		gp->GetFunction(nullptr, &got);
		printf("soft shadows: game shader %u bytes, driver received %u bytes (%s)\n", (unsigned)bytes, got, got > bytes ? "rewritten" : "NOT rewritten");
		dev->SetPixelShader(gp);
		dev->GetPixelShader(&cur);
		printf("soft shadows on : device has the shader the game set: %s\n", cur == gp ? "yes" : "NO");
		// Menu: "5 Soft shadow width" down to 0 = off (the game's filter).
		auto press = [&](int vk) { MenuPress(dev, vk); };
		press('5');
		press(VK_LEFT);
		press(VK_LEFT);
		press(VK_LEFT);
		dev->SetPixelShader(gp);
		dev->GetPixelShader(&cur);
		UINT curSize = 0;
		if (cur)
			cur->GetFunction(nullptr, &curSize);
		printf("soft shadows off: device has the game's filter (%u bytes, AO output added; soft %u): %s\n", curSize, got,
			cur != gp && curSize > bytes && curSize < got ? "yes" : "NO");

		// Back on: Right twice (0 -> 2).
		press(VK_RIGHT);
		press(VK_RIGHT);
		dev->SetPixelShader(gp);
		dev->GetPixelShader(&cur);
		UINT w2 = 0;
		if (cur)
			cur->GetFunction(nullptr, &w2);
		printf("debug menu width 2: device has another variant (%u bytes; width 3 %u, original %u): %s\n", w2, got, curSize,
			cur != gp && w2 != got && w2 > curSize ? "yes" : "NO");
		press(VK_RIGHT);
		dev->SetPixelShader(gp);
		dev->GetPixelShader(&cur);
		printf("debug menu width 3 again: device has the game's shader: %s\n", cur == gp ? "yes" : "NO");
		press('1');
		press(VK_RIGHT);

		// Saving: width 3 -> 2, then the ini is written a second after the last key press.
		char ini[MAX_PATH];
		GetFullPathNameA("scripts\\AC2ShadowOverhaul.ini", MAX_PATH, ini, nullptr);
		press('5');
		press(VK_LEFT);
		int before = GetPrivateProfileIntA("SoftShadows", "FilterWidth", -1, ini);
		Sleep(1200);
		dev->Present(NULL, NULL, NULL, NULL);
		int after = GetPrivateProfileIntA("SoftShadows", "FilterWidth", -1, ini);
		printf("debug menu save: FilterWidth %d right after the key, %d a second later: %s\n", before, after,
			before == 3 && after == 2 ? "yes" : "NO");
		WritePrivateProfileStringA("SoftShadows", "FilterWidth", "3", ini); // leave the test ini as it was

		// Contact hardening (off in the ini): the menu item after "Contact shadow length" (0, then
		// Down) switches the game's shader to its contact hardening variant, live.
		press('5');
		press(VK_RIGHT); // back to width 3, the variant handed to the game
		dev->SetPixelShader(gp);
		dev->GetPixelShader(&cur);
		bool plainAgain = cur == gp;
		press('0');
		press(VK_DOWN);
		press(VK_RIGHT);
		dev->SetPixelShader(gp);
		IDirect3DPixelShader9* pcss = nullptr;
		dev->GetPixelShader(&pcss);
		UINT pcssSize = 0;
		if (pcss)
			pcss->GetFunction(nullptr, &pcssSize);
		printf("contact hardening on: device has another variant (%u bytes; width 3 %u): %s\n", pcssSize, got,
			plainAgain && pcss && pcss != gp && pcssSize > got ? "yes" : "NO");
		// Penumbra (the next item) up: once the setting has settled the variant is rebuilt.
		press(VK_DOWN);
		press(VK_RIGHT);
		dev->SetPixelShader(gp);
		dev->GetPixelShader(&cur);
		bool notYet = cur == pcss;
		Sleep(400);
		dev->SetPixelShader(gp);
		IDirect3DPixelShader9* rebuilt = nullptr;
		dev->GetPixelShader(&rebuilt);
		UINT rebuiltSize = 0;
		if (rebuilt)
			rebuilt->GetFunction(nullptr, &rebuiltSize);
		printf("contact hardening penumbra changed: same variant until it settles, then a rebuilt one (%u bytes): %s\n", rebuiltSize,
			notYet && rebuilt && rebuilt != pcss && rebuilt != gp && rebuiltSize == pcssSize ? "yes" : "NO");
		dev->SetPixelShader(gp);
		dev->GetPixelShader(&cur);
		printf("contact hardening: the rebuilt variant is kept (not rebuilt again): %s\n", cur == rebuilt ? "yes" : "NO");
		// Off again: the game's shader (plain width 3).
		press(VK_UP);
		press(VK_LEFT);
		dev->SetPixelShader(gp);
		dev->GetPixelShader(&cur);
		printf("contact hardening off: device has the game's shader: %s\n", cur == gp ? "yes" : "NO");
		Sleep(1200);
		dev->Present(NULL, NULL, NULL, NULL);
		char scale[32] = {};
		GetPrivateProfileStringA("SoftShadows", "PenumbraScale", "", scale, sizeof(scale), ini);
		printf("contact hardening saved to the ini (ContactHardening %d, PenumbraScale %s): %s\n",
			GetPrivateProfileIntA("SoftShadows", "ContactHardening", -1, ini), scale,
			GetPrivateProfileIntA("SoftShadows", "ContactHardening", -1, ini) == 0 && strcmp(scale, "0.045") == 0 ? "yes" : "NO");
		// Leave the test ini as it was.
		WritePrivateProfileStringA("SoftShadows", "ContactHardening", NULL, ini);
		WritePrivateProfileStringA("SoftShadows", "PenumbraScale", NULL, ini);
	} while (0);

	// [Debug] FrameDumpKey (F8 here; off by default): a second dump without the menu.
	{
		auto dumps = [] {
			int n = 0;
			WIN32_FIND_DATAA fd;
			HANDLE h = FindFirstFileA("scripts\\AC2ShadowOverhaul_FrameDump_*.txt", &fd);
			if (h != INVALID_HANDLE_VALUE)
			{
				do
					n += strstr(fd.cFileName, "_shaders") == nullptr && strstr(fd.cFileName, "_ps") == nullptr &&
						 strstr(fd.cFileName, "_vs") == nullptr;
				while (FindNextFileA(h, &fd));
				FindClose(h);
			}
			return n;
		};
		int before = dumps();
		Sleep(1100); // dump file names have one-second resolution
		g_keys[VK_F8] = true;
		dev->Present(NULL, NULL, NULL, NULL);
		g_keys[VK_F8] = false;
		for (int i = 0; i < 3; i++)
			dev->Present(NULL, NULL, NULL, NULL);
		int after = dumps();
		printf("frame dump hotkey: %d dump(s) before, %d after: %s\n", before, after, after == before + 1 ? "yes" : "NO");
	}
	autoDs->Release();
	hr = dev->Reset(&pp);
	printf("Reset 0x%08lX\n", hr);
	dev->Release();
	d3d->Release();
	return 0;
}
