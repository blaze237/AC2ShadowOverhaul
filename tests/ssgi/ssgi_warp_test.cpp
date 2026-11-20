// Runs the real SSGI passes (src/shaders/ssgi.hlsl) on synthetic scenes with D3D11's software
// renderer (WARP), so the maths is exercised without a D3D9 GPU. The HLSL is compiled for shader
// model 4 in backwards-compatibility mode with SSGI_D3D11 defined (pixel centres differ).
//
// Scene (view space, metres, camera at the origin looking down +z, 90 degree horizontal fov):
// a white sunlit floor (radiance 0.8) at y = -1.6, a red wall (radiance 1, 0.1, 0.1) at x = -3, a
// dark grey back wall (0.2) at z = 12 up to y = 4, sky above. Checks:
//  - floor next to the red wall gets red light, the foot of the red wall white light from the floor;
//  - at probes from 0.25 m to 5.5 m from the walls, the light is close to a ray-traced reference
//    that sees only the surfaces on screen (so it isn't confined to corners); the sky gets nothing;
//  - nothing gets more light than the brightest surface sends;
//  - temporal: with a static camera the frame count grows and the light stays; where last frame's
//    depth doesn't match it restarts; after the camera moves, a pattern stored in last frame's
//    history comes back at the reprojected positions (using the mod's own reprojection rows);
//  - the blur keeps the light and the final pass applies the intensity.
// Writes ssgi_warp_gi.png (the final GI light) to look at.
#include <windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <string>
#include <vector>
#include <utility>
#include "ssgi/gi.h"
#include "core/camera.h"
using namespace ssgi;
using namespace core;

static const UINT W = 1280, H = 720, HW = W / 2, HH = H / 2;
static const float P00 = 1.0f, P11 = (float)W / H, NEAR_ = 0.1f, FAR_ = 1000.0f;
static const float A = FAR_ / (FAR_ - NEAR_), B = -NEAR_ * FAR_ / (FAR_ - NEAR_);

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

typedef HRESULT(WINAPI* PFN_Compile)(const void*, SIZE_T, const char*, const D3D_SHADER_MACRO*, ID3DInclude*, const char*, const char*, UINT, UINT,
	ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI* PFN_Reflect)(const void*, SIZE_T, REFIID, void**);
static PFN_Compile g_compile;
static PFN_Reflect g_reflect;
static std::string g_src;
static const GUID kIID_Reflection = { 0x8d536ca1, 0x0cca, 0x4956, { 0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84 } };

static std::string g_geoSrc; // src/core/shaders/geoprep.hlsl: the geometry prep shared with ambient occlusion

