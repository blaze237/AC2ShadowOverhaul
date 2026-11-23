// Runs the motion blur shaders (src/mb/shaders/motionblur.hlsl) with D3D11's software renderer
// (WARP) on a synthetic frame, in the plugin's pass order and with its constants (camera maths
// through core/camera.h, fed game-style matrices).
//
// Frame: a striped wall 20 m away, a red block 5 m away (pixels 130-189 x 60-119) and a post 3 m
// away in front of the block's middle (pixels 150-155 x 50-129). Cases:
//   still - the camera doesn't move: untouched, bit for bit
//   off   - the camera moves, strength 0: untouched
//   pan   - the camera moves sideways (the wall 8 px, the block 32 px): half-velocities of 4 px on the
//           wall and the tile limit on the block; the wall's stripes blur; the nearer block streaks
//           over the wall
//   short - the same with a 4 px limit: the block's streak stops at it
// Writes mb_warp_<case>.png (top: input, bottom: output).
//
// The HLSL is compiled for shader model 4 in backwards-compatibility mode with MB_D3D11 defined.
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

static const UINT W = 320, H = 180, K = 16;
static const float NEAR_ = 0.1f, FAR_ = 2000.0f;
static const double kA = FAR_ / (FAR_ - NEAR_), kB = -NEAR_ * FAR_ / (FAR_ - NEAR_);
static const double kP00 = 1.0, kP11 = 1.0 * W / H;
static const int OX0 = 130, OX1 = 190, OY0 = 60, OY1 = 120; // the block
static const int PX0 = 150, PX1 = 156, PY0 = 50, PY1 = 130; // the post
static const float kWallZ = 20.0f, kBlockZ = 5.0f, kPostZ = 3.0f;

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

static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;

// ---- Targets -----------------------------------------------------------------------------------

struct Target
{
	ID3D11Texture2D* tex = nullptr;
	ID3D11RenderTargetView* rtv = nullptr;
	ID3D11ShaderResourceView* srv = nullptr;
	UINT w = 0, h = 0;
	DXGI_FORMAT fmt;
};

static Target MakeTarget(UINT w, UINT h, DXGI_FORMAT fmt, const void* data = nullptr, UINT pitch = 0)
{
	Target t;
	t.w = w;
	t.h = h;
	t.fmt = fmt;
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = w;
	d.Height = h;
	d.MipLevels = d.ArraySize = 1;
	d.Format = fmt;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA sd = { data, pitch, 0 };
	g_dev->CreateTexture2D(&d, data ? &sd : nullptr, &t.tex);
	g_dev->CreateRenderTargetView(t.tex, nullptr, &t.rtv);
	g_dev->CreateShaderResourceView(t.tex, nullptr, &t.srv);
	return t;
}

static void Free(Target& t)
{
	if (t.srv) t.srv->Release();
	if (t.rtv) t.rtv->Release();
	if (t.tex) t.tex->Release();
	t = Target();
}

static float HalfToFloat(uint16_t h)
{
	uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023, f;
	if (e == 0)
		f = m ? 0 : s << 31;
	else if (e == 31)
		f = (s << 31) | 0x7F800000 | (m << 13);
	else
		f = (s << 31) | ((e + 112) << 23) | (m << 13);
	float r;
	memcpy(&r, &f, 4);
	if (e == 0 && m)
		r = (s ? -1.0f : 1.0f) * m / 16777216.0f;
	return r;
}

static uint16_t FloatToHalf(float v)
{
	uint32_t f;
	memcpy(&f, &v, 4);
	uint32_t s = (f >> 16) & 0x8000;
	int e = (int)((f >> 23) & 255) - 112;
	uint32_t m = f & 0x7FFFFF;
	if (e <= 0)
		return (uint16_t)s;
	if (e >= 31)
		return (uint16_t)(s | 0x7C00);
	return (uint16_t)(s | (e << 10) | ((m + 0x1000) >> 13));
}

// RGBA float16 read back as floats (w * h * 4).
static std::vector<float> Read(const Target& t)
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
	std::vector<float> out((size_t)t.w * t.h * 4);
	for (UINT y = 0; y < t.h; y++)
	{
		const uint16_t* row = (const uint16_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch);
		for (UINT x = 0; x < t.w * 4; x++)
			out[(size_t)y * t.w * 4 + x] = HalfToFloat(row[x]);
	}
	g_ctx->Unmap(st, 0);
	st->Release();
	return out;
}

