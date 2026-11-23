// Runs the real TAA shaders (src/shaders/taa.hlsl) with D3D11's software renderer (WARP) on a
// synthetic scene, frame after frame, the way the plugin drives them in the game: each frame is
// rendered with the plugin's jitter sequence (a CPU ray caster stands in for the game), the camera
// matrices go through the plugin's own C++ (CameraFromMatrices / ReprojectionMatrix, fed
// game-style g_WorldViewProj and g_World), and the result is compared with a 64-sample
// supersampled reference image.
//
// Scene: a checkerboard ground, thin poles (some under a pixel wide), a box, a very bright (HDR)
// panel, sky. Cases:
//   still   - static camera: the result must converge close to the reference, far closer than a
//             single sample per pixel, and stop shimmering
//   pan     - camera turning and moving sideways: reprojection must keep the result close to the
//             reference (and clearly better than without reprojection)
//   object  - static camera, a box sliding across: no ghost trail where the box was
//   hdr     - bright values survive without NaNs and the output pass's round trip is exact
//   light   - static camera, the light on everything x 1.5 at once (a light switched on), and the
//             same change settling the way SSGI's bounce light does: its temporal pass blends at
//             least 10 % per frame without TAA (tests/ssgi: 22 frames to 90 %) and 20 % while TAA
//             averages (11 frames). How many frames TAA's output lags behind its input: with 10 %
//             the two histories together take half as long again as SSGI alone without TAA, with
//             20 % about as long (gi.cpp doubles the blend while TAA averages).
// Writes taa_warp_<case>.png (left: one sample per pixel, middle: TAA, right: reference).
//
// The HLSL is compiled for shader model 4 in backwards-compatibility mode with TAA_D3D11 defined
// (pixel centres differ from D3D9's VPOS).
#include "core/camera.h"
using namespace core;
#include <windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <string>
#include <vector>
#include <stdlib.h>
#include <algorithm>

static const UINT W = 480, H = 270;
static const float NEAR_ = 0.1f, FAR_ = 1779.0f;
static const double kA = FAR_ / (FAR_ - NEAR_), kB = -NEAR_ * FAR_ / (FAR_ - NEAR_);
static const double kP00 = 1.317, kP11 = 1.317 * 16.0 / 9.0;

typedef HRESULT(WINAPI* PFN_Compile)(const void*, SIZE_T, const char*, const D3D_SHADER_MACRO*, ID3DInclude*, const char*, const char*, UINT, UINT,
	ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI* PFN_Reflect)(const void*, SIZE_T, REFIID, void**);
static PFN_Compile g_compile;
static PFN_Reflect g_reflect;
static std::string g_src;
static const GUID kIID_Reflection = { 0x8d536ca1, 0x0cca, 0x4956, { 0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84 } };

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

// ---- Scene -------------------------------------------------------------------------------------

struct Cam
{
	double eye[3], right[3], up[3], fwd[3];
};

static Cam MakeCam(double x, double y, double z, double yaw, double pitch)
{
	Cam c;
	c.eye[0] = x;
	c.eye[1] = y;
	c.eye[2] = z;
	// World: x right, y up, z forward at yaw 0 (left-handed like D3D).
	double cy = cos(yaw), sy = sin(yaw), cp = cos(pitch), sp = sin(pitch);
	double f[3] = { sy * cp, -sp, cy * cp }, r[3] = { cy, 0, -sy };
	double u[3] = { f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0] };
	memcpy(c.fwd, f, sizeof(f));
	memcpy(c.right, r, sizeof(r));
	memcpy(c.up, u, sizeof(u));
	return c;
}

struct Box
{
	double lo[3], hi[3];
	float col[3];
};
static std::vector<Box> g_boxes;
static double g_movingX = 0; // the sliding box's x offset (object case)
static bool g_follow = false; // follow case: a box that moves with the camera (like the player)
static double g_followEye[3];

static void BuildScene(bool moving)
{
	g_boxes.clear();
	// Thin poles at increasing distances: 2 cm to 10 cm wide.
	for (int i = 0; i < 8; i++)
	{
		double x = -3.0 + i * 0.85, z = 6.0 + i * 2.5, w = 0.02 + 0.012 * i;
		g_boxes.push_back({ { x, 0, z }, { x + w, 2.5, z + w }, { 0.85f, 0.2f, 0.15f } });
	}
	g_boxes.push_back({ { 1.0, 0, 9.0 }, { 2.2, 1.2, 10.0 }, { 0.3f, 0.5f, 0.85f } });
	// A very bright panel (a sunlit window, an HDR highlight).
	g_boxes.push_back({ { -2.2, 1.0, 12.0 }, { -1.4, 1.6, 12.1 }, { 12.0f, 11.0f, 9.0f } });
	if (moving)
		g_boxes.push_back({ { -0.6 + g_movingX, 0, 5.0 }, { 0.4 + g_movingX, 1.6, 5.5 }, { 0.95f, 0.85f, 0.2f } });
	if (g_follow)
	{
		const double* e = g_followEye;
		g_boxes.push_back({ { e[0] - 0.35, 0, e[2] + 3.0 }, { e[0] + 0.35, 1.75, e[2] + 3.4 }, { 0.95f, 0.85f, 0.2f } });
	}
}

static bool RayBox(const double o[3], const double d[3], const Box& b, double& t)
{
	double t0 = 0, t1 = 1e30;
	for (int k = 0; k < 3; k++)
	{
		double inv = 1.0 / d[k], a = (b.lo[k] - o[k]) * inv, c = (b.hi[k] - o[k]) * inv;
		if (a > c)
			std::swap(a, c);
		t0 = a > t0 ? a : t0;
		t1 = c < t1 ? c : t1;
	}
	if (t0 > t1 || t0 <= 1e-6)
		return false;
	t = t0;
	return true;
}

// Colour and view depth (0 = sky) at screen position (sx, sy) in pixels (D3D9 centres at integers).
static void Shade(const Cam& c, double sx, double sy, float col[3], double& z)
{
	double nx = 2.0 * sx / W - 1.0, ny = 1.0 - 2.0 * sy / H;
	double d[3];
	for (int k = 0; k < 3; k++)
		d[k] = c.right[k] * nx / kP00 + c.up[k] * ny / kP11 + c.fwd[k]; // forward component 1: t = view z
	double best = 1e30, t;
	const Box* hit = nullptr;
	for (const Box& b : g_boxes)
		if (RayBox(c.eye, d, b, t) && t < best)
		{
			best = t;
			hit = &b;
		}
	bool ground = false;
	if (d[1] < 0)
	{
		t = -c.eye[1] / d[1];
		if (t < best)
		{
			best = t;
			ground = true;
			hit = nullptr;
		}
	}
	if (ground)
	{
		double x = c.eye[0] + d[0] * best, zz = c.eye[2] + d[2] * best;
		bool chk = ((int)floor(x / 0.5) + (int)floor(zz / 0.5)) & 1;
		float v = chk ? 0.85f : 0.08f;
		col[0] = v;
		col[1] = v * 0.95f;
		col[2] = v * 0.9f;
		z = best;
	}
	else if (hit)
	{
		memcpy(col, hit->col, 12);
		z = best;
	}
	else
	{
		col[0] = 0.35f;
		col[1] = 0.55f + 0.2f * (float)ny;
		col[2] = 0.95f;
		z = 0;
	}
	if (z > FAR_)
		z = 0;
}