static ID3DBlob* CompileEntry(const char* entry, const char* profile, bool geo = false)
{
	const D3D_SHADER_MACRO defs[] = { { "SSGI_D3D11", "1" }, { "GEO_D3D11", "1" }, { nullptr, nullptr } };
	ID3DBlob *code = nullptr, *err = nullptr;
	const std::string& src = geo ? g_geoSrc : g_src;
	HRESULT hr = g_compile(src.data(), src.size(), geo ? "geoprep.hlsl" : "ssgi.hlsl", defs, nullptr, entry, profile,
		D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
	if (FAILED(hr))
		printf("%s: compile failed: %s\n", entry, err ? (const char*)err->GetBufferPointer() : "?");
	return code;
}

// ---- Scene -------------------------------------------------------------------------------------

struct Hit
{
	float z = 0; // view z of the camera at `eye` (0 = sky)
	float rad[3] = {};
	int surface = -1; // 0 floor, 1 red wall, 2 back wall
	float world[3] = {};
};

// Camera at `eye` (world = view space of the first camera), looking down +z, no rotation.
static Hit Cast(float px, float py, const float eye[3])
{
	float ndcX = 2.0f * px / W - 1.0f, ndcY = 1.0f - 2.0f * py / H;
	float d[3] = { ndcX / P00, ndcY / P11, 1.0f };
	Hit best;
	float bestT = 1e30f;
	auto consider = [&](float t, int surf) {
		if (!(t > 0) || t >= bestT)
			return;
		float p[3] = { eye[0] + d[0] * t, eye[1] + d[1] * t, eye[2] + d[2] * t };
		if (surf == 0 && (p[0] < -3 || p[2] > 12))
			return;
		if (surf == 1 && (p[2] > 12 || p[1] < -1.6f))
			return;
		if (surf == 2 && (p[1] > 4 || p[0] < -3 || p[1] < -1.6f))
			return;
		bestT = t;
		best.surface = surf;
		memcpy(best.world, p, sizeof(p));
	};
	if (d[1] < 0)
		consider((-1.6f - eye[1]) / d[1], 0);
	if (d[0] < 0)
		consider((-3.0f - eye[0]) / d[0], 1);
	consider((12.0f - eye[2]) / d[2], 2);
	if (best.surface < 0)
		return best;
	best.z = bestT; // d.z = 1
	static const float rads[3][3] = { { 0.8f, 0.8f, 0.8f }, { 1.0f, 0.1f, 0.1f }, { 0.2f, 0.2f, 0.2f } };
	memcpy(best.rad, rads[best.surface], sizeof(best.rad));
	return best;
}

// Ground truth for the light arriving at p (normal n): cosine-weighted rays into the scene.
// onScreenOnly counts only surfaces the camera at the origin sees, the most a screen-space method
// can find.
static void Project(const float p[3], const float eye[3], float& px, float& py);
static void Reference(const float p[3], const float n[3], bool onScreenOnly, float out[3])
{
	const float eye[3] = { 0, 0, 0 };
	float t1[3], t2[3];
	if (fabsf(n[1]) > 0.9f)
		t1[0] = 1, t1[1] = 0, t1[2] = 0;
	else
		t1[0] = 0, t1[1] = 1, t1[2] = 0;
	float d0 = t1[0] * n[0] + t1[1] * n[1] + t1[2] * n[2];
	for (int c = 0; c < 3; c++)
		t1[c] -= d0 * n[c];
	float l = sqrtf(t1[0] * t1[0] + t1[1] * t1[1] + t1[2] * t1[2]);
	for (int c = 0; c < 3; c++)
		t1[c] /= l;
	t2[0] = n[1] * t1[2] - n[2] * t1[1];
	t2[1] = n[2] * t1[0] - n[0] * t1[2];
	t2[2] = n[0] * t1[1] - n[1] * t1[0];
	static const float rads[3][3] = { { 0.8f, 0.8f, 0.8f }, { 1.0f, 0.1f, 0.1f }, { 0.2f, 0.2f, 0.2f } };
	const int N = 64;
	out[0] = out[1] = out[2] = 0;
	for (int a = 0; a < N; a++)
		for (int b = 0; b < N; b++)
		{
			float u = (a + 0.5f) / N, v = (b + 0.5f) / N;
			float r = sqrtf(u), ph = 6.2831853f * v, cz = sqrtf(1 - u);
			float d[3];
			for (int c = 0; c < 3; c++)
				d[c] = t1[c] * r * cosf(ph) + t2[c] * r * sinf(ph) + n[c] * cz;
			float o[3] = { p[0] + n[0] * 1e-3f, p[1] + n[1] * 1e-3f, p[2] + n[2] * 1e-3f };
			float bestT = 1e30f;
			int surf = -1;
			auto consider = [&](float t, int s) {
				if (!(t > 0) || t >= bestT)
					return;
				float q[3] = { o[0] + d[0] * t, o[1] + d[1] * t, o[2] + d[2] * t };
				if (s == 0 && (q[0] < -3 || q[2] > 12))
					return;
				if (s == 1 && (q[2] > 12 || q[1] < -1.6f))
					return;
				if (s == 2 && (q[1] > 4 || q[0] < -3 || q[1] < -1.6f))
					return;
				bestT = t;
				surf = s;
			};
			if (d[1] != 0)
				consider((-1.6f - o[1]) / d[1], 0);
			if (d[0] != 0)
				consider((-3.0f - o[0]) / d[0], 1);
			if (d[2] != 0)
				consider((12.0f - o[2]) / d[2], 2);
			if (surf < 0)
				continue;
			if (onScreenOnly)
			{
				float q[3] = { o[0] + d[0] * bestT, o[1] + d[1] * bestT, o[2] + d[2] * bestT };
				if (q[2] <= NEAR_)
					continue;
				float px, py;
				Project(q, eye, px, py);
				if (px < 0 || py < 0 || px >= W || py >= H)
					continue;
				Hit h = Cast(px, py, eye);
				if (h.surface != surf || fabsf(h.z - q[2]) > 0.02f * q[2])
					continue;
			}
			for (int c = 0; c < 3; c++)
				out[c] += rads[surf][c] / (N * N);
		}
}

static void Project(const float p[3], const float eye[3], float& px, float& py)
{
	float x = p[0] - eye[0], y = p[1] - eye[1], z = p[2] - eye[2];
	px = (x / z * P00 + 1.0f) * 0.5f * W;
	py = (1.0f - y / z * P11) * 0.5f * H;
}

static Mat4 ViewProj(const float eye[3])
{
	Mat4 m = {};
	m.m[0][0] = P00;
	m.m[0][3] = -P00 * eye[0];
	m.m[1][1] = P11;
	m.m[1][3] = -P11 * eye[1];
	m.m[2][2] = A;
	m.m[2][3] = B - A * eye[2];
	m.m[3][2] = 1;
	m.m[3][3] = -eye[2];
	return m;
}

// ---- PNG (RGB, stored) -------------------------------------------------------------------------

static uint32_t Crc(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFF)
{
	for (size_t i = 0; i < n; i++)
	{
		c ^= p[i];
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
	}
	return c;
}
static void WritePng(const char* path, const std::vector<uint8_t>& rgb, UINT w, UINT h)
{
	std::vector<uint8_t> raw;
	for (UINT y = 0; y < h; y++)
	{
		raw.push_back(0);
		raw.insert(raw.end(), rgb.begin() + (size_t)y * w * 3, rgb.begin() + (size_t)(y + 1) * w * 3);
	}
	std::vector<uint8_t> z = { 0x78, 0x01 };
	uint32_t a = 1, b = 0;
	for (uint8_t v : raw)
	{
		a = (a + v) % 65521;
		b = (b + a) % 65521;
	}
	for (size_t i = 0; i < raw.size(); i += 65535)
	{
		size_t n = raw.size() - i < 65535 ? raw.size() - i : 65535;
		z.push_back(i + n == raw.size() ? 1 : 0);
		z.push_back(n & 0xFF);
		z.push_back(n >> 8);
		z.push_back(~n & 0xFF);
		z.push_back((~n >> 8) & 0xFF);
		z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
	}
	uint32_t ad = (b << 16) | a;
	for (int k = 3; k >= 0; k--)
		z.push_back((ad >> (8 * k)) & 0xFF);
	FILE* f = fopen(path, "wb");
	if (!f)
		return;
	auto be32 = [&](uint32_t v) {
		uint8_t q[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
		fwrite(q, 1, 4, f);
	};
	auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
		be32((uint32_t)data.size());
		std::vector<uint8_t> td(type, type + 4);
		td.insert(td.end(), data.begin(), data.end());
		fwrite(td.data(), 1, td.size(), f);
		be32(~Crc(td.data(), td.size()));
	};
	static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
	fwrite(sig, 1, 8, f);
	std::vector<uint8_t> ihdr = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 24), (uint8_t)(h >> 16),
		(uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0 };
	chunk("IHDR", ihdr);
	chunk("IDAT", z);
	chunk("IEND", {});
	fclose(f);
}