// ---- Shaders -----------------------------------------------------------------------------------

static ID3DBlob* CompileEntry(const char* src, size_t n, const char* entry, const char* profile)
{
	const D3D_SHADER_MACRO defs[] = { { "MB_D3D11", "1" }, { nullptr, nullptr } };
	ID3DBlob *code = nullptr, *err = nullptr;
	HRESULT hr = g_compile(src, n, "motionblur.hlsl", defs, nullptr, entry, profile,
		D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
	if (FAILED(hr))
		printf("%s: compile failed: %s\n", entry, err ? (const char*)err->GetBufferPointer() : "?");
	return code;
}

struct Pass
{
	ID3D11PixelShader* ps = nullptr;
	std::vector<uint8_t> cb;
	std::vector<std::pair<std::string, UINT>> vars, textures, samplers;
};

static bool MakePass(Pass& p, const char* entry)
{
	ID3DBlob* code = CompileEntry(g_src.data(), g_src.size(), entry, "ps_4_0");
	if (!code)
		return false;
	g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &p.ps);
	ID3D11ShaderReflection* r = nullptr;
	g_reflect(code->GetBufferPointer(), code->GetBufferSize(), kIID_Reflection, (void**)&r);
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
	code->Release();
	return true;
}

static void SetConst(Pass& p, const char* name, const float v[4])
{
	for (auto& x : p.vars)
		if (x.first == name && x.second + 16 <= p.cb.size())
			memcpy(&p.cb[x.second], v, 16);
}

static ID3D11VertexShader* g_vsFull;
static ID3D11InputLayout* g_layoutFull;
static ID3D11Buffer* g_vbFull;
static ID3D11SamplerState* g_point;

static ID3D11Buffer* Upload(const std::vector<uint8_t>& cbData)
{
	ID3D11Buffer* cb = nullptr;
	D3D11_BUFFER_DESC cbd = { (UINT)cbData.size(), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
	D3D11_SUBRESOURCE_DATA cd = { cbData.data(), 0, 0 };
	g_dev->CreateBuffer(&cbd, &cd, &cb);
	return cb;
}

static void Bind(Pass& p, const Target& out, const std::vector<std::pair<const char*, ID3D11ShaderResourceView*>>& srvs, ID3D11Buffer* cb)
{
	ID3D11ShaderResourceView* none[8] = {};
	g_ctx->PSSetShaderResources(0, 8, none);
	g_ctx->OMSetRenderTargets(1, &out.rtv, nullptr);
	for (auto& t : p.textures)
		for (auto& s : srvs)
			if (t.first == s.first)
				g_ctx->PSSetShaderResources(t.second, 1, &s.second);
	for (auto& s : p.samplers)
		g_ctx->PSSetSamplers(s.second, 1, &g_point);
	D3D11_VIEWPORT vp = { 0, 0, (float)out.w, (float)out.h, 0, 1 };
	g_ctx->RSSetViewports(1, &vp);
	g_ctx->PSSetShader(p.ps, nullptr, 0);
	g_ctx->PSSetConstantBuffers(0, 1, &cb);
}

static void Draw(Pass& p, const Target& out, const std::vector<std::pair<const char*, ID3D11ShaderResourceView*>>& srvs)
{
	ID3D11Buffer* cb = Upload(p.cb);
	Bind(p, out, srvs, cb);
	UINT stride = 16, offset = 0;
	g_ctx->IASetVertexBuffers(0, 1, &g_vbFull, &stride, &offset);
	g_ctx->IASetInputLayout(g_layoutFull);
	g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	g_ctx->VSSetShader(g_vsFull, nullptr, 0);
	g_ctx->Draw(3, 0);
	cb->Release();
	ID3D11ShaderResourceView* none[8] = {};
	g_ctx->PSSetShaderResources(0, 8, none);
}

// ---- The frame ---------------------------------------------------------------------------------

static Camera MakeCamera(double eyeX)
{
	// Looking along +z from (eyeX, 0, 0); g_World = identity, so g_WorldViewProj = P * V.
	Mat4 P = { { { kP00, 0, 0, 0 }, { 0, kP11, 0, 0 }, { 0, 0, kA, kB }, { 0, 0, 1, 0 } } };
	Mat4 V = { { { 1, 0, 0, -eyeX }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } } };
	Mat4 vp = Mul(P, V);
	float wvp[16], world[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++)
			wvp[4 * r + c] = (float)vp.m[r][c];
	Camera cam;
	char why[128];
	if (!CameraFromMatrices(wvp, world, cam, why, sizeof(why)))
		printf("camera: %s\n", why);
	return cam;
}