// The frame the game would render with jitter o: pixel q shows the scene at q - o.
static void RenderFrame(const Cam& c, float ox, float oy, std::vector<float>& rgba, std::vector<float>& zw)
{
	rgba.resize((size_t)W * H * 4);
	zw.resize((size_t)W * H);
	for (UINT y = 0; y < H; y++)
		for (UINT x = 0; x < W; x++)
		{
			float col[3];
			double z;
			Shade(c, x - ox, y - oy, col, z);
			float* p = &rgba[((size_t)y * W + x) * 4];
			p[0] = col[0];
			p[1] = col[1];
			p[2] = col[2];
			p[3] = 1;
			zw[(size_t)y * W + x] = z > 0 ? (float)(kA + kB / z) : 1.0f;
		}
}

static void RenderReference(const Cam& c, std::vector<float>& rgb)
{
	rgb.assign((size_t)W * H * 3, 0.0f);
	const int N = 8;
	for (UINT y = 0; y < H; y++)
		for (UINT x = 0; x < W; x++)
		{
			float acc[3] = {};
			for (int j = 0; j < N; j++)
				for (int i = 0; i < N; i++)
				{
					float col[3];
					double z;
					Shade(c, x - 0.5 + (i + 0.5) / N, y - 0.5 + (j + 0.5) / N, col, z);
					for (int k = 0; k < 3; k++)
						acc[k] += col[k];
				}
			for (int k = 0; k < 3; k++)
				rgb[((size_t)y * W + x) * 3 + k] = acc[k] / (N * N);
		}
}

// Game-style constants: VP rows (clip = VP * world position), a world matrix, WVP = VP * World.
static Camera CameraThroughPlugin(const Cam& c, int seed)
{
	double V[4][4] = {}, P[4][4] = {}, VP[4][4], Wm[4][4] = {}, WVP[4][4];
	for (int k = 0; k < 3; k++)
	{
		V[0][k] = c.right[k];
		V[1][k] = c.up[k];
		V[2][k] = c.fwd[k];
	}
	auto dot = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
	V[0][3] = -dot(c.right, c.eye);
	V[1][3] = -dot(c.up, c.eye);
	V[2][3] = -dot(c.fwd, c.eye);
	V[3][3] = 1;
	P[0][0] = kP00;
	P[1][1] = kP11;
	P[2][2] = kA;
	P[2][3] = kB;
	P[3][2] = 1;
	for (int r = 0; r < 4; r++)
		for (int cc = 0; cc < 4; cc++)
			VP[r][cc] = P[r][0] * V[0][cc] + P[r][1] * V[1][cc] + P[r][2] * V[2][cc] + P[r][3] * V[3][cc];
	// Some object's world matrix: rotated about y and placed in the world (like the game's draws).
	double a = 0.3 + 0.1 * seed;
	Wm[0][0] = cos(a);
	Wm[0][2] = sin(a);
	Wm[2][0] = -sin(a);
	Wm[2][2] = cos(a);
	Wm[1][1] = 1;
	Wm[0][3] = 95.0 + seed;
	Wm[1][3] = 2.0;
	Wm[2][3] = 207.0;
	Wm[3][3] = 1;
	for (int r = 0; r < 4; r++)
		for (int cc = 0; cc < 4; cc++)
			WVP[r][cc] = VP[r][0] * Wm[0][cc] + VP[r][1] * Wm[1][cc] + VP[r][2] * Wm[2][cc] + VP[r][3] * Wm[3][cc];
	float wvp[16], world[16];
	for (int i = 0; i < 16; i++)
	{
		wvp[i] = (float)WVP[i / 4][i % 4];
		world[i] = (float)Wm[i / 4][i % 4];
	}
	Camera cam;
	char why[160] = "";
	if (!CameraFromMatrices(wvp, world, cam, why, sizeof(why)))
		printf("camera rejected: %s\n", why);
	return cam;
}

// ---- D3D11 plumbing ----------------------------------------------------------------------------

struct Target
{
	ID3D11Texture2D* tex = nullptr;
	ID3D11RenderTargetView* rtv = nullptr;
	ID3D11ShaderResourceView* srv = nullptr;
};

static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;

static Target MakeTarget(UINT w, UINT h, DXGI_FORMAT fmt)
{
	Target t;
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = w;
	d.Height = h;
	d.MipLevels = d.ArraySize = 1;
	d.Format = fmt;
	d.SampleDesc.Count = 1;
	d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	g_dev->CreateTexture2D(&d, nullptr, &t.tex);
	g_dev->CreateRenderTargetView(t.tex, nullptr, &t.rtv);
	g_dev->CreateShaderResourceView(t.tex, nullptr, &t.srv);
	return t;
}

static float HalfToFloat(uint16_t h)
{
	uint32_t sign = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF, b;
	if (e == 0)
	{
		float v = ldexpf((float)m, -24);
		return (h & 0x8000) ? -v : v;
	}
	if (e == 31)
		b = sign | 0x7F800000 | (m << 13);
	else
		b = sign | ((e - 15 + 127) << 23) | (m << 13);
	float v;
	memcpy(&v, &b, 4);
	return v;
}

// Reads an R16G16B16A16_FLOAT target back as floats.
static std::vector<float> ReadBack(const Target& t)
{
	D3D11_TEXTURE2D_DESC d;
	t.tex->GetDesc(&d);
	d.BindFlags = 0;
	d.Usage = D3D11_USAGE_STAGING;
	d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ID3D11Texture2D* st = nullptr;
	g_dev->CreateTexture2D(&d, nullptr, &st);
	g_ctx->CopyResource(st, t.tex);
	D3D11_MAPPED_SUBRESOURCE m;
	g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m);
	std::vector<float> out((size_t)W * H * 4);
	for (UINT y = 0; y < H; y++)
	{
		const uint16_t* row = (const uint16_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch);
		for (UINT x = 0; x < W * 4; x++)
			out[(size_t)y * W * 4 + x] = HalfToFloat(row[x]);
	}
	g_ctx->Unmap(st, 0);
	st->Release();
	return out;
}