// ---- D3D11 -------------------------------------------------------------------------------------

static ID3D11Device* dev;
static ID3D11DeviceContext* ctx;

struct Target
{
	ID3D11Texture2D* tex = nullptr;
	ID3D11RenderTargetView* rtv = nullptr;
	ID3D11ShaderResourceView* srv = nullptr;
	UINT w = 0, h = 0;
};
static Target MakeTarget(UINT w, UINT h, DXGI_FORMAT fmt, const void* init = nullptr, UINT pitch = 0)
{
	Target t;
	t.w = w;
	t.h = h;
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = w;
	d.Height = h;
	d.MipLevels = d.ArraySize = 1;
	d.Format = fmt;
	d.SampleDesc.Count = 1;
	d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA sd = { init, pitch, 0 };
	dev->CreateTexture2D(&d, init ? &sd : nullptr, &t.tex);
	dev->CreateRenderTargetView(t.tex, nullptr, &t.rtv);
	dev->CreateShaderResourceView(t.tex, nullptr, &t.srv);
	return t;
}

static std::vector<float> ReadBack(const Target& t) // RGBA32F targets only
{
	D3D11_TEXTURE2D_DESC d;
	t.tex->GetDesc(&d);
	d.BindFlags = 0;
	d.Usage = D3D11_USAGE_STAGING;
	d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ID3D11Texture2D* st = nullptr;
	dev->CreateTexture2D(&d, nullptr, &st);
	ctx->CopyResource(st, t.tex);
	D3D11_MAPPED_SUBRESOURCE m;
	ctx->Map(st, 0, D3D11_MAP_READ, 0, &m);
	std::vector<float> out((size_t)t.w * t.h * 4);
	for (UINT y = 0; y < t.h; y++)
		memcpy(&out[(size_t)y * t.w * 4], (const uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)t.w * 16);
	ctx->Unmap(st, 0);
	st->Release();
	return out;
}

struct Shader
{
	ID3DBlob* code = nullptr;
	ID3D11PixelShader* ps = nullptr;
	std::vector<uint8_t> cb;
	std::vector<std::pair<std::string, UINT>> vars, textures, samplers;
};
static bool Load(Shader& s, const char* entry, bool geo = false)
{
	s.code = CompileEntry(entry, "ps_4_0", geo);
	if (!s.code)
		return false;
	dev->CreatePixelShader(s.code->GetBufferPointer(), s.code->GetBufferSize(), nullptr, &s.ps);
	ID3D11ShaderReflection* r = nullptr;
	if (FAILED(g_reflect(s.code->GetBufferPointer(), s.code->GetBufferSize(), kIID_Reflection, (void**)&r)) || !r)
		return false;
	D3D11_SHADER_DESC sd = {};
	r->GetDesc(&sd);
	for (UINT i = 0; i < sd.BoundResources; i++)
	{
		D3D11_SHADER_INPUT_BIND_DESC b = {};
		r->GetResourceBindingDesc(i, &b);
		if (b.Type == D3D_SIT_TEXTURE)
			s.textures.push_back({ b.Name, b.BindPoint });
		if (b.Type == D3D_SIT_SAMPLER)
			s.samplers.push_back({ b.Name, b.BindPoint });
	}
	if (ID3D11ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByName("$Globals"))
	{
		D3D11_SHADER_BUFFER_DESC bd = {};
		if (SUCCEEDED(cb->GetDesc(&bd)))
		{
			s.cb.assign((bd.Size + 15) & ~15u, 0);
			for (UINT i = 0; i < bd.Variables; i++)
			{
				D3D11_SHADER_VARIABLE_DESC vd = {};
				cb->GetVariableByIndex(i)->GetDesc(&vd);
				s.vars.push_back({ vd.Name, vd.StartOffset });
			}
		}
	}
	r->Release();
	return true;
}