static float Stripe(int x) { return (x / 4) % 2 ? 0.8f : 0.2f; }

struct Case
{
	const char* name;
	double camMovePx; // the wall's screen motion from the camera (px, x right); things at depth z move camMovePx * 20 / z
	float strength;
	UINT K;           // tile size = longest half-streak (px)
};

struct Result
{
	std::vector<float> in, out, prep;
};

static Pass g_prep, g_tileMax, g_nmax, g_recon;

static Result RunCase(const Case& cs)
{
	// Camera: now at x = 0; then at -dx, so the wall (20 m) moved camMovePx px on screen: a point at
	// depth z moves -P00 * dx / z * W / 2 px when the camera moves dx (x right).
	const double dx = -cs.camMovePx * kWallZ / (kP00 * W / 2.0);
	Camera now = MakeCamera(0.0), then = MakeCamera(-dx);

	// The frame: colour and z/w.
	std::vector<uint16_t> colour((size_t)W * H * 4);
	std::vector<float> zw((size_t)W * H);
	Result res;
	res.in.resize((size_t)W * H * 4);
	for (UINT y = 0; y < H; y++)
		for (UINT x = 0; x < W; x++)
		{
			float r = Stripe(x), g = Stripe(x), b = Stripe(x), z = kWallZ;
			if ((int)x >= OX0 && (int)x < OX1 && (int)y >= OY0 && (int)y < OY1)
				r = 1.0f, g = b = 0.1f, z = kBlockZ;
			if ((int)x >= PX0 && (int)x < PX1 && (int)y >= PY0 && (int)y < PY1)
				r = g = 0.1f, b = 1.0f, z = kPostZ;
			size_t i = (size_t)y * W + x;
			float c[4] = { r, g, b, 1 };
			for (int k = 0; k < 4; k++)
			{
				colour[i * 4 + k] = FloatToHalf(c[k]);
				res.in[i * 4 + k] = HalfToFloat(colour[i * 4 + k]);
			}
			zw[i] = (float)(kA + kB / z);
		}
	const UINT K = cs.K;
	Target tColour = MakeTarget(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, colour.data(), W * 8);
	Target tZw = MakeTarget(W, H, DXGI_FORMAT_R32_FLOAT, zw.data(), W * 4);
	Target tPrep = MakeTarget(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT);
	const UINT tx = (W + K - 1) / K, ty = (H + K - 1) / K;
	Target tTile = MakeTarget(tx, ty, DXGI_FORMAT_R16G16B16A16_FLOAT), tNmax = MakeTarget(tx, ty, DXGI_FORMAT_R16G16B16A16_FLOAT);
	Target tOut = MakeTarget(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT);

	float reproj[4][4];
	bool camValid = ReprojectionMatrix(now, then, reproj);
	CHECK(camValid, "%s: reprojection matrix", cs.name);
	const float c[10][4] = {
		{ (float)W, (float)H, 1.0f / W, 1.0f / H },
		{ now.f.A, now.f.B, now.f.P00, now.f.P11 },
		{ now.f.P02, now.f.P12, 0, 0 },
		{ reproj[0][0], reproj[0][1], reproj[0][2], reproj[0][3] },
		{ reproj[1][0], reproj[1][1], reproj[1][2], reproj[1][3] },
		{ reproj[2][0], reproj[2][1], reproj[2][2], reproj[2][3] },
		{ reproj[3][0], reproj[3][1], reproj[3][2], reproj[3][3] },
		{ cs.strength, 1.0f, 0.0f, (float)K }, // shutter 1: the streak is the whole frame's motion
		{ 16.0f, 0.05f, 0.0f, 0.0f },
		{ (float)tx, (float)ty, 1.0f / tx, 1.0f / ty },
	};
	const char* names[10] = { "c_Size", "c_Proj", "c_Proj2", "c_Reproj0", "c_Reproj1", "c_Reproj2", "c_Reproj3", "c_Blur", "c_Params", "c_Tiles" };
	for (Pass* p : { &g_prep, &g_tileMax, &g_nmax, &g_recon })
		for (int k = 0; k < 10; k++)
			SetConst(*p, names[k], c[k]);

	Draw(g_prep, tPrep, { { "s_0", tZw.srv } });
	Draw(g_tileMax, tTile, { { "s_0", tPrep.srv } });
	Draw(g_nmax, tNmax, { { "s_0", tTile.srv } });
	Draw(g_recon, tOut, { { "s_0", tColour.srv }, { "s_1", tPrep.srv }, { "s_2", tNmax.srv } });

	res.out = Read(tOut);
	res.prep = Read(tPrep);
	for (Target* t : { &tColour, &tZw, &tPrep, &tTile, &tNmax, &tOut })
		Free(*t);
	return res;
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

static void SavePng(const char* name, const Result& r)
{
	std::vector<uint8_t> rgb((size_t)W * H * 2 * 3);
	for (UINT y = 0; y < 2 * H; y++)
		for (UINT x = 0; x < W; x++)
			for (int k = 0; k < 3; k++)
			{
				const std::vector<float>& src = y < H ? r.in : r.out;
				float v = src[((size_t)(y % H) * W + x) * 4 + k];
				v = v < 0 ? 0 : v > 1 ? 1 : v;
				rgb[((size_t)y * W + x) * 3 + k] = (uint8_t)(powf(v, 1.0f / 2.2f) * 255.0f + 0.5f);
			}
	char path[64];
	snprintf(path, sizeof(path), "mb_warp_%s.png", name);
	WritePng(path, rgb, W, 2 * H);
}

// ---- Checks ------------------------------------------------------------------------------------

static float At(const std::vector<float>& img, int x, int y, int k) { return img[((size_t)y * W + x) * 4 + k]; }

static bool Untouched(const Result& r, int& firstX, int& firstY)
{
	for (UINT y = 0; y < H; y++)
		for (UINT x = 0; x < W; x++)
			for (int k = 0; k < 3; k++)
				if (At(r.out, x, y, k) != At(r.in, x, y, k))
				{
					firstX = (int)x;
					firstY = (int)y;
					return false;
				}
	return true;
}

static bool AnyNan(const Result& r)
{
	for (float v : r.out)
		if (!(v == v) || fabsf(v) > 1e4f)
			return true;
	return false;
}

int main()
{
	HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
	g_compile = (PFN_Compile)GetProcAddress(dc, "D3DCompile");
	g_reflect = (PFN_Reflect)GetProcAddress(dc, "D3DReflect");
	if (!g_compile || !g_reflect)
		return printf("d3dcompiler_47.dll not found\n"), 2;
	FILE* f = fopen("../../src/mb/shaders/motionblur.hlsl", "rb");
	if (!f)
		return printf("motionblur.hlsl not found (run from tests/mb)\n"), 2;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		g_src.append(buf, n);
	fclose(f);
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx)))
		return printf("no WARP device\n"), 2;

	// The fullscreen triangle.
	{
		ID3DBlob* vs = CompileEntry(g_src.data(), g_src.size(), "VS_Fullscreen", "vs_4_0");
		g_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vsFull);
		D3D11_INPUT_ELEMENT_DESC el = { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
		g_dev->CreateInputLayout(&el, 1, vs->GetBufferPointer(), vs->GetBufferSize(), &g_layoutFull);
		vs->Release();
		static const float tri[3][4] = { { -1, -1, 0, 1 }, { -1, 3, 0, 1 }, { 3, -1, 0, 1 } };
		D3D11_BUFFER_DESC bd = { sizeof(tri), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
		D3D11_SUBRESOURCE_DATA sd = { tri, 0, 0 };
		g_dev->CreateBuffer(&bd, &sd, &g_vbFull);
		D3D11_SAMPLER_DESC s = {};
		s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		s.MaxLOD = D3D11_FLOAT32_MAX;
		g_dev->CreateSamplerState(&s, &g_point);
	}
	if (!MakePass(g_prep, "PS_Prep") || !MakePass(g_tileMax, "PS_TileMax") || !MakePass(g_nmax, "PS_NeighborMax") ||
		!MakePass(g_recon, "PS_Reconstruct"))
		return 2;

	int fx, fy;
	const int cy = (OY0 + OY1) / 2;
	{
		printf("== still: the camera doesn't move\n");
		Case cs = { "still", 0.0, 1.0f, K };
		Result r = RunCase(cs);
		SavePng(cs.name, r);
		bool same = Untouched(r, fx, fy);
		CHECK(same, "still: changed at %d,%d", fx, fy);
		printf("  %s\n", same ? "untouched" : "CHANGED");
	}
	{
		printf("== off: the camera moves, strength 0\n");
		Case cs = { "off", 8.0, 0.0f, K };
		Result r = RunCase(cs);
		bool same = Untouched(r, fx, fy);
		CHECK(same, "off: changed at %d,%d", fx, fy);
		printf("  %s\n", same ? "untouched" : "CHANGED");
	}
	{
		printf("== pan: the camera moves sideways (the wall 8 px, the block at 5 m 32 px)\n");
		Case cs = { "pan", 8.0, 1.0f, K };
		Result r = RunCase(cs);
		SavePng(cs.name, r);
		CHECK(!AnyNan(r), "pan: NaN or huge values");
		// Velocities: half the motion (the streak runs from -v to +v), clamped to K; nearer = longer.
		float vWall = At(r.prep, 20, 20, 0), vBlock = At(r.prep, OX0 + 20, cy, 0);
		printf("  half-velocity: wall %.2f px, block %.2f px (clamped to %u)\n", vWall, vBlock, K);
		CHECK(fabsf(fabsf(vWall) - 4.0f) < 0.05f, "pan: the wall's half-velocity is %.3f px, expected 4", vWall);
		CHECK(fabsf(fabsf(vBlock) - (float)K) < 0.05f && vBlock * vWall > 0, "pan: the block's half-velocity is %.3f px, expected %u", vBlock, K);
		// The stripes (period 8 px) blurred over 8 px: their contrast mostly gone.
		double in = 0, out = 0;
		for (int x = 40; x < 120; x++)
		{
			in += fabs(At(r.in, x, 20, 1) - 0.5);
			out += fabs(At(r.out, x, 20, 1) - 0.5);
		}
		printf("  stripe contrast %.3f -> %.3f\n", in / 80, out / 80);
		CHECK(out < 0.5 * in, "pan: the wall didn't blur (contrast %.3f -> %.3f)", in / 80, out / 80);
		// The nearer block streaks over the wall further than the wall's own blur reaches.
		float red = At(r.out, OX0 - 3, cy, 0) - At(r.out, OX0 - 3, cy, 1), red10 = At(r.out, OX0 - 10, cy, 0) - At(r.out, OX0 - 10, cy, 1);
		printf("  red left of the block: 3 px %.3f, 10 px %.3f\n", red, red10);
		CHECK(red > 0.05f && red10 > 0.002f, "pan: the near block doesn't streak over the wall (%.3f, %.3f)", red, red10);
		CHECK(fabsf(At(r.out, 20, 20, 0) - At(r.out, 20, 20, 1)) < 0.01f, "pan: colour bled into the far wall");
	}
	{
		printf("== short: the same with a 4 px limit\n");
		Case cs = { "short", 8.0, 1.0f, 4 };
		Result r = RunCase(cs);
		CHECK(!AnyNan(r), "short: NaN or huge values");
		float red = At(r.out, OX0 - 10, cy, 0) - At(r.out, OX0 - 10, cy, 1);
		float vBlock = At(r.prep, OX0 + 20, cy, 0);
		printf("  block's half-velocity %.2f px; red 10 px left of the block %.3f\n", vBlock, red);
		CHECK(fabsf(fabsf(vBlock) - 4.0f) < 0.05f, "short: the block's half-velocity is %.3f px, expected 4", vBlock);
		CHECK(fabsf(red) < 0.01f, "short: the streak is longer than the limit (%.3f)", red);
	}
	printf(g_failures ? "FAILED (%d)\n" : "all passed\n", g_failures);
	return g_failures ? 1 : 0;
}