static ID3DBlob* CompileEntry(const char* entry, const char* profile)
{
	const D3D_SHADER_MACRO defs[] = { { "TAA_D3D11", "1" }, { nullptr, nullptr } };
	ID3DBlob *code = nullptr, *err = nullptr;
	HRESULT hr = g_compile(g_src.data(), g_src.size(), "taa.hlsl", defs, nullptr, entry, profile,
		D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
	if (FAILED(hr))
		printf("%s: compile failed: %s\n", entry, err ? (const char*)err->GetBufferPointer() : "?");
	return code;
}

struct Pass
{
	ID3DBlob* code = nullptr;
	ID3D11PixelShader* ps = nullptr;
	std::vector<uint8_t> cb;
	std::vector<std::pair<std::string, UINT>> vars, textures, samplers;
};

static bool MakePass(Pass& p, const char* entry)
{
	p.code = CompileEntry(entry, "ps_4_0");
	if (!p.code)
		return false;
	g_dev->CreatePixelShader(p.code->GetBufferPointer(), p.code->GetBufferSize(), nullptr, &p.ps);
	ID3D11ShaderReflection* r = nullptr;
	g_reflect(p.code->GetBufferPointer(), p.code->GetBufferSize(), kIID_Reflection, (void**)&r);
	D3D11_SHADER_DESC sd = {};
	r->GetDesc(&sd);
	for (UINT i = 0; i < sd.BoundResources; i++)
	{
		D3D11_SHADER_INPUT_BIND_DESC b = {};
		r->GetResourceBindingDesc(i, &b);
		if (b.Type == D3D_SIT_TEXTURE)
			p.textures.push_back({ b.Name, b.BindPoint });
		if (b.Type == D3D_SIT_SAMPLER)
			p.samplers.push_back({ b.Name, b.BindPoint });
	}
	ID3D11ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByName("$Globals");
	D3D11_SHADER_BUFFER_DESC bd = {};
	if (cb && SUCCEEDED(cb->GetDesc(&bd)))
	{
		p.cb.assign((bd.Size + 15) & ~15u, 0);
		for (UINT i = 0; i < bd.Variables; i++)
		{
			D3D11_SHADER_VARIABLE_DESC vd = {};
			cb->GetVariableByIndex(i)->GetDesc(&vd);
			p.vars.push_back({ vd.Name, vd.StartOffset });
		}
	}
	r->Release();
	return true;
}

static void SetConst(Pass& p, const char* name, const float v[4])
{
	for (auto& x : p.vars)
		if (x.first == name && x.second + 16 <= p.cb.size())
			memcpy(&p.cb[x.second], v, 16);
}

static ID3D11VertexShader* g_vs;
static ID3D11InputLayout* g_layout;
static ID3D11Buffer* g_vb;
static ID3D11SamplerState *g_point, *g_linear;

// names: the sampler2D names bound to each srv; linearNames get the bilinear sampler.
static void Draw(Pass& p, const Target& out, const std::vector<std::pair<const char*, ID3D11ShaderResourceView*>>& srvs, const char* linearName)
{
	ID3D11Buffer* cb = nullptr;
	D3D11_BUFFER_DESC cbd = { (UINT)p.cb.size(), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
	D3D11_SUBRESOURCE_DATA cd = { p.cb.data(), 0, 0 };
	g_dev->CreateBuffer(&cbd, &cd, &cb);
	ID3D11ShaderResourceView* none[8] = {};
	g_ctx->PSSetShaderResources(0, 8, none);
	g_ctx->OMSetRenderTargets(1, &out.rtv, nullptr);
	for (auto& t : p.textures)
		for (auto& s : srvs)
			if (t.first == s.first)
				g_ctx->PSSetShaderResources(t.second, 1, &s.second);
	for (auto& s : p.samplers)
	{
		ID3D11SamplerState* st = linearName && s.first == linearName ? g_linear : g_point;
		g_ctx->PSSetSamplers(s.second, 1, &st);
	}
	D3D11_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
	g_ctx->RSSetViewports(1, &vp);
	UINT stride = 16, offset = 0;
	g_ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &offset);
	g_ctx->IASetInputLayout(g_layout);
	g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	g_ctx->VSSetShader(g_vs, nullptr, 0);
	g_ctx->PSSetShader(p.ps, nullptr, 0);
	g_ctx->PSSetConstantBuffers(0, 1, &cb);
	g_ctx->Draw(3, 0);
	cb->Release();
	g_ctx->PSSetShaderResources(0, 8, none);
}

// ---- PNG (uncompressed deflate) ----------------------------------------------------------------

static uint32_t Crc(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu)
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
		raw.insert(raw.end(), &rgb[(size_t)y * w * 3], &rgb[(size_t)y * w * 3] + w * 3);
	}
	std::vector<uint8_t> z = { 0x78, 0x01 };
	uint32_t a = 1, b = 0;
	for (size_t i = 0; i < raw.size(); i += 65535)
	{
		size_t n = raw.size() - i < 65535 ? raw.size() - i : 65535;
		z.push_back(i + n == raw.size());
		z.push_back(n & 0xFF);
		z.push_back(n >> 8);
		z.push_back(~n & 0xFF);
		z.push_back((~n >> 8) & 0xFF);
		z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
	}
	for (uint8_t v : raw)
	{
		a = (a + v) % 65521;
		b = (b + a) % 65521;
	}
	uint32_t ad = (b << 16) | a;
	for (int k = 3; k >= 0; k--)
		z.push_back((ad >> (8 * k)) & 0xFF);
	FILE* f = fopen(path, "wb");
	if (!f)
		return;
	auto be = [&](uint32_t v) { uint8_t q[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; fwrite(q, 1, 4, f); };
	auto chunk = [&](const char* type, const std::vector<uint8_t>& d) {
		be((uint32_t)d.size());
		std::vector<uint8_t> td(type, type + 4);
		td.insert(td.end(), d.begin(), d.end());
		fwrite(td.data(), 1, td.size(), f);
		be(Crc(td.data(), td.size()) ^ 0xFFFFFFFFu);
	};
	const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	fwrite(sig, 1, 8, f);
	std::vector<uint8_t> ihdr = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 24), (uint8_t)(h >> 16),
		(uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0 };
	chunk("IHDR", ihdr);
	chunk("IDAT", z);
	chunk("IEND", {});
	fclose(f);
}

static uint8_t ToByte(float v)
{
	v = v / (1.0f + v); // the error metric's display mapping (keeps HDR from dominating)
	v = powf(v < 0 ? 0 : v, 1.0f / 2.2f) * 255.0f + 0.5f;
	return (uint8_t)(v > 255 ? 255 : v);
}