static ID3D11VertexShader* g_vs;
static ID3D11InputLayout* g_layout;
static ID3D11Buffer* g_vb;
static ID3D11SamplerState *g_point, *g_linear;

// Constants as src/gi.cpp sets them (c0..c10, by name).
struct Consts
{
	float c[11][4];
};
static const char* const kConstNames[11] = { "c_Full", "c_Half", "c_Proj", "c_Proj2", "c_Trace", "c_Trace2", "c_Reproj0", "c_Reproj1",
	"c_Reproj2", "c_Temporal", "c_Blur" };

static void Pass(Shader& s, const Consts& k, std::vector<const Target*> outs, std::vector<std::pair<const char*, const Target*>> inputs)
{
	for (auto& v : s.vars)
		for (int i = 0; i < 11; i++)
			if (v.first == kConstNames[i] && v.second + 16 <= s.cb.size())
				memcpy(&s.cb[v.second], k.c[i], 16);
	ID3D11Buffer* cb = nullptr;
	if (!s.cb.empty())
	{
		D3D11_BUFFER_DESC cbd = { (UINT)s.cb.size(), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
		D3D11_SUBRESOURCE_DATA cd = { s.cb.data(), 0, 0 };
		dev->CreateBuffer(&cbd, &cd, &cb);
	}
	ID3D11ShaderResourceView* none[8] = {};
	ctx->PSSetShaderResources(0, 8, none);
	ID3D11RenderTargetView* rtvs[2] = { outs[0]->rtv, outs.size() > 1 ? outs[1]->rtv : nullptr };
	ctx->OMSetRenderTargets((UINT)outs.size(), rtvs, nullptr);
	for (auto& t : s.textures)
		for (auto& in : inputs)
			if (t.first == in.first)
				ctx->PSSetShaderResources(t.second, 1, &in.second->srv);
	for (auto& sm : s.samplers)
	{
		bool linear = sm.first == "s_Colour" || sm.first == "s_Out";
		ctx->PSSetSamplers(sm.second, 1, linear ? &g_linear : &g_point);
	}
	D3D11_VIEWPORT vp = { 0, 0, (float)outs[0]->w, (float)outs[0]->h, 0, 1 };
	ctx->RSSetViewports(1, &vp);
	UINT stride = 16, offset = 0;
	ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &offset);
	ctx->IASetInputLayout(g_layout);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ctx->VSSetShader(g_vs, nullptr, 0);
	ctx->PSSetShader(s.ps, nullptr, 0);
	ctx->PSSetConstantBuffers(0, 1, &cb);
	ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
	ctx->Draw(3, 0);
	ctx->OMSetRenderTargets(0, nullptr, nullptr);
	if (cb)
		cb->Release();
}

static const float* At(const std::vector<float>& img, UINT w, int x, int y) { return &img[((size_t)y * w + x) * 4]; }

// Average of a half-res image over a small window around the half-res texel of full-res pixel (px, py).
static void Avg(const std::vector<float>& img, float px, float py, float out[4], int r = 2)
{
	int cx = (int)(px / 2), cy = (int)(py / 2);
	for (int k = 0; k < 4; k++)
		out[k] = 0;
	int n = 0;
	for (int y = cy - r; y <= cy + r; y++)
		for (int x = cx - r; x <= cx + r; x++)
			if (x >= 0 && y >= 0 && x < (int)HW && y < (int)HH)
			{
				const float* v = At(img, HW, x, y);
				for (int k = 0; k < 4; k++)
					out[k] += v[k];
				n++;
			}
	for (int k = 0; k < 4; k++)
		out[k] /= n ? n : 1;
}