// Error in display terms: per channel x / (1 + x).
static double Rmse(const std::vector<float>& rgba, const std::vector<float>& refRgb, const std::vector<uint8_t>* mask = nullptr)
{
	double s = 0;
	size_t n = 0;
	for (size_t i = 0; i < (size_t)W * H; i++)
	{
		if (mask && !(*mask)[i])
			continue;
		for (int k = 0; k < 3; k++)
		{
			double a = rgba[i * 4 + k], b = refRgb[i * 3 + k];
			a = a / (1 + fabs(a));
			b = b / (1 + fabs(b));
			s += (a - b) * (a - b);
		}
		n++;
	}
	return n ? sqrt(s / (3.0 * n)) : 0;
}

// ---- The plugin's frame loop -------------------------------------------------------------------

struct Settings
{
	float feedback = getenv("TAA_FB") ? (float)atof(getenv("TAA_FB")) : 0.95f, motionFeedback = 0.8f, motionPixels = 16.0f, depthTol = 0.05f, sharpness = 0.0f, clip = getenv("TAA_CLIP") ? (float)atof(getenv("TAA_CLIP")) : 1.25f, hdrK = getenv("TAA_K") ? (float)atof(getenv("TAA_K")) : 0.25f;
	bool reproject = true;
	float distrust = getenv("TAA_DT") ? (float)atof(getenv("TAA_DT")) : 0.6f; // as src/taa.cpp
	int debug = 0;
};

struct Run
{
	Pass resolve, output;
	Target scene, depth, hist[2], out;
	int cur = 0;
	bool historyValid = false;
	Camera prev;
	Settings s;
	std::vector<float> rgba, zw;
	float light = 1.0f; // the scene's colour (not the sky) x this: a light changing (the light case)

	void Init(Pass& r, Pass& o)
	{
		resolve = r;
		output = o;
		scene = MakeTarget(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT); // the game's FP16 scene colour
		depth = MakeTarget(W, H, DXGI_FORMAT_R32_FLOAT);
		hist[0] = MakeTarget(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT);
		hist[1] = MakeTarget(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT);
		out = MakeTarget(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT);
	}

	// One frame: render with the jitter of frame f, resolve, output. Returns the output image.
	std::vector<float> Frame(const Cam& c, unsigned f, int seed)
	{
		auto halton = [](unsigned i, unsigned base) {
			float fr = 1, r = 0;
			for (; i; i /= base)
			{
				fr /= base;
				r += fr * (i % base);
			}
			return r;
		};
		unsigned phase = (f & 7) + 1;
		float ox = halton(phase, 2) - 0.5f, oy = halton(phase, 3) - 0.5f;
		RenderFrame(c, ox, oy, rgba, zw);
		for (size_t i = 0; light != 1.0f && i < zw.size(); i++)
			if (zw[i] < 1.0f)
				for (int k = 0; k < 3; k++)
					rgba[i * 4 + k] *= light;
		std::vector<uint16_t> half(rgba.size());
		for (size_t i = 0; i < rgba.size(); i++)
		{
			// float -> half (round to nearest; the values here are normal or zero)
			float v = rgba[i];
			uint32_t b;
			memcpy(&b, &v, 4);
			uint32_t sign = (b >> 16) & 0x8000, e = (b >> 23) & 0xFF;
			int32_t ex = (int32_t)e - 127 + 15;
			uint16_t h = (uint16_t)sign;
			if (e != 0 && ex > 0 && ex < 31)
				h = (uint16_t)(sign | (((uint32_t)ex << 10) + (((b & 0x7FFFFF) + 0x1000) >> 13))); // a mantissa carry bumps the exponent
			half[i] = h;
		}
		g_ctx->UpdateSubresource(scene.tex, 0, nullptr, half.data(), W * 8, 0);
		g_ctx->UpdateSubresource(depth.tex, 0, nullptr, zw.data(), W * 4, 0);

		Camera cam = CameraThroughPlugin(c, seed);
		float reproj[4][4] = {};
		bool valid = historyValid && prev.ok && cam.ok;
		if (valid && s.reproject)
			ReprojectionMatrix(cam, prev, reproj);
		else if (valid)
			ReprojectionMatrix(cam, cam, reproj); // as if nothing moved
		const float cSize[4] = { (float)W, (float)H, 1.0f / W, 1.0f / H };
		const float cJitter[4] = { ox, oy, valid ? 1.0f : 0.0f, (float)s.debug };
		const float cProj[4] = { (float)cam.A, (float)cam.B, (float)cam.P00, (float)cam.P11 };
		const float cProj2[4] = { (float)cam.P02, (float)cam.P12, 0, 0 };
		const float cParams[4] = { 1 - s.feedback, 1 - s.motionFeedback, s.motionPixels, s.depthTol };
		const float cParams2[4] = { s.sharpness, s.clip, s.hdrK, s.distrust };
		for (Pass* p : { &resolve, &output })
		{
			SetConst(*p, "c_Size", cSize);
			SetConst(*p, "c_Jitter", cJitter);
			SetConst(*p, "c_Reproj0", reproj[0]);
			SetConst(*p, "c_Reproj1", reproj[1]);
			SetConst(*p, "c_Reproj2", reproj[2]);
			SetConst(*p, "c_Reproj3", reproj[3]);
			SetConst(*p, "c_Proj", cProj);
			SetConst(*p, "c_Proj2", cProj2);
			SetConst(*p, "c_Params", cParams);
			SetConst(*p, "c_Params2", cParams2);
		}
		int prv = cur ^ 1;
		Draw(resolve, hist[cur], { { "s_Scene", scene.srv }, { "s_History", hist[prv].srv }, { "s_Depth", depth.srv }, { "s_HistPoint", hist[prv].srv } },
			"s_History");
		Draw(output, out, { { "s_History", hist[cur].srv } }, nullptr);
		cur = prv;
		historyValid = true;
		prev = cam;
		return ReadBack(out);
	}
};

static void SavePanel(const char* name, const std::vector<float>& single, const std::vector<float>& taa, const std::vector<float>& ref)
{
	std::vector<uint8_t> img((size_t)W * 3 * H * 3);
	for (UINT y = 0; y < H; y++)
		for (UINT x = 0; x < W; x++)
			for (int k = 0; k < 3; k++)
			{
				size_t i = (size_t)y * W + x;
				img[((size_t)y * W * 3 + x) * 3 + k] = ToByte(single[i * 4 + k]);
				img[((size_t)y * W * 3 + W + x) * 3 + k] = ToByte(taa[i * 4 + k]);
				img[((size_t)y * W * 3 + 2 * W + x) * 3 + k] = ToByte(ref[i * 3 + k]);
			}
	char path[64];
	snprintf(path, sizeof(path), "taa_warp_%s.png", name);
	WritePng(path, img, W * 3, H);
}