int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	for (int k = 0; k < 2; k++)
	{
		FILE* f = fopen(k ? "../../src/core/shaders/geoprep.hlsl" : "../../src/ssgi/shaders/ssgi.hlsl", "rb");
		if (!f)
			return printf("run from tests/ssgi\n"), 1;
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
			(k ? g_geoSrc : g_src).append(buf, n);
		fclose(f);
	}
	HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
	g_compile = dc ? (PFN_Compile)GetProcAddress(dc, "D3DCompile") : nullptr;
	g_reflect = dc ? (PFN_Reflect)GetProcAddress(dc, "D3DReflect") : nullptr;
	if (!g_compile || !g_reflect)
		return printf("d3dcompiler_47 not available\n"), 1;
	D3D_FEATURE_LEVEL fl;
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx)))
		return printf("no WARP device\n"), 1;

	ID3DBlob* vsCode = CompileEntry("VS_Fullscreen", "vs_4_0");
	Shader geoPrep, radiance, trace, temporal, blur, debug;
	if (!vsCode || !Load(geoPrep, "PS_GeoPrep", true) || !Load(radiance, "PS_Radiance") || !Load(trace, "PS_Trace") || !Load(temporal, "PS_Temporal") || !Load(blur, "PS_Blur") ||
		!Load(debug, "PS_Debug"))
		return 1;
	dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &g_vs);
	D3D11_INPUT_ELEMENT_DESC el = { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
	dev->CreateInputLayout(&el, 1, vsCode->GetBufferPointer(), vsCode->GetBufferSize(), &g_layout);
	const float tri[3][4] = { { -1, -1, 0, 1 }, { -1, 3, 0, 1 }, { 3, -1, 0, 1 } };
	D3D11_BUFFER_DESC bd = { sizeof(tri), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
	D3D11_SUBRESOURCE_DATA sd = { tri, 0, 0 };
	dev->CreateBuffer(&bd, &sd, &g_vb);
	D3D11_SAMPLER_DESC smp = {};
	smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	smp.MaxLOD = D3D11_FLOAT32_MAX;
	dev->CreateSamplerState(&smp, &g_point);
	smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	dev->CreateSamplerState(&smp, &g_linear);

	// Frame 0: camera at the origin.
	const float eye0[3] = { 0, 0, 0 };
	std::vector<float> depth((size_t)W * H), colour((size_t)W * H * 4);
	for (UINT y = 0; y < H; y++)
		for (UINT x = 0; x < W; x++)
		{
			Hit h = Cast((float)x, (float)y, eye0);
			depth[(size_t)y * W + x] = h.z > 0 ? A + B / h.z : 1.0f;
			float* c = &colour[((size_t)y * W + x) * 4];
			c[0] = h.rad[0];
			c[1] = h.rad[1];
			c[2] = h.rad[2];
			c[3] = 1;
		}
	const DXGI_FORMAT F = DXGI_FORMAT_R32G32B32A32_FLOAT;
	Target tDepth = MakeTarget(W, H, DXGI_FORMAT_R32_FLOAT, depth.data(), W * 4);
	Target tColour = MakeTarget(W, H, F, colour.data(), W * 16);
	Target tGeo[2] = { MakeTarget(HW, HH, F), MakeTarget(HW, HH, F) }, tRad = MakeTarget(HW, HH, F), tRaw = MakeTarget(HW, HH, F);
	Target tHist[2] = { MakeTarget(HW, HH, F), MakeTarget(HW, HH, F) }, tTmp = MakeTarget(HW, HH, F), tOut = MakeTarget(HW, HH, F);
	Target tDebug = MakeTarget(W, H, F);

	// The mod's defaults; R, MS, SL and ST in the environment override radius, maximum screen radius
	// (x height), slices and steps, for tuning.
	auto envf = [](const char* name, float def) { return getenv(name) ? (float)atof(getenv(name)) : def; };
	const float radius = envf("R", 10.0f), maxScreen = envf("MS", 1.0f), thickness = 0.5f, intensity = 1.0f;
	float rows[3][4];
	IdentityRows(W, H, rows);
	Consts k = { {
		{ (float)W, (float)H, 1.0f / W, 1.0f / H },
		{ (float)HW, (float)HH, 1.0f / HW, 1.0f / HH },
		{ A, B, P00, P11 },
		{ 0, 0, 0, 4.0f },
		{ radius, 0.5f * P00 * W, maxScreen * H, thickness },
		{ envf("SL", 3), envf("ST", 8), intensity, 0 },
		{ rows[0][0], rows[0][1], rows[0][2], rows[0][3] },
		{ rows[1][0], rows[1][1], rows[1][2], rows[1][3] },
		{ rows[2][0], rows[2][1], rows[2][2], rows[2][3] },
		{ 0.1f, 0.05f, 0.0f, 10.0f },
		{ 1.5f, 0, 0, 0 },
	} };

	// The geometry prep shared with AO (normals + depth), then SSGI's radiance pass, as the mod runs them.
	auto prep = [&](const Target& geo, const Target& depthT) {
		Pass(geoPrep, k, { &geo }, { { "s_Depth", &depthT } });
		Pass(radiance, k, { &tRad }, { { "s_Colour", &tColour } });
	};

	// ---- Prep + trace, averaged over 8 noise frames (the game averages over frames too)
	prep(tGeo[0], tDepth);
	std::vector<float> geo = ReadBack(tGeo[0]);

	std::vector<float> gi((size_t)HW * HH * 4, 0.0f);
	const int frames = 8;
	for (int fr = 0; fr < frames; fr++)
	{
		k.c[3][2] = (float)fr;
		Pass(trace, k, { &tRaw }, { { "s_Geo", &tGeo[0] }, { "s_Rad", &tRad } });
		std::vector<float> raw = ReadBack(tRaw);
		for (size_t i = 0; i < gi.size(); i++)
			gi[i] += raw[i] / frames;
	}

	// Normals: floor up, red wall facing +x, back wall facing the camera.
	{
		float px, py;
		const float floorPt[3] = { 1.0f, -1.6f, 5.0f }, wallPt[3] = { -3.0f, 0.5f, 6.0f }, backPt[3] = { 1.0f, 1.0f, 12.0f };
		const float* pts[3] = { floorPt, wallPt, backPt };
		const float expect[3][3] = { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } };
		const char* names[3] = { "floor", "red wall", "back wall" };
		for (int s = 0; s < 3; s++)
		{
			Project(pts[s], eye0, px, py);
			const float* g = At(geo, HW, (int)(px / 2), (int)(py / 2));
			float dot = g[0] * expect[s][0] + g[1] * expect[s][1] + g[2] * expect[s][2];
			CHECK(dot > 0.98f, "%s normal (%.2f, %.2f, %.2f)", names[s], g[0], g[1], g[2]);
		}
	}

	auto report = [&](const char* what, const float p[3], float out[4]) {
		float px, py;
		Project(p, eye0, px, py);
		Avg(gi, px, py, out);
		printf("  %-34s px (%4.0f, %4.0f): GI (%.3f, %.3f, %.3f), open %.2f\n", what, px, py, out[0], out[1], out[2], out[3]);
	};
	printf("trace (8 frames averaged):\n");
	float nearWall[4], wallFoot[4], wallHigh[4], sky[4];
	const float pNear[3] = { -2.75f, -1.6f, 6.0f }, pFoot[3] = { -3.0f, -1.35f, 7.0f },
				pHigh[3] = { -3.0f, 2.5f, 7.0f }, pSky[3] = { 1.0f, 8.0f, 11.0f };
	report("floor 0.25 m from the red wall", pNear, nearWall);
	report("red wall 0.25 m above the floor", pFoot, wallFoot);
	report("red wall 4 m above the floor", pHigh, wallHigh);
	report("sky", pSky, sky);
	CHECK(nearWall[0] > 4 * nearWall[1] && nearWall[0] > 4 * nearWall[2], "floor by the red wall isn't red (%.3f, %.3f, %.3f)", nearWall[0],
		nearWall[1], nearWall[2]);
	CHECK(wallFoot[1] > 0.15f && fabsf(wallFoot[1] - wallFoot[2]) < 0.02f && wallFoot[0] < 1.3f * wallFoot[1] + 0.02f,
		"foot of the red wall should get white light from the floor (%.3f, %.3f, %.3f)", wallFoot[0], wallFoot[1], wallFoot[2]);
	CHECK(wallHigh[1] < 0.5f * wallFoot[1], "high on the wall gets as much floor light as its foot (%.3f vs %.3f)", wallHigh[1], wallFoot[1]);
	CHECK(sky[0] == 0 && sky[1] == 0 && sky[2] == 0, "sky gets light");

	// Against ray-traced light. A screen-space method can only find surfaces on screen, so it is held
	// to that reference: close to it near the surfaces, and still a good part of it metres away (the
	// light must not be confined to corners). The red wall is infinitely high, so the floor far from
	// it should still be clearly red.
	{
		printf("  compared with ray-traced light (all surfaces / only those on screen):\n");
		const float up[3] = { 0, 1, 0 }, right[3] = { 1, 0, 0 };
		struct
		{
			const char* what;
			float p[3];
			const float* n;
			float minShare;
		} probes[] = {
			{ "floor 0.25 m from the wall", { -2.75f, -1.6f, 6.0f }, up, 0.8f },
			{ "floor 1 m from the wall", { -2.0f, -1.6f, 6.0f }, up, 0.75f },
			{ "floor 2.5 m from the wall", { -0.5f, -1.6f, 6.0f }, up, 0.65f },
			{ "floor 5.5 m from the wall", { 2.5f, -1.6f, 6.0f }, up, 0.35f },
			{ "floor 3 m before the back wall", { 0.0f, -1.6f, 9.0f }, up, 0.65f },
			{ "wall 1 m above the floor", { -3.0f, -0.6f, 7.0f }, right, 0.75f },
			{ "wall 4 m above the floor", { -3.0f, 2.4f, 7.0f }, right, 0.45f },
		};
		for (auto& pr : probes)
		{
			float px, py, g[4], all[3], vis[3];
			Project(pr.p, eye0, px, py);
			Avg(gi, px, py, g);
			Reference(pr.p, pr.n, false, all);
			Reference(pr.p, pr.n, true, vis);
			printf("    %-32s SSGI (%.3f, %.3f, %.3f)  all (%.3f, %.3f, %.3f)  on screen (%.3f, %.3f, %.3f)\n", pr.what, g[0], g[1], g[2],
				all[0], all[1], all[2], vis[0], vis[1], vis[2]);
			for (int c = 0; c < 2; c++)
				if (vis[c] > 0.03f)
					CHECK(g[c] > pr.minShare * vis[c] && g[c] < 1.15f * vis[c] + 0.01f, "%s: %s light %.3f, ray-traced on screen %.3f (expected %.0f-115 %%)",
						pr.what, c ? "white" : "red", g[c], vis[c], 100 * pr.minShare);
		}
	}
	float maxGi = 0;
	bool nan = false;
	for (size_t i = 0; i < gi.size(); i += 4)
		for (int c = 0; c < 3; c++)
		{
			nan |= !(gi[i + c] == gi[i + c]) || gi[i + c] < 0;
			maxGi = fmaxf(maxGi, gi[i + c]);
		}
	CHECK(!nan, "negative or NaN light");
	CHECK(maxGi <= 1.0f, "more light (%.3f) than the brightest surface sends (1.0)", maxGi);

	// ---- Temporal, static camera: frame 1 has no history, frame 2 accumulates.
	k.c[9][2] = 0; // no history
	Pass(temporal, k, { &tHist[0] }, { { "s_Geo", &tGeo[0] }, { "s_Raw", &tRaw }, { "s_Hist", &tHist[1] }, { "s_PrevGeo", &tGeo[1] } });
	prep(tGeo[1], tDepth); // same frame again
	k.c[9][2] = 1;
	Pass(temporal, k, { &tHist[1] }, { { "s_Geo", &tGeo[1] }, { "s_Raw", &tRaw }, { "s_Hist", &tHist[0] }, { "s_PrevGeo", &tGeo[0] } });
	std::vector<float> h1 = ReadBack(tHist[1]);
	{
		float px, py, v[4];
		Project(pNear, eye0, px, py);
		Avg(h1, px, py, v, 0);
		CHECK(fabsf(v[3] - 2) < 1e-3f, "static camera: frame count %.2f, expected 2", v[3]);
		std::vector<float> rawImg = ReadBack(tRaw);
		const float* raw = At(rawImg, HW, (int)(px / 2), (int)(py / 2));
		CHECK(fabsf(v[0] - raw[0]) < 1e-3f, "static camera: accumulated %.4f vs %.4f", v[0], raw[0]);
	}
	// Last frame saw something else there (depth 30% off): restart.
	{
		std::vector<float> fake = ReadBack(tGeo[0]);
		for (size_t i = 3; i < fake.size(); i += 4)
			fake[i] *= 1.3f;
		Target tFake = MakeTarget(HW, HH, F, fake.data(), HW * 16);
		Pass(temporal, k, { &tHist[1] }, { { "s_Geo", &tGeo[1] }, { "s_Raw", &tRaw }, { "s_Hist", &tHist[0] }, { "s_PrevGeo", &tFake } });
		std::vector<float> h2 = ReadBack(tHist[1]);
		float px, py, v[4];
		Project(pNear, eye0, px, py);
		Avg(h2, px, py, v, 0);
		CHECK(fabsf(v[3] - 1) < 1e-3f, "depth mismatch: frame count %.2f, expected 1 (history rejected)", v[3]);
	}

	// ---- Temporal after a camera move: last frame's history holds a pattern (its own world position);
	// it must come back at the reprojected position.
	{
		const float eye1[3] = { 0.4f, 0.1f, 0.8f };
		std::vector<float> depth1((size_t)W * H);
		for (UINT y = 0; y < H; y++)
			for (UINT x = 0; x < W; x++)
			{
				Hit h = Cast((float)x, (float)y, eye1);
				depth1[(size_t)y * W + x] = h.z > 0 ? A + B / h.z : 1.0f;
			}
		Target tDepth1 = MakeTarget(W, H, DXGI_FORMAT_R32_FLOAT, depth1.data(), W * 4);
		// History of frame 0: rgb = world position of each texel's pixel, count 5; geo of frame 0.
		std::vector<float> pattern((size_t)HW * HH * 4, 0.0f);
		for (UINT y = 0; y < HH; y++)
			for (UINT x = 0; x < HW; x++)
			{
				Hit h = Cast(2.0f * x, 2.0f * y, eye0);
				float* p = &pattern[((size_t)y * HW + x) * 4];
				if (h.z > 0)
				{
					p[0] = h.world[0];
					p[1] = h.world[1];
					p[2] = h.world[2];
					p[3] = 5;
				}
			}
		Target tPattern = MakeTarget(HW, HH, F, pattern.data(), HW * 16);
		prep(tGeo[0], tDepth);
		prep(tGeo[1], tDepth1);
		// The new frame's "trace" is all zeros, the blend weight is 0.2 (count 5 -> 6, max(1/6, 0.1)):
		// output = history x 5/6.
		std::vector<float> zeros((size_t)HW * HH * 4, 0.0f);
		Target tZero = MakeTarget(HW, HH, F, zeros.data(), HW * 16);
		float r[3][4];
		ReprojectionRows(ViewProj(eye0), ViewProj(eye1), W, H, r);
		Consts k1 = k;
		for (int i = 0; i < 3; i++)
			memcpy(k1.c[6 + i], r[i], 16);
		k1.c[9][2] = 1;
		Pass(temporal, k1, { &tHist[0] }, { { "s_Geo", &tGeo[1] }, { "s_Raw", &tZero }, { "s_Hist", &tPattern }, { "s_PrevGeo", &tGeo[0] } });
		std::vector<float> out = ReadBack(tHist[0]);
		int tested = 0, bad = 0, rejected = 0;
		float worst = 0;
		for (UINT y = 4; y < HH - 4; y += 7)
			for (UINT x = 4; x < HW - 4; x += 7)
			{
				Hit h1 = Cast(2.0f * x, 2.0f * y, eye1);
				if (h1.z <= 0)
					continue;
				// Visible in frame 0 at the same surface, away from edges?
				float px0, py0;
				Project(h1.world, eye0, px0, py0);
				if (px0 < 8 || py0 < 8 || px0 > W - 8 || py0 > H - 8)
					continue;
				Hit h0 = Cast(px0, py0, eye0);
				if (h0.surface != h1.surface || fabsf(h0.z - (h1.world[2] - eye0[2])) > 0.01f)
					continue;
				bool edge = false;
				for (int dy = -3; dy <= 3 && !edge; dy += 3)
					for (int dx = -3; dx <= 3 && !edge; dx += 3)
						edge = Cast(px0 + dx, py0 + dy, eye0).surface != h0.surface || Cast(2.0f * x + dx, 2.0f * y + dy, eye1).surface != h1.surface;
				if (edge)
					continue;
				const float* v = At(out, HW, x, y);
				tested++;
				if (fabsf(v[3] - 6) > 1e-3f)
				{
					rejected++;
					continue;
				}
				// World position from the pattern, scaled by 5/6; within a texel's worth of distance.
				float err = 0;
				for (int c = 0; c < 3; c++)
					err = fmaxf(err, fabsf(v[c] / (5.0f / 6.0f) - h1.world[c]));
				float texel = 2.0f * h0.z / (P00 * W) * 2.0f; // metres per half-res texel (x2 for slanted floors)
				if (err > 1.5f * texel + 0.01f)
					bad++;
				worst = fmaxf(worst, err / texel);
			}
		printf("camera move: %d texels tested, %d rejected, %d off target (worst %.2f texels)\n", tested, rejected, bad, worst);
		CHECK(tested > 500, "too few texels to test the reprojection (%d)", tested);
		CHECK(rejected < tested / 50 + 1, "%d of %d visible texels lost their history after the camera moved", rejected, tested);
		CHECK(bad == 0, "%d texels took their history from the wrong place", bad);
	}

	// ---- Blur + intensity: one run of the full chain on frame 0's trace.
	{
		prep(tGeo[0], tDepth);
		std::vector<float> avg(gi);
		Target tAvg = MakeTarget(HW, HH, F, avg.data(), HW * 16);
		Pass(blur, k, { &tTmp }, { { "s_Geo", &tGeo[0] }, { "s_Src", &tAvg } });
		Consts kd = k;
		kd.c[10][0] = 0;
		kd.c[10][1] = 1.5f;
		kd.c[10][2] = 1;
		kd.c[5][2] = 2.0f; // intensity 2
		Pass(blur, kd, { &tOut }, { { "s_Geo", &tGeo[0] }, { "s_Src", &tTmp } });
		std::vector<float> fin = ReadBack(tOut);
		float px, py, a[4], b[4];
		Project(pNear, eye0, px, py);
		Avg(gi, px, py, a, 1);
		Avg(fin, px, py, b, 1);
		CHECK(fabsf(b[0] - 2 * a[0]) < 0.25f * a[0], "blurred light x intensity 2: %.3f vs 2 x %.3f", b[0], a[0]);
		printf("blur: by the wall %.3f -> %.3f with intensity 2\n", a[0], b[0]);
		float mid0[4], mid1[4];
		const float pMid[3] = { -0.5f, -1.6f, 6.0f };
		Project(pMid, eye0, px, py);
		Avg(gi, px, py, mid0, 1);
		Avg(fin, px, py, mid1, 1);
		CHECK(fabsf(mid1[0] - 2 * mid0[0]) < 0.25f * mid0[0], "blurred light x intensity 2 on open floor: %.3f vs 2 x %.3f", mid1[0], mid0[0]);
		// Debug view 1 reads it back at full res.
		Consts kv = kd;
		kv.c[5][3] = 1;
		Pass(debug, kv, { &tDebug }, { { "s_Out", &tOut }, { "s_Normals", &tGeo[0] } });
		std::vector<float> dbg = ReadBack(tDebug);
		Project(pNear, eye0, px, py);
		const float* d = At(dbg, W, (int)px, (int)py);
		CHECK(fabsf(d[0] - b[0]) < 0.25f * b[0] + 0.02f, "debug view shows %.3f, the GI texture holds %.3f", d[0], b[0]);
		std::vector<uint8_t> png((size_t)HW * HH * 3);
		for (size_t i = 0; i < (size_t)HW * HH; i++)
			for (int c = 0; c < 3; c++)
				png[i * 3 + c] = (uint8_t)fminf(255.0f, 255.0f * powf(fmaxf(fin[i * 4 + c] * 0.5f, 0.0f), 1 / 2.2f));
		WritePng("ssgi_warp_gi.png", png, HW, HH);
	}

	printf(g_fail ? "%d FAILURES\n" : "all SSGI shader checks passed\n", g_fail);
	return g_fail ? 1 : 0;
}