int main()
{
	FILE* f = fopen("../../src/taa/shaders/taa.hlsl", "rb");
	if (!f)
		return printf("run from tests/taa\n"), 1;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		g_src.append(buf, n);
	fclose(f);
	HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
	g_compile = dc ? (PFN_Compile)GetProcAddress(dc, "D3DCompile") : nullptr;
	g_reflect = dc ? (PFN_Reflect)GetProcAddress(dc, "D3DReflect") : nullptr;
	if (!g_compile || !g_reflect)
		return printf("d3dcompiler_47 not available\n"), 1;
	D3D_FEATURE_LEVEL fl;
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_dev, &fl, &g_ctx)))
		return printf("no WARP device\n"), 1;

	ID3DBlob* vsCode = CompileEntry("VS_Fullscreen", "vs_4_0");
	Pass resolve, output;
	if (!vsCode || !MakePass(resolve, "PS_Resolve") || !MakePass(output, "PS_Output"))
		return 1;
	g_dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &g_vs);
	D3D11_INPUT_ELEMENT_DESC el = { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
	g_dev->CreateInputLayout(&el, 1, vsCode->GetBufferPointer(), vsCode->GetBufferSize(), &g_layout);
	const float tri[3][4] = { { -1, -1, 0, 1 }, { -1, 3, 0, 1 }, { 3, -1, 0, 1 } };
	D3D11_BUFFER_DESC bd = { sizeof(tri), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
	D3D11_SUBRESOURCE_DATA sd = { tri, 0, 0 };
	g_dev->CreateBuffer(&bd, &sd, &g_vb);
	D3D11_SAMPLER_DESC smp = {};
	smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	smp.MaxLOD = D3D11_FLOAT32_MAX;
	g_dev->CreateSamplerState(&smp, &g_point);
	smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	g_dev->CreateSamplerState(&smp, &g_linear);

	// --- still ---
	{
		printf("== still camera, 32 frames\n");
		BuildScene(false);
		Cam c = MakeCam(0.3, 1.7, 0.0, 0.05, 0.12);
		std::vector<float> ref, single, a, b;
		RenderReference(c, ref);
		Run r;
		r.Init(resolve, output);
		std::vector<float> zw;
		RenderFrame(c, 0, 0, single, zw);
		const char* trace = getenv("TAA_TRACE"); // "x,y": print one pixel's history frame by frame
		int tx = -1, ty = -1;
		if (trace)
			sscanf(trace, "%d,%d", &tx, &ty);
		float cpuEma = 0;
		for (unsigned fr = 0; fr < 32; fr++)
		{
			a = b;
			b = r.Frame(c, fr, 0);
			if (tx >= 0)
			{
				size_t i = (size_t)ty * W + tx;
				float in = r.rgba[i * 4 + 1];
				cpuEma = fr == 0 ? in : cpuEma + (in - cpuEma) * (1.0f - r.s.feedback);
				std::vector<float> h = ReadBack(r.hist[r.cur ^ 1]);
				printf("  frame %2u: input %.4f  GPU history %.4f (out %.4f)  CPU %.4f\n", fr, in, h[i * 4 + 1], b[i * 4 + 1], cpuEma);
				if (fr == 5)
				{
					// What the resolve reads: its history fetch at p must be last frame's history at p,
					// its centre sample this frame's input at p. (Re-runs frame 6 in debug modes.)
					Run probe = r;
					probe.s.debug = 4;
					std::vector<float> hPrev = h;
					probe.Frame(c, 6, 0);
					std::vector<float> fetched = ReadBack(probe.hist[probe.cur ^ 1]);
					int bx = -1, by = -1;
					for (int dy = -2; dy <= 2; dy++)
						for (int dx = -2; dx <= 2; dx++)
							if (fabsf(hPrev[((size_t)(ty + dy) * W + tx + dx) * 4 + 1] - fetched[i * 4 + 1]) < 1e-3f && bx < 0)
								bx = dx, by = dy;
					printf("  history fetch at p: %.4f; last history at p %.4f; matches last history at offset (%d, %d)\n", fetched[i * 4 + 1],
						hPrev[i * 4 + 1], bx, by);
					probe = r;
					probe.s.debug = 5;
					probe.Frame(c, 6, 0);
					std::vector<float> cen = ReadBack(probe.hist[probe.cur ^ 1]);
					printf("  centre sample at p: %.4f; input at p %.4f\n", cen[i * 4 + 1], probe.rgba[i * 4 + 1]);
					probe = r;
					probe.s.debug = 3;
					float k = probe.s.hdrK;
					probe.s.hdrK = 0;
					probe.Frame(c, 6, 0);
					probe.s.hdrK = k;
					std::vector<float> mv = ReadBack(probe.hist[probe.cur ^ 1]);
					printf("  motion (%.5f, %.5f), history used: %.0f (1 camera, 2 with camera, 0 none); depth here %.4f, last history depth %.4f\n",
						mv[i * 4], mv[i * 4 + 1], mv[i * 4 + 2], mv[i * 4 + 3], hPrev[i * 4 + 3]);
				}
			}
		}
		double eSingle = Rmse(single, ref), eTaa = Rmse(b, ref);
		// The best 8 jitter phases can do: their plain average (what a converged TAA approaches).
		std::vector<float> ideal((size_t)W * H * 4, 0.0f), fr8;
		for (unsigned ph = 1; ph <= 8; ph++)
		{
			auto halton = [](unsigned i, unsigned base) { float fr = 1, r = 0; for (; i; i /= base) { fr /= base; r += fr * (i % base); } return r; };
			RenderFrame(c, halton(ph, 2) - 0.5f, halton(ph, 3) - 0.5f, fr8, zw);
			for (size_t i = 0; i < ideal.size(); i++)
				ideal[i] += fr8[i] / 8.0f;
		}
		std::vector<uint8_t> nearMask((size_t)W * H, 0), farMask((size_t)W * H, 0);
		for (size_t i = 0; i < (size_t)W * H; i++)
			(i / W > H * 3 / 5 ? nearMask : farMask)[i] = 1;
		double eIdeal = Rmse(ideal, ref);
		double nearTaa = 0, nearIdeal = 0, nearSingle = 0; // filled below
		{
			// A plain running average on the CPU (no clipping, linear), same frames, same weight.
			std::vector<float> ema;
			auto halton = [](unsigned i, unsigned base) { float fr = 1, r = 0; for (; i; i /= base) { fr /= base; r += fr * (i % base); } return r; };
			for (unsigned fr = 0; fr < 32; fr++)
			{
				unsigned ph = (fr & 7) + 1;
				RenderFrame(c, halton(ph, 2) - 0.5f, halton(ph, 3) - 0.5f, fr8, zw);
				if (ema.empty())
					ema = fr8;
				for (size_t i = 0; i < ema.size(); i++)
					ema[i] += (fr8[i] - ema[i]) * (1.0f - r.s.feedback);
			}
			std::vector<uint8_t> nm((size_t)W * H, 0);
			for (size_t i = 0; i < (size_t)W * H; i++)
				nm[i] = i / W > H * 3 / 5;
			printf("  CPU running average: all %.4f, near half %.4f\n", Rmse(ema, ref), Rmse(ema, ref, &nm));
		}
nearTaa = Rmse(b, ref, &nearMask);
		nearIdeal = Rmse(ideal, ref, &nearMask);
		nearSingle = Rmse(single, ref, &nearMask);
				printf("  ideal 8-phase average %.4f; near half: one sample %.4f, ideal %.4f, TAA %.4f; far: ideal %.4f, TAA %.4f\n", eIdeal,
			Rmse(single, ref, &nearMask), Rmse(ideal, ref, &nearMask), Rmse(b, ref, &nearMask), Rmse(ideal, ref, &farMask), Rmse(b, ref, &farMask));
		// Shimmer: change between consecutive frames, TAA vs the raw jittered frames.
		double shimmerTaa = 0, shimmerRaw = 0;
		std::vector<float> raw1, raw2;
		RenderFrame(c, 0.25f, -0.1f, raw1, zw);
		RenderFrame(c, -0.25f, 0.2f, raw2, zw);
		{
			std::vector<float> refA(W * H * 3);
			for (size_t i = 0; i < (size_t)W * H; i++)
				for (int k = 0; k < 3; k++)
					refA[i * 3 + k] = a[i * 4 + k];
			shimmerTaa = Rmse(b, refA);
			for (size_t i = 0; i < (size_t)W * H; i++)
				for (int k = 0; k < 3; k++)
					refA[i * 3 + k] = raw1[i * 4 + k];
			shimmerRaw = Rmse(raw2, refA);
		}
		printf("  error vs 64-sample reference: one sample %.4f, TAA %.4f (%.0f%%)\n", eSingle, eTaa, 100.0 * eTaa / eSingle);
		printf("  frame-to-frame change: raw jittered frames %.4f, TAA %.4f\n", shimmerRaw, shimmerTaa);
		CHECK(eTaa < 0.5 * eSingle, "still: TAA error %.4f not well below one sample per pixel %.4f", eTaa, eSingle);
		CHECK(nearTaa < 0.4 * nearSingle && nearTaa < 1.6 * nearIdeal, "still: near half %.4f (one sample %.4f, ideal %.4f)", nearTaa, nearSingle,
			nearIdeal);
		CHECK(shimmerTaa < 0.35 * shimmerRaw, "still: TAA still shimmers (%.4f vs raw %.4f)", shimmerTaa, shimmerRaw);
		SavePanel("still", single, b, ref);
		{
			r.s.debug = 1;
			std::vector<float> dbg = r.Frame(c, 40, 0);
			r.s.debug = 0;
			SavePanel("still_debug", single, dbg, ref);
			// Raw motion of the resolve (k = 0 so the output pass passes it through): must be ~0 here.
			r.s.debug = 3;
			float k = r.s.hdrK;
			r.s.hdrK = 0;
			r.Frame(c, 41, 0);
			r.s.debug = 0;
			std::vector<float> raw = ReadBack(r.hist[r.cur ^ 1]);
			r.s.hdrK = k;
			double maxM = 0, sumM = 0;
			int rejected = 0;
			for (size_t i = 0; i < (size_t)W * H; i++)
			{
				double m = fabs(raw[i * 4]) + fabs(raw[i * 4 + 1]);
				maxM = m > maxM ? m : maxM;
				sumM += m;
				rejected += raw[i * 4 + 2] != 1.0f;
			}
			printf("  still camera motion: max %.6f px, mean %.6f px; pixels not using camera reprojection: %d\n", maxM, sumM / (W * H), rejected);
			CHECK(maxM < 0.01, "still: motion should be zero (max %.6f px)", maxM);
		}

		// Sharpening: the same frame with and without it. It must change something and never leave the
		// range of each pixel and its 4 neighbours (no halos), in the space the shader works in.
		{
			Run sharpRun = r, plainRun = r;
			sharpRun.s.sharpness = 1.0f;
			std::vector<float> plain = plainRun.Frame(c, 32, 0), sharp = sharpRun.Frame(c, 32, 0);
			const float k = r.s.hdrK;
			auto comp = [&](const std::vector<float>& img, size_t i, int ch) {
				float l = 0.299f * img[i * 4] + 0.587f * img[i * 4 + 1] + 0.114f * img[i * 4 + 2];
				return img[i * 4 + ch] / (1.0f + k * (l > 0 ? l : 0));
			};
			double change = 0, worst = 0;
			for (UINT y = 1; y + 1 < H; y++)
				for (UINT x = 1; x + 1 < W; x++)
					for (int ch = 0; ch < 3; ch++)
					{
						size_t i = (size_t)y * W + x;
						float v = comp(sharp, i, ch), lo = 1e9f, hi = -1e9f;
						for (size_t q : { i, i - 1, i + 1, i - W, i + W })
						{
							float u = comp(plain, q, ch);
							lo = u < lo ? u : lo;
							hi = u > hi ? u : hi;
						}
						double out = v < lo ? lo - v : v > hi ? v - hi : 0;
						worst = out > worst ? out : worst;
						change += fabs(v - comp(plain, i, ch));
					}
			change /= (double)(W - 2) * (H - 2) * 3;
			printf("  sharpness 1: mean change %.4f, furthest outside the neighbours' range %.5f\n", change, worst);
			CHECK(change > 0.002, "sharpening does nothing (%.5f)", change);
			CHECK(worst < 0.002, "sharpening overshoots the neighbours' range by %.4f", worst);
		}
	}

	// --- pan ---
	{
		printf("== camera turning and moving sideways, 40 frames\n");
		BuildScene(false);
		std::vector<float> outRe, outNo, ref, single, zw;
		for (int variant = 0; variant < 2; variant++)
		{
			Run r;
			r.Init(resolve, output);
			r.s.reproject = variant == 0;
			for (unsigned fr = 0; fr < 40; fr++)
			{
				Cam c = MakeCam(0.3 + 0.03 * fr, 1.7, 0.02 * fr, 0.05 + 0.006 * fr, 0.12);
				std::vector<float> o = r.Frame(c, fr, (int)fr);
				if (fr == 39)
					(variant == 0 ? outRe : outNo) = o;
			}
		}
		Cam last = MakeCam(0.3 + 0.03 * 39, 1.7, 0.02 * 39, 0.05 + 0.006 * 39, 0.12);
		RenderReference(last, ref);
		RenderFrame(last, 0, 0, single, zw);
		double eSingle = Rmse(single, ref), eRe = Rmse(outRe, ref), eNo = Rmse(outNo, ref);
		printf("  error: one sample %.4f, TAA %.4f (%.0f%%), TAA without reprojection %.4f\n", eSingle, eRe, 100.0 * eRe / eSingle, eNo);
		CHECK(eRe < 0.8 * eSingle, "pan: TAA error %.4f not below one sample per pixel %.4f", eRe, eSingle);
		CHECK(eRe < 0.8 * eNo, "pan: reprojection doesn't help (%.4f vs %.4f without)", eRe, eNo);
		SavePanel("pan", single, outRe, ref);
	}

	// --- object ---
	{
		printf("== box sliding across a still camera, 40 frames\n");
		Cam c = MakeCam(0.3, 1.7, 0.0, 0.05, 0.12);
		Run r;
		r.Init(resolve, output);
		std::vector<float> o, ref, single, zw;
		for (unsigned fr = 0; fr < 40; fr++)
		{
			g_movingX = -2.0 + 0.06 * fr;
			BuildScene(true);
			o = r.Frame(c, fr, 0);
		}
		RenderReference(c, ref);
		RenderFrame(c, 0, 0, single, zw);
		// Where the box was 2..20 frames ago but isn't now: the trail.
		std::vector<uint8_t> trail((size_t)W * H, 0), now((size_t)W * H, 0);
		auto boxMask = [&](double mx, std::vector<uint8_t>& m) {
			g_movingX = mx;
			BuildScene(true);
			std::vector<float> withBox;
			RenderFrame(c, 0, 0, withBox, zw);
			BuildScene(false);
			std::vector<float> without;
			RenderFrame(c, 0, 0, without, zw);
			for (size_t i = 0; i < (size_t)W * H; i++)
				if (fabsf(withBox[i * 4] - without[i * 4]) + fabsf(withBox[i * 4 + 2] - without[i * 4 + 2]) > 0.05f)
					m[i] = 1;
		};
		for (int back = 2; back <= 20; back++)
			boxMask(-2.0 + 0.06 * (39 - back), trail);
		boxMask(-2.0 + 0.06 * 39, now);
		// Keep a 2-pixel margin from the box's current outline (edges are allowed to be soft).
		std::vector<uint8_t> nearNow = now;
		for (int pass = 0; pass < 2; pass++)
		{
			std::vector<uint8_t> grow = nearNow;
			for (UINT y = 1; y + 1 < H; y++)
				for (UINT x = 1; x + 1 < W; x++)
					if (nearNow[y * W + x])
						grow[y * W + x - 1] = grow[y * W + x + 1] = grow[(y - 1) * W + x] = grow[(y + 1) * W + x] = 1;
			nearNow = grow;
		}
		size_t count = 0;
		for (size_t i = 0; i < (size_t)W * H; i++)
		{
			trail[i] = trail[i] && !nearNow[i];
			count += trail[i];
		}
		g_movingX = -2.0 + 0.06 * 39;
		BuildScene(true);
		RenderReference(c, ref);
		double eTrail = Rmse(o, ref, &trail), eTrailSingle = Rmse(single, ref, &trail), eAll = Rmse(o, ref);
		// Ghosting = the box's yellow left behind: red minus blue above the reference's.
		double ghost = 0;
		for (size_t i = 0; i < (size_t)W * H; i++)
			if (trail[i])
			{
				auto disp = [](float v) { return v / (1.0f + fabsf(v)); };
				double e = (disp(o[i * 4]) - disp(o[i * 4 + 2])) - (disp(ref[i * 3]) - disp(ref[i * 3 + 2]));
				ghost += e > 0 ? e : 0;
			}
		ghost /= count ? count : 1;
		printf("  trail (%zu px): error TAA %.4f, one sample %.4f; whole image TAA %.4f\n", count, eTrail, eTrailSingle, eAll);
		CHECK(count > 1000, "object: trail mask too small (%zu)", count);
		printf("  box colour left in the trail: %.4f\n", ghost);
		CHECK(ghost < 0.01, "object: ghost trail behind the box (%.4f)", ghost);
		CHECK(eAll < 0.6 * Rmse(single, ref), "object: whole image error %.4f", eAll);
		SavePanel("object", single, o, ref);
	}

	// --- light ---
	{
		printf("== light changing, still camera\n");
		BuildScene(false);
		Cam c = MakeCam(0.3, 1.7, 0.0, 0.05, 0.12);
		// Mean over the pixels that aren't sky of an RGBA image (green).
		std::vector<float> zw0, col0;
		RenderFrame(c, 0, 0, col0, zw0);
		auto mean = [&](const std::vector<float>& img) {
			double sum = 0;
			int n = 0;
			for (size_t i = 0; i < zw0.size(); i++)
				if (zw0[i] < 1.0f)
				{
					sum += img[i * 4 + 1];
					n++;
				}
			return sum / n;
		};
		// Frames until the output (and the input) has 90 % of the change: at once (blend 1) or settling
		// by `blend` of the rest per frame.
		auto lag = [&](float blend, int& inFrames, int& outFrames) {
			Run r;
			r.Init(resolve, output);
			unsigned fr = 0;
			std::vector<float> o;
			for (; fr < 16; fr++)
				o = r.Frame(c, fr, 0);
			double outBefore = mean(o), inBefore = mean(r.rgba);
			std::vector<double> outs, ins;
			for (int k = 0; k < 64; k++, fr++)
			{
				r.light = 1.0f + 0.5f * (1.0f - powf(1.0f - blend, (float)(k + 1)));
				o = r.Frame(c, fr, 0);
				outs.push_back(mean(o));
				ins.push_back(mean(r.rgba));
			}
			// The settled values: the light at x 1.5 (both curves end there; the last frames are close).
			r.light = 1.5f;
			for (int k = 0; k < 16; k++, fr++)
				o = r.Frame(c, fr, 0);
			double outAfter = mean(o), inAfter = 1.5 * inBefore;
			inFrames = outFrames = -1;
			for (int k = 0; k < 64; k++)
			{
				if (inFrames < 0 && ins[k] - inBefore >= 0.9 * (inAfter - inBefore))
					inFrames = k + 1;
				if (outFrames < 0 && outs[k] - outBefore >= 0.9 * (outAfter - outBefore))
					outFrames = k + 1;
			}
		};
		int stepIn, stepOut, in10, out10, in20, out20;
		lag(1.0f, stepIn, stepOut);
		lag(0.1f, in10, out10);
		lag(0.2f, in20, out20);
		printf("  light x 1.5 at once: the input has 90%% of the change after %d frame(s), TAA's output after %d (its clip box hardly\n"
			   "  narrows on a uniformly brighter textured image, so mostly its feedback decides)\n",
			stepIn, stepOut);
		printf("  settling like SSGI's bounce light at 10%% per frame (SSGI without TAA): input after %d frames, TAA's output after %d\n", in10,
			out10);
		printf("  settling at 20%% per frame (SSGI while TAA averages): input after %d frames, TAA's output after %d\n", in20, out20);
		CHECK(stepOut > 0 && stepOut <= 30, "light: TAA takes %d frames to follow a light switched on", stepOut);
		// Together, SSGI's doubled blend and TAA follow a change about as fast as SSGI did alone.
		CHECK(out20 > 0 && out20 <= in10 + 4 && out20 < out10 - 6, "light: SSGI at 20%% + TAA take %d frames (SSGI alone at 10%%: %d; at 10%% + TAA: %d)",
			out20, in10, out10);
	}

	// --- follow ---
	{
		printf("== camera running forward with a figure that moves with it (the player), 40 frames\n");
		Run r;
		r.Init(resolve, output);
		std::vector<float> o, ref, single, zw;
		Cam c;
		g_follow = true;
		for (unsigned fr = 0; fr < 40; fr++)
		{
			c = MakeCam(0.3, 1.7, 0.08 * fr, 0.05, 0.12);
			memcpy(g_followEye, c.eye, sizeof(g_followEye));
			BuildScene(false);
			if (fr == 39)
			{
				// Which history the resolve used on the figure (debug 3, run on a copy).
				Run probe = r;
				probe.s.debug = 3;
				probe.s.hdrK = 0;
				probe.Frame(c, fr, (int)fr);
				std::vector<float> raw = ReadBack(probe.hist[probe.cur ^ 1]);
				std::vector<float> withFig, without;
				RenderFrame(c, 0, 0, withFig, zw);
				g_follow = false;
				BuildScene(false);
				RenderFrame(c, 0, 0, without, zw);
				g_follow = true;
				BuildScene(false);
				std::vector<uint8_t> fig((size_t)W * H, 0);
				size_t n = 0, byFollow = 0;
				for (size_t i = 0; i < (size_t)W * H; i++)
					if (fabsf(withFig[i * 4] - without[i * 4]) + fabsf(withFig[i * 4 + 2] - without[i * 4 + 2]) > 0.05f)
					{
						fig[i] = 1;
						n++;
						byFollow += raw[i * 4 + 2] == 2.0f;
					}
				o = r.Frame(c, fr, (int)fr);
				RenderReference(c, ref);
				RenderFrame(c, 0, 0, single, zw);
				// Edges of the figure: the mask grown by a pixel minus its interior.
				std::vector<uint8_t> edge((size_t)W * H, 0);
				size_t ne = 0;
				for (UINT y = 1; y + 1 < H; y++)
					for (UINT x = 1; x + 1 < W; x++)
					{
						size_t i = (size_t)y * W + x;
						int sum = fig[i] + fig[i - 1] + fig[i + 1] + fig[i - W] + fig[i + W];
						if (sum > 0 && sum < 5)
							edge[i] = 1, ne++;
					}
				double eFig = Rmse(o, ref, &edge), eFigSingle = Rmse(single, ref, &edge);
				if (getenv("TAA_DIAG"))
				{
					std::vector<std::pair<float, size_t>> worst;
					auto disp = [](float v) { return v / (1.0f + fabsf(v)); };
					for (size_t i = 0; i < (size_t)W * H; i++)
						if (edge[i])
						{
							float e = 0;
							for (int ch = 0; ch < 3; ch++)
								e += fabsf(disp(o[i * 4 + ch]) - disp(ref[i * 3 + ch]));
							worst.push_back({ e, i });
						}
					std::sort(worst.begin(), worst.end(), [](auto& a, auto& b) { return a.first > b.first; });
					for (size_t k = 0; k < worst.size() && k < 12; k++)
					{
						size_t i = worst[k].second;
						printf("    (%zu,%zu) err %.3f which %.0f motion (%.2f,%.2f) fig %d  taa (%.3f %.3f %.3f) ref (%.3f %.3f %.3f) single (%.3f %.3f %.3f)\n",
							i % W, i / W, worst[k].first, raw[i * 4 + 2], raw[i * 4], raw[i * 4 + 1], fig[i], o[i * 4], o[i * 4 + 1], o[i * 4 + 2], ref[i * 3],
							ref[i * 3 + 1], ref[i * 3 + 2], single[i * 4], single[i * 4 + 1], single[i * 4 + 2]);
					}
				}
				printf("  figure: %zu px, %.0f%% resolved with the history that moved with the camera; edges (%zu px): TAA %.4f, one sample %.4f; whole image TAA %.4f, one sample %.4f\n",
					n, 100.0 * byFollow / (n ? n : 1), ne, eFig, eFigSingle, Rmse(o, ref), Rmse(single, ref));
				// The figure's edges lie on pixel boundaries (one sample is already exact there), so the check
				// is that nothing smears around it; its interior is uniform, so either history is right there.
				CHECK(eFig <= eFigSingle, "follow: the figure's edges are smeared (%.4f vs one sample %.4f)", eFig, eFigSingle);
				CHECK(Rmse(o, ref) < 0.6 * Rmse(single, ref), "follow: whole image %.4f vs one sample %.4f", Rmse(o, ref), Rmse(single, ref));
				SavePanel("follow", single, o, ref);
			}
			else
				r.Frame(c, fr, (int)fr);
		}
		g_follow = false;
	}

	// --- hdr ---
	{
		printf("== HDR values\n");
		BuildScene(false);
		Cam c = MakeCam(0.3, 1.7, 0.0, 0.05, 0.12);
		Run r;
		r.Init(resolve, output);
		std::vector<float> o;
		for (unsigned fr = 0; fr < 16; fr++)
			o = r.Frame(c, fr, 0);
		float maxV = 0;
		bool bad = false;
		for (float v : o)
		{
			bad = bad || !isfinite(v);
			maxV = v > maxV ? v : maxV;
		}
		printf("  brightest output %.2f (the panel is 12)\n", maxV);
		CHECK(!bad, "hdr: NaN or infinity in the output");
		CHECK(maxV > 10.0f && maxV < 13.0f, "hdr: the bright panel came out at %.2f", maxV);
	}

	printf(g_failures ? "FAILED (%d)\n" : "all passed\n", g_failures);
	return g_failures ? 1 : 0;
}
