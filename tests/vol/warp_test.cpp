// Runs the real shaders (src/shaders/volumetrics.hlsl) on a synthetic town with D3D11's software
// renderer (WARP), so the fog and shaft maths are exercised without a D3D9 GPU or the game. The
// HLSL is compiled for shader model 4 in backwards-compatibility mode with VOL_D3D11 defined
// (pixel centres differ).
//
// Scene (world metres, z up, like the game): ground at z = 0, a street along +x with a row of
// buildings on each side (4 m alleys between them), a low sun ahead and to the left. The sun
// shadow atlas is built like the game's: 4 cascades side by side, light space centred on the
// camera, depth = z * nearFar.x + nearFar.y, lit when stored >= depth. The scene colour is shaded
// with the same shadows plus a game-like distance fog.
//
// Views:
//   street - camera in the street looking along it: the alleys on the sunny side let light into the
//            street, which the fog shows as shafts.
//   sun    - camera turned towards the low sun behind the buildings: screen-space shafts.
// Checks (printed PASS / FAIL): no NaNs; transmittance matches the analytic value for a uniform
// fog; shadowed fog has less in-scatter than unshadowed and shows the shafts' stripes; the sky gets
// screen-space shafts near the sun and none far from it; the composite is scene * T + light.
// Writes warp_<view>_*.png to look at.
//
// TAA:
//  - TAA off (noise offset 0): the march, blurred fog and composite are bit for bit what the shaders
//    gave before the offset was added (hashes recorded from those shaders);
//  - jitter: the ray each fog pixel marches along (the shader's own ViewRay / WorldDir, through a
//    test entry point appended to the real source) goes through the jittered pixel with TAA's
//    offset folded into P02 / P12, and misses it by the offset without;
//  - noise: the fog averaged over 16 frames with the per-frame offset the mod sets while TAA runs
//    (core::Scene_NoiseOffset) is closer to what the march converges to than one frame.
//
// Sky occlusion ([VolumetricFog] SkyOcclusion; the CPU reference traces the cover over each sample):
//  - matches the reference in the street; takes sky light away, never adds it, and leaves the
//    transmittance alone;
//  - under a low roof over everything (a hall, a tomb) the sky light drops to what the cover height
//    says; on an open plain, with no shadow map, at strength 0 or with the sun low it changes nothing
//    (bit for bit).
// Density noise ([VolumetricFog] NoiseAmount, the volume from src/vol/noise.h):
//  - the volume tiles, has mean 0.5 and a full mip chain;
//  - amount 0 gives bit for bit the fog without it; with noise the fog keeps its average level but
//    varies; it drifts smoothly (a frame's drift changes little, 20 s much more); far away, where the
//    steps are long, the mip level filters away detail the steps can't resolve.
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
#include <algorithm>
#include "vol/noise.h"

static const UINT W = 960, H = 540, DIV = 2, LW = (W + DIV - 1) / DIV, LH = (H + DIV - 1) / DIV, SDIV = 4, SW = (W + SDIV - 1) / SDIV,
				  SH = (H + SDIV - 1) / SDIV;
static const UINT TILE = 1024; // shadow cascade tile size
static const float NEAR_ = 0.1f, FAR_ = 1800.0f;
static const float A = FAR_ / (FAR_ - NEAR_), B = -NEAR_ * FAR_ / (FAR_ - NEAR_);
static const float P00 = 1.0f, P11 = 16.0f / 9.0f; // 90 degree horizontal fov

static int g_fails = 0;
static void Check(bool ok, const char* what, double a = 0, double b = 0)
{
	printf("%s  %s (%g, %g)\n", ok ? "PASS" : "FAIL", what, a, b);
	if (!ok)
		g_fails++;
}

// ---- Vector helpers ----------------------------------------------------------------------------

struct V3
{
	float x, y, z;
};
static V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
static float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static V3 Norm(V3 a) { return a * (1.0f / sqrtf(Dot(a, a))); }

// ---- Scene -------------------------------------------------------------------------------------

struct Box
{
	V3 lo, hi;
};
static std::vector<Box> g_boxes;

static void BuildTown()
{
	// Blocks 12 m long with 5 m alleys between them; the sunny side is lower, so the sun gets into
	// the street only through its alleys.
	for (int k = -3; k < 14; k++)
	{
		float x0 = 6.0f + 17.0f * k, x1 = x0 + 12.0f;
		float hl = 10.0f + 3.0f * ((k * 7 + 21) % 3), hr = 12.0f + 3.0f * ((k * 5 + 15) % 3);
		g_boxes.push_back({ { x0, 6.0f, 0.0f }, { x1, 16.0f, hl } });     // sunny-side row (left)
		g_boxes.push_back({ { x0, -16.0f, 0.0f }, { x1, -6.0f, hr } });   // right row
	}
	g_boxes.push_back({ { 2.0f, -2.0f, 0.0f }, { 3.0f, -1.0f, 3.0f } }); // a post in the street
}

static bool RayBox(V3 o, V3 d, const Box& b, float& t, float* tExit = nullptr)
{
	float t0 = 0.0f, t1 = 1e30f;
	const float* po = &o.x;
	const float* pd = &d.x;
	const float* lo = &b.lo.x;
	const float* hi = &b.hi.x;
	for (int k = 0; k < 3; k++)
	{
		if (fabsf(pd[k]) < 1e-9f)
		{
			if (po[k] < lo[k] || po[k] > hi[k])
				return false;
			continue;
		}
		float a = (lo[k] - po[k]) / pd[k], c = (hi[k] - po[k]) / pd[k];
		if (a > c)
		{
			float tmp = a;
			a = c;
			c = tmp;
		}
		t0 = a > t0 ? a : t0;
		t1 = c < t1 ? c : t1;
		if (t0 > t1)
			return false;
	}
	t = t0;
	if (tExit)
		*tExit = t1;
	return true;
}

// Nearest hit: distance, normal; false = sky.
static bool Trace(V3 o, V3 d, float& t, V3& n)
{
	t = 1e30f;
	bool hit = false;
	if (d.z < -1e-6f)
	{
		float tg = -o.z / d.z;
		if (tg > 0)
		{
			t = tg;
			n = { 0, 0, 1 };
			hit = true;
		}
	}
	for (const Box& b : g_boxes)
	{
		float tb;
		if (RayBox(o, d, b, tb) && tb < t && tb > 1e-4f)
		{
			t = tb;
			hit = true;
			V3 p = o + d * tb;
			const float e = 1e-3f;
			n = fabsf(p.x - b.lo.x) < e ? V3{ -1, 0, 0 } : fabsf(p.x - b.hi.x) < e ? V3{ 1, 0, 0 } : fabsf(p.y - b.lo.y) < e ? V3{ 0, -1, 0 }
				: fabsf(p.y - b.hi.y) < e ? V3{ 0, 1, 0 } : fabsf(p.z - b.hi.z) < e ? V3{ 0, 0, 1 } : V3{ 0, 0, -1 };
		}
	}
	return hit;
}

// ---- Sun shadow atlas, laid out like the game's ------------------------------------------------

static V3 g_sun, g_lx, g_ly, g_eye;
static const float kVanillaMax[4] = { 400, 100, 30, 10 }; // the game's default cascades, coarsest first
// The cascade rings in use: vanilla, or stretched like the shadow distance tweaks do (src/shadows.cpp
// scales the game's splits before it builds the cascades, so the constants the fog reads change).
static float kRangeMax[4] = { 400, 100, 30, 10 }, kRangeMin[4] = { 100, 30, 10, 0 };
static void SetCascades(float distanceScale, float splitScale)
{
	kRangeMax[0] = kVanillaMax[0] * distanceScale;
	for (int i = 1; i < 4; i++)
		kRangeMax[i] = fminf(kVanillaMax[i] * splitScale, kRangeMax[i - 1]); // kept ordered, as shadows.cpp does
	for (int i = 0; i < 3; i++)
		kRangeMin[i] = kRangeMax[i + 1];
	kRangeMin[3] = 0;
}
static const float kNearFar[2] = { -1.0f / 3600.0f, 1.0f / 3.0f };
static float g_light[3][4], g_scaleX[4], g_scaleY[4], g_offX[4], g_offY[4], g_fade[2];
static std::vector<float> g_atlas;

static void BuildShadowSpace()
{
	g_lx = Norm(Cross(g_sun, V3{ 0, 0, 1 }));
	g_ly = Cross(g_sun, g_lx);
	V3 axes[3] = { g_lx, g_ly, g_sun };
	for (int r = 0; r < 3; r++)
	{
		g_light[r][0] = axes[r].x;
		g_light[r][1] = axes[r].y;
		g_light[r][2] = axes[r].z;
		g_light[r][3] = -Dot(axes[r], g_eye);
	}
	for (int i = 0; i < 4; i++)
	{
		g_scaleX[i] = 1.0f / (2.0f * kRangeMax[i] * 4.0f);
		g_offX[i] = (i + 0.5f) / 4.0f;
		g_scaleY[i] = -1.0f / (2.0f * kRangeMax[i]);
		g_offY[i] = 0.5f;
	}
	g_fade[0] = -0.925f * kRangeMax[0];
	g_fade[1] = 1.0f / (0.0725f * kRangeMax[0]);
	g_atlas.assign((size_t)TILE * 4 * TILE, 1.0f);
	for (int i = 0; i < 4; i++)
		for (UINT y = 0; y < TILE; y++)
			for (UINT x = 0; x < TILE; x++)
			{
				float u = (i * TILE + x + 0.5f) / (4.0f * TILE), v = (y + 0.5f) / TILE;
				float lsx = (u - g_offX[i]) / g_scaleX[i], lsy = (v - g_offY[i]) / g_scaleY[i];
				V3 o = g_eye + g_lx * lsx + g_ly * lsy + g_sun * 1000.0f;
				float t;
				V3 n;
				if (Trace(o, g_sun * -1.0f, t, n))
					g_atlas[(size_t)y * 4 * TILE + i * TILE + x] = (1000.0f - t) * kNearFar[0] + kNearFar[1];
			}
}

// ---- Camera and the scene's colour / depth -----------------------------------------------------

static V3 g_right, g_up, g_fwd;
static const V3 kSunCol = { 1.32f, 1.11f, 0.84f }, kFogCol = { 0.55f, 0.59f, 0.65f };

static void SetCamera(float yawDeg, float pitchDeg)
{
	float yaw = yawDeg * 0.0174533f, pitch = pitchDeg * 0.0174533f;
	g_fwd = { cosf(yaw) * cosf(pitch), sinf(yaw) * cosf(pitch), sinf(pitch) };
	g_right = Norm(Cross(g_fwd, V3{ 0, 0, 1 }));
	g_up = Cross(g_right, g_fwd);
}

static V3 PixelDir(float px, float py)
{
	float ndcX = px * 2.0f / W - 1.0f, ndcY = 1.0f - py * 2.0f / H;
	return Norm(g_right * (ndcX / P00) + g_up * (ndcY / P11) + g_fwd);
}

// TAA's sub-pixel offset (pixels, x right, y down) the scene is rendered with: pixel q shows the
// unjittered image at q - offset. 0 except in the jitter check.
static float g_jitterPx[2] = { 0, 0 };

static void RenderScene(std::vector<float>& depth, std::vector<float>& colour)
{
	depth.assign((size_t)W * H, 1.0f);
	colour.assign((size_t)W * H * 4, 0.0f);
	for (UINT y = 0; y < H; y++)
		for (UINT x = 0; x < W; x++)
		{
			V3 d = PixelDir((float)x - g_jitterPx[0], (float)y - g_jitterPx[1]), n;
			float t;
			V3 c;
			if (Trace(g_eye, d, t, n))
			{
				float z = t * Dot(d, g_fwd);
				depth[(size_t)y * W + x] = A + B / z;
				V3 p = g_eye + d * t;
				float tt;
				V3 nn;
				float lit = Trace(p + n * 1e-3f, g_sun, tt, nn) ? 0.0f : 1.0f;
				float ndl = fmaxf(Dot(n, g_sun), 0.0f);
				V3 albedo = n.z > 0.5f && p.z < 0.01f ? V3{ 0.45f, 0.42f, 0.38f } : V3{ 0.7f, 0.6f, 0.5f };
				c = V3{ albedo.x * (0.35f + kSunCol.x * ndl * lit), albedo.y * (0.38f + kSunCol.y * ndl * lit), albedo.z * (0.42f + kSunCol.z * ndl * lit) };
				float f = fminf(fmaxf((t - 10.9f) * 0.0101f, 0.0f), 1.0f) * 0.596f; // the game's linear fog
				c = c + (kFogCol - c) * f;
			}
			else
			{
				float s = fmaxf(Dot(d, g_sun), 0.0f);
				V3 sky = V3{ 0.45f, 0.62f, 0.85f } + (V3{ 0.85f, 0.85f, 0.8f } - V3{ 0.45f, 0.62f, 0.85f }) * powf(1.0f - fmaxf(d.z, 0.0f), 3.0f);
				c = sky + kSunCol * (powf(s, 8.0f) * 0.5f + powf(s, 400.0f) * 4.0f);
			}
			float* o = &colour[((size_t)y * W + x) * 4];
			o[0] = c.x;
			o[1] = c.y;
			o[2] = c.z;
			o[3] = 1.0f;
		}
}

// ---- PNG ---------------------------------------------------------------------------------------

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

static void WritePngRgb(const char* path, const std::vector<float>& rgba, UINT w, UINT h, float scale = 1.0f)
{
	std::string raw;
	for (UINT y = 0; y < h; y++)
	{
		raw.push_back(0);
		for (UINT x = 0; x < w; x++)
			for (int k = 0; k < 3; k++)
			{
				float v = rgba[((size_t)y * w + x) * 4 + k] * scale;
				v = v < 0 ? 0 : v > 1 ? 1 : v;
				raw.push_back((char)(uint8_t)(powf(v, 1.0f) * 255.0f + 0.5f));
			}
	}
	std::string z = "\x78\x01";
	uint32_t a = 1, b = 0;
	for (unsigned char ch : raw)
	{
		a = (a + ch) % 65521;
		b = (b + a) % 65521;
	}
	for (size_t i = 0; i < raw.size(); i += 65535)
	{
		size_t n = raw.size() - i < 65535 ? raw.size() - i : 65535;
		z.push_back(i + n == raw.size() ? 1 : 0);
		z.push_back((char)(n & 0xFF));
		z.push_back((char)(n >> 8));
		z.push_back((char)(~n & 0xFF));
		z.push_back((char)((~n >> 8) & 0xFF));
		z.append(raw, i, n);
	}
	uint32_t ad = (b << 16) | a;
	for (int k = 3; k >= 0; k--)
		z.push_back((char)(ad >> (k * 8)));
	auto be = [](std::string& s, uint32_t v) {
		for (int k = 3; k >= 0; k--)
			s.push_back((char)(v >> (k * 8)));
	};
	auto chunk = [&](std::string& out, const char* type, const std::string& data) {
		be(out, (uint32_t)data.size());
		std::string td = std::string(type, 4) + data;
		out += td;
		be(out, Crc((const uint8_t*)td.data(), td.size()) ^ 0xFFFFFFFF);
	};
	std::string png = "\x89PNG\r\n\x1a\n", ihdr;
	be(ihdr, w);
	be(ihdr, h);
	ihdr += std::string("\x08\x02\x00\x00\x00", 5);
	chunk(png, "IHDR", ihdr);
	chunk(png, "IDAT", z);
	chunk(png, "IEND", "");
	FILE* f = fopen(path, "wb");
	if (f)
	{
		fwrite(png.data(), 1, png.size(), f);
		fclose(f);
	}
}

// ---- D3D11 plumbing ----------------------------------------------------------------------------

typedef HRESULT(WINAPI* PFN_Compile)(const void*, SIZE_T, const char*, const D3D_SHADER_MACRO*, ID3DInclude*, const char*, const char*, UINT, UINT,
	ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI* PFN_Reflect)(const void*, SIZE_T, REFIID, void**);
static PFN_Compile g_compile;
static PFN_Reflect g_reflect;
static std::string g_src;
static const GUID kIID_Reflection = { 0x8d536ca1, 0x0cca, 0x4956, { 0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84 } };
static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;

static ID3DBlob* CompileEntry(const char* entry, const char* profile, const std::string& src)
{
	const D3D_SHADER_MACRO defs[] = { { "VOL_D3D11", "1" }, { nullptr, nullptr } };
	ID3DBlob *code = nullptr, *err = nullptr;
	HRESULT hr = g_compile(src.data(), src.size(), "volumetrics.hlsl", defs, nullptr, entry, profile,
		D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
	if (FAILED(hr))
		printf("%s: compile failed: %s\n", entry, err ? (const char*)err->GetBufferPointer() : "?");
	return code;
}

// The constant registers c0..c31 by name, as in the HLSL.
static const char* const kRegNames[32] = { "c_Full", "c_Low", "c_Proj", "c_Proj2", "c_Eye", "c_Right", "c_Up", "c_Fwd", "c_Light0",
	"c_Light1", "c_Light2", "c_ShadowZ", "c_RangeMin", "c_RangeMax", "c_ScaleX", "c_ScaleY", "c_OffsetX", "c_OffsetY", "c_Bias", "c_Fade",
	"c_Cloud", "c_SunDir", "c_SunCol", "c_Ambient", "c_Medium", "c_Blur", "c_Shafts", "c_Shafts2", "c_ShaftCol", "c_ShRes", "c_Noise",
	"c_Noise2" };

struct Pass
{
	ID3D11PixelShader* ps = nullptr;
	ID3D11Buffer* cb = nullptr;
	std::vector<std::pair<int, UINT>> regOffsets; // register -> byte offset in $Globals
	std::vector<std::pair<int, UINT>> texSlots;   // our sampler register (s0..s6) -> SM4 texture slot (packed in compatibility mode)
	UINT cbSize = 0;
	int noiseSampler = -1; // SM4 sampler slot of s_Noise (the others keep the slots they always had)
};

static Pass MakePass(const char* entry, const std::string* src = nullptr)
{
	Pass p;
	ID3DBlob* code = CompileEntry(entry, "ps_4_0", src ? *src : g_src);
	if (!code)
		return p;
	g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &p.ps);
	ID3D11ShaderReflection* refl = nullptr;
	g_reflect(code->GetBufferPointer(), code->GetBufferSize(), kIID_Reflection, (void**)&refl);
	static const char* const kSamplers[7] = { "s_Depth", "s_Shadow", "s_Cloud", "s_Fog", "s_Scene", "s_Shafts", "s_Noise" };
	D3D11_SHADER_DESC sdesc = {};
	if (refl && SUCCEEDED(refl->GetDesc(&sdesc)))
		for (UINT i = 0; i < sdesc.BoundResources; i++)
		{
			D3D11_SHADER_INPUT_BIND_DESC b;
			refl->GetResourceBindingDesc(i, &b);
			for (int k = 0; k < 7; k++)
				if (b.Type == D3D_SIT_TEXTURE && !strcmp(b.Name, kSamplers[k]))
					p.texSlots.push_back({ k, b.BindPoint });
			if (b.Type == D3D_SIT_SAMPLER && !strcmp(b.Name, "s_Noise"))
				p.noiseSampler = (int)b.BindPoint;
		}
	ID3D11ShaderReflectionConstantBuffer* cb = refl ? refl->GetConstantBufferByName("$Globals") : nullptr;
	D3D11_SHADER_BUFFER_DESC bd = {};
	if (cb && SUCCEEDED(cb->GetDesc(&bd)))
	{
		p.cbSize = (bd.Size + 15) & ~15u;
		for (UINT i = 0; i < bd.Variables; i++)
		{
			D3D11_SHADER_VARIABLE_DESC vd;
			cb->GetVariableByIndex(i)->GetDesc(&vd);
			for (int r = 0; r < 32; r++)
				if (!strcmp(vd.Name, kRegNames[r]))
					p.regOffsets.push_back({ r, vd.StartOffset });
		}
		D3D11_BUFFER_DESC d = { p.cbSize, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
		g_dev->CreateBuffer(&d, nullptr, &p.cb);
	}
	return p;
}

struct Tex
{
	ID3D11Texture2D* t = nullptr;
	ID3D11ShaderResourceView* srv = nullptr;
	ID3D11RenderTargetView* rtv = nullptr;
	UINT w = 0, h = 0;
};

static Tex MakeTex(UINT w, UINT h, DXGI_FORMAT fmt, bool rt, const void* init = nullptr, UINT pitch = 0)
{
	Tex t;
	t.w = w;
	t.h = h;
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = w;
	d.Height = h;
	d.MipLevels = d.ArraySize = 1;
	d.Format = fmt;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (rt ? D3D11_BIND_RENDER_TARGET : 0);
	D3D11_SUBRESOURCE_DATA sd = { init, pitch, 0 };
	g_dev->CreateTexture2D(&d, init ? &sd : nullptr, &t.t);
	g_dev->CreateShaderResourceView(t.t, nullptr, &t.srv);
	if (rt)
		g_dev->CreateRenderTargetView(t.t, nullptr, &t.rtv);
	return t;
}

static std::vector<float> ReadBack(const Tex& t)
{
	D3D11_TEXTURE2D_DESC d;
	t.t->GetDesc(&d);
	d.Usage = D3D11_USAGE_STAGING;
	d.BindFlags = 0;
	d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ID3D11Texture2D* st = nullptr;
	g_dev->CreateTexture2D(&d, nullptr, &st);
	g_ctx->CopyResource(st, t.t);
	D3D11_MAPPED_SUBRESOURCE m;
	std::vector<float> out((size_t)t.w * t.h * 4);
	if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m)))
	{
		for (UINT y = 0; y < t.h; y++)
		{
			const uint16_t* row = (const uint16_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch);
			for (UINT x = 0; x < t.w * 4; x++)
			{
				uint16_t h = row[x];
				uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, f = h & 0x3FF, bits;
				if (e == 0)
					bits = f ? 0 : s << 31; // denormals as 0
				else if (e == 31)
					bits = (s << 31) | 0x7F800000 | (f << 13);
				else
					bits = (s << 31) | ((e + 112) << 23) | (f << 13);
				if (e == 0 && f)
					bits = s << 31;
				float v;
				memcpy(&v, &bits, 4);
				out[(size_t)y * t.w * 4 + x] = v;
			}
		}
		g_ctx->Unmap(st, 0);
	}
	st->Release();
	return out;
}

// FNV-1a over a target's texels: two renders hash the same only if every bit agrees.
static uint64_t HashTex(const Tex& t, UINT bytesPerTexel)
{
	D3D11_TEXTURE2D_DESC d;
	t.t->GetDesc(&d);
	d.Usage = D3D11_USAGE_STAGING;
	d.BindFlags = 0;
	d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ID3D11Texture2D* st = nullptr;
	g_dev->CreateTexture2D(&d, nullptr, &st);
	g_ctx->CopyResource(st, t.t);
	D3D11_MAPPED_SUBRESOURCE m;
	uint64_t h = 1469598103934665603ull;
	if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m)))
	{
		for (UINT y = 0; y < t.h; y++)
		{
			const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
			for (size_t i = 0; i < (size_t)t.w * bytesPerTexel; i++)
				h = (h ^ row[i]) * 1099511628211ull;
		}
		g_ctx->Unmap(st, 0);
	}
	st->Release();
	return h;
}

// A 32-bit float RGBA target.
static std::vector<float> ReadBack32(const Tex& t)
{
	D3D11_TEXTURE2D_DESC d;
	t.t->GetDesc(&d);
	d.Usage = D3D11_USAGE_STAGING;
	d.BindFlags = 0;
	d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ID3D11Texture2D* st = nullptr;
	g_dev->CreateTexture2D(&d, nullptr, &st);
	g_ctx->CopyResource(st, t.t);
	D3D11_MAPPED_SUBRESOURCE m;
	std::vector<float> out((size_t)t.w * t.h * 4);
	if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m)))
	{
		for (UINT y = 0; y < t.h; y++)
			memcpy(&out[(size_t)y * t.w * 4], (const uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)t.w * 16);
		g_ctx->Unmap(st, 0);
	}
	st->Release();
	return out;
}

static ID3D11VertexShader* g_vs;
static ID3D11SamplerState* g_samplers[6];
static ID3D11SamplerState* g_noiseSampler; // linear, mip linear, wrap (the plugin's s6)
static ID3D11BlendState *g_blendComposite, *g_blendOff;

static void Run(Pass& p, const float c[32][4], const Tex& target, ID3D11ShaderResourceView* const srvs[7], bool composite = false)
{
	std::vector<uint8_t> buf(p.cbSize, 0);
	for (auto& ro : p.regOffsets)
		if (ro.second + 16 <= p.cbSize)
			memcpy(&buf[ro.second], c[ro.first], 16);
	if (p.cb)
		g_ctx->UpdateSubresource(p.cb, 0, nullptr, buf.data(), 0, 0);
	ID3D11ShaderResourceView* none[7] = {};
	g_ctx->PSSetShaderResources(0, 7, none);
	g_ctx->OMSetRenderTargets(1, &target.rtv, nullptr);
	D3D11_VIEWPORT vp = { 0, 0, (float)target.w, (float)target.h, 0, 1 };
	g_ctx->RSSetViewports(1, &vp);
	g_ctx->VSSetShader(g_vs, nullptr, 0);
	g_ctx->PSSetShader(p.ps, nullptr, 0);
	g_ctx->PSSetConstantBuffers(0, 1, &p.cb);
	ID3D11ShaderResourceView* packed[7] = {};
	for (auto& ts : p.texSlots)
		packed[ts.second] = srvs[ts.first];
	g_ctx->PSSetShaderResources(0, 7, packed);
	g_ctx->PSSetSamplers(0, 6, g_samplers);
	if (p.noiseSampler >= 0)
		g_ctx->PSSetSamplers((UINT)p.noiseSampler, 1, &g_noiseSampler);
	float bf[4] = {};
	g_ctx->OMSetBlendState(composite ? g_blendComposite : g_blendOff, bf, 0xFFFFFFFF);
	g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	g_ctx->Draw(3, 0);
	ID3D11RenderTargetView* nort = nullptr;
	g_ctx->OMSetRenderTargets(1, &nort, nullptr);
}

static bool AnyNan(const std::vector<float>& v)
{
	for (float f : v)
		if (f != f || fabsf(f) > 1e30f)
			return true;
	return false;
}

// ---- One view ------------------------------------------------------------------------------------

struct Settings
{
	// The plugin's defaults ([VolumetricFog] / [SunShafts] in AC2ShadowOverhaul.ini), except the denser fog the checks use.
	float density = 0.004f, falloff = 0.02f, base = 0.0f, sun = 0.35f, g = 0.45f, ambient = 0.25f, maxDist = 300.0f, steps = 40;
	float shaftIntensity = 0.35f, shaftThreshold = 0.5f;
	bool shadows = true, shafts = true;
	float view = 0;
	float noise = 0;        // c_Medium.w: the per-frame noise offset (0 without TAA)
	bool narrowBlur = false; // a narrower fog blur (sigma 1), measured against the mod's with TAA (not used by the mod)
	float skyOcc = 0;        // [VolumetricFog] SkyOcclusion (0 here, so the checks above see the fog as it was)
	float skyFog = 0;        // [VolumetricFog] SkyFog (0 here: the sky only gains the fog's light, as the checks above expect)
	float noiseAmount = 0, noiseSize = 15.0f, windSpeed = 1.5f, windDir = 30.0f; // [VolumetricFog] Noise*, Wind*
	double time = 0;         // seconds on the plugin's clock
	float noiseLodBias = 0.5f; // the plugin's extra half mip level (-20: always the finest level)
};

struct Outputs
{
	std::vector<float> march, fog, shafts, composite, scene, depth;
	uint64_t hashMarch = 0, hashFog = 0, hashComposite = 0;
};

static Pass g_march, g_blur, g_blurNarrow, g_mask, g_radial, g_comp;
static Tex g_depthTex, g_atlasTex, g_cloudTex, g_fogT[2], g_shaftT[2], g_sceneTex, g_outTex;
static ID3D11ShaderResourceView* g_noiseSrv;

// reuseScene: the scene (depth, colour) of the last call, for repeated runs of one view.
static Outputs RunView(const Settings& s, bool reuseScene = false)
{
	Outputs o;
	static std::vector<float> lastDepth, lastScene;
	if (!reuseScene || lastDepth.empty())
		RenderScene(lastDepth, lastScene);
	o.depth = lastDepth;
	o.scene = lastScene;
	g_depthTex = MakeTex(W, H, DXGI_FORMAT_R32_FLOAT, false, o.depth.data(), W * 4);
	g_sceneTex = MakeTex(W, H, DXGI_FORMAT_R32G32B32A32_FLOAT, false, o.scene.data(), W * 16);
	g_outTex = MakeTex(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, true);

	float c[32][4] = {};
	auto set4 = [&](int r, float x, float y, float z, float w) {
		c[r][0] = x;
		c[r][1] = y;
		c[r][2] = z;
		c[r][3] = w;
	};
	auto copy4 = [&](int r, const float* v) { memcpy(c[r], v, 16); };
	set4(0, (float)W, (float)H, 1.0f / W, 1.0f / H);
	set4(1, (float)LW, (float)LH, 1.0f / LW, 1.0f / LH);
	set4(2, A, B, P00, P11);
	// P02 / P12 with TAA's offset folded in, as the mod sets them (0 without TAA).
	set4(3, 2.0f * g_jitterPx[0] / W, -2.0f * g_jitterPx[1] / H, (float)DIV, A + B / (0.99f * FAR_));
	set4(4, g_eye.x, g_eye.y, g_eye.z, s.maxDist);
	set4(5, g_right.x, g_right.y, g_right.z, s.steps);
	set4(6, g_up.x, g_up.y, g_up.z, s.view);
	set4(7, g_fwd.x, g_fwd.y, g_fwd.z, (float)SDIV);
	for (int r = 0; r < 3; r++)
		copy4(8 + r, g_light[r]);
	set4(11, kNearFar[0], kNearFar[1], s.shadows ? 1.0f : 0.0f, 0);
	copy4(12, kRangeMin);
	copy4(13, kRangeMax);
	copy4(14, g_scaleX);
	copy4(15, g_scaleY);
	copy4(16, g_offX);
	copy4(17, g_offY);
	set4(19, g_fade[0], g_fade[1], 0, 0);
	set4(21, g_sun.x, g_sun.y, g_sun.z, s.g);
	set4(22, kSunCol.x * s.sun, kSunCol.y * s.sun, kSunCol.z * s.sun, 0);
	set4(23, kFogCol.x * s.ambient, kFogCol.y * s.ambient, kFogCol.z * s.ambient, 0);
	set4(24, s.density, s.falloff, s.base, s.noise);
	set4(27, s.shaftThreshold, 12.0f, s.skyFog, 0);
	set4(29, (float)SW, (float)SH, 1.0f / SW, 1.0f / SH);
	// Sky occlusion and noise, as vol::Apply sets them.
	if (s.shadows && g_sun.z > 0)
	{
		c[31][2] = s.skyOcc * fminf(fmaxf((g_sun.z - 0.1f) / 0.25f, 0.0f), 1.0f);
		c[31][3] = g_sun.z / fabsf(kNearFar[0]);
	}
	if (s.noiseAmount > 0)
	{
		const double tile = 8.0 * s.noiseSize, dir = s.windDir * (3.14159265358979 / 180.0);
		const double wind[3] = { cos(dir) * s.windSpeed, sin(dir) * s.windSpeed, 0.25 * s.windSpeed };
		const float* eye = &g_eye.x;
		for (int k = 0; k < 3; k++)
		{
			const double u = (eye[k] - wind[k] * s.time) / tile;
			c[30][k] = (float)(u - floor(u));
		}
		c[30][3] = s.noiseAmount;
		c[31][0] = (float)(1.0 / tile);
		c[31][1] = (float)(log2(64.0 / tile) + s.noiseLodBias);
	}

	// Sun on screen (as the plugin computes it).
	float sx = Dot(g_right, g_sun), sy = Dot(g_up, g_sun), sz = Dot(g_fwd, g_sun);
	float sunUv[2] = { 0.5f, 0.5f }, fade = 0;
	if (sz > 0.02f)
	{
		float ndcX = P00 * sx / sz, ndcY = P11 * sy / sz;
		sunUv[0] = ndcX * 0.5f + 0.5f + 0.5f / W;
		sunUv[1] = 0.5f - ndcY * 0.5f + 0.5f / H;
		float edge = fmaxf(fabsf(ndcX), fabsf(ndcY));
		fade = fminf(fmaxf((sz - 0.02f) * 5.0f, 0.0f), 1.0f) * fminf(fmaxf((3.0f - edge) * 0.5f, 0.0f), 1.0f);
	}
	bool shafts = s.shafts && fade > 0;
	if (shafts)
		set4(28, kSunCol.x * s.shaftIntensity * fade, kSunCol.y * s.shaftIntensity * fade, kSunCol.z * s.shaftIntensity * fade, 1);

	ID3D11ShaderResourceView* srv[7] = { g_depthTex.srv, g_atlasTex.srv, g_cloudTex.srv, nullptr, nullptr, nullptr, g_noiseSrv };
	Run(g_march, c, g_fogT[0], srv);
	o.march = ReadBack(g_fogT[0]);
	o.hashMarch = HashTex(g_fogT[0], 8);
	Pass& blur = s.narrowBlur ? g_blurNarrow : g_blur;
	set4(25, 1, 0, 0, 0);
	srv[3] = g_fogT[0].srv;
	Run(blur, c, g_fogT[1], srv);
	set4(25, 0, 1, 0, 0);
	srv[3] = g_fogT[1].srv;
	Run(blur, c, g_fogT[0], srv);
	srv[3] = nullptr;
	if (shafts)
	{
		srv[4] = g_sceneTex.srv;
		Run(g_mask, c, g_shaftT[0], srv);
		srv[4] = nullptr;
		set4(26, sunUv[0], sunUv[1], 0.75f / 16.0f, 0.93f);
		srv[5] = g_shaftT[0].srv;
		Run(g_radial, c, g_shaftT[1], srv);
		set4(26, sunUv[0], sunUv[1], 0.75f / 256.0f, 1.0f);
		srv[5] = g_shaftT[1].srv;
		Run(g_radial, c, g_shaftT[0], srv);
	}
	// Composite onto a copy of the scene.
	{
		D3D11_BOX box = { 0, 0, 0, W, H, 1 };
		(void)box;
		std::vector<uint16_t> half((size_t)W * H * 4);
		for (size_t i = 0; i < half.size(); i++)
		{
			float f = o.scene[i];
			uint32_t b;
			memcpy(&b, &f, 4);
			uint32_t sgn = (b >> 16) & 0x8000, e = (b >> 23) & 0xFF, m = b & 0x7FFFFF;
			int ne = (int)e - 127 + 15;
			half[i] = (uint16_t)(ne <= 0 ? sgn : ne >= 31 ? sgn | 0x7C00 : sgn | (ne << 10) | (m >> 13));
		}
		g_ctx->UpdateSubresource(g_outTex.t, 0, nullptr, half.data(), W * 8, 0);
	}
	srv[3] = g_fogT[0].srv;
	srv[5] = g_shaftT[0].srv;
	Run(g_comp, c, g_outTex, srv, true);
	o.fog = ReadBack(g_fogT[0]);
	o.hashFog = HashTex(g_fogT[0], 8);
	o.shafts = shafts ? ReadBack(g_shaftT[0]) : std::vector<float>((size_t)SW * SH * 4, 0.0f);
	o.composite = ReadBack(g_outTex);
	o.hashComposite = HashTex(g_outTex, 8);
	g_depthTex.srv->Release();
	g_depthTex.t->Release();
	g_sceneTex.srv->Release();
	g_sceneTex.t->Release();
	return o;
}

static std::vector<float> FogRgb(const std::vector<float>& fog, bool transmittance)
{
	std::vector<float> v(fog.size());
	for (size_t i = 0; i < fog.size(); i += 4)
		for (int k = 0; k < 3; k++)
		{
			v[i + k] = transmittance ? fog[i + 3] : fog[i + k];
			v[i + 3] = 1;
		}
	return v;
}

// ---- CPU reference for the ray march -------------------------------------------------------------
// The same integration as PS_FogMarch (sample positions, jitter, height density, phase), but with
// sunlight visibility traced against the actual boxes instead of looked up in the shadow atlas.
// Differences come only from the atlas's texel size and the shader's float precision.

static void Reference(UINT lx, UINT ly, const Settings& s, float out[4])
{
	float fx = fminf((float)(lx * DIV + DIV / 2), (float)W - 1), fy = fminf((float)(ly * DIV + DIV / 2), (float)H - 1);
	V3 dir = PixelDir(fx, fy), n;
	float t;
	float dist = Trace(g_eye, dir, t, n) ? t : s.maxDist;
	float tEnd = fminf(dist, s.maxDist);
	float jitter = (float)fmod(52.9829189 * fmod(0.06711056 * lx + 0.00583715 * ly, 1.0), 1.0);
	jitter = jitter + s.noise - floorf(jitter + s.noise);
	float c = Dot(dir, g_sun), g = s.g;
	float phase = (1 - g * g) / powf(fmaxf(1 + g * g - 2 * g * c, 1e-4f), 1.5f);
	float T = 1;
	V3 S = { 0, 0, 0 };
	for (int i = 0; i < (int)s.steps; i++)
	{
		float x0 = i / s.steps, x1 = (i + 1) / s.steps;
		float t0 = tEnd * x0 * x0, t1 = tEnd * x1 * x1, ts = t0 + (t1 - t0) * jitter;
		V3 P = g_eye + dir * ts;
		float density = s.density * expf(-s.falloff * fmaxf(P.z - s.base, 0.0f));
		float tt;
		V3 nn;
		float vis = !s.shadows ? 1.0f : Trace(P, g_sun, tt, nn) ? 0.0f : 1.0f;
		// Sky occlusion: the cover the sun sees over the sample (its sun-most surface: the farthest
		// exit along the ray) and its height above it, against 20 m (under a metre: open, see SunAndSky).
		float sky = 1.0f;
		if (vis == 0.0f && s.skyOcc > 0)
		{
			float far_ = 0, te;
			for (const Box& b : g_boxes)
				if (RayBox(P, g_sun, b, tt, &te))
					far_ = fmaxf(far_, te);
			const float cover = far_ * g_sun.z;
			float open = fmaxf(fminf(cover / 20.0f, 1.0f), fminf(fmaxf(2.0f - cover, 0.0f), 1.0f));
			sky = 1.0f - s.skyOcc * fminf(fmaxf((g_sun.z - 0.1f) / 0.25f, 0.0f), 1.0f) * (1.0f - open);
		}
		V3 L = kSunCol * (s.sun * phase * vis) + kFogCol * (s.ambient * sky);
		float st = expf(-density * (t1 - t0));
		S = S + L * (T * (1 - st));
		T *= st;
	}
	out[0] = S.x;
	out[1] = S.y;
	out[2] = S.z;
	out[3] = T;
}

static void CheckAgainstReference(const Outputs& o, const Settings& s, const char* view)
{
	std::vector<float> err;
	float worstT = 0;
	unsigned seed = 12345;
	for (int k = 0; k < 400; k++)
	{
		seed = seed * 1103515245u + 12345u;
		UINT lx = (seed >> 8) % LW;
		seed = seed * 1103515245u + 12345u;
		UINT ly = (seed >> 8) % LH;
		float ref[4];
		Reference(lx, ly, s, ref);
		const float* g = &o.march[((size_t)ly * LW + lx) * 4];
		err.push_back(fabsf(g[1] - ref[1]) / fmaxf(ref[1], 0.02f));
		worstT = fmaxf(worstT, fabsf(g[3] - ref[3]));
	}
	std::sort(err.begin(), err.end());
	char what[160];
	snprintf(what, sizeof(what), "%s: ray march matches the CPU reference (median / 90%% relative error of the in-scatter)", view);
	Check(err[err.size() / 2] < 0.03f && err[err.size() * 9 / 10] < 0.15f, what, err[err.size() / 2], err[err.size() * 9 / 10]);
	snprintf(what, sizeof(what), "%s: transmittance matches the CPU reference (worst difference)", view);
	Check(worstT < 0.01f, what, worstT, 0.01);
}

// Hashes of the street view's march, fog and composite, then the beams view's, with the noise
// offset at 0, recorded from the shaders before the offset was added (src/vol/shaders/volumetrics.hlsl
// of commit 7a64ec5): without TAA nothing may change, not one bit. (Recorded on WARP; a different
// WARP version could round differently, in which case the shaders of that commit give the new numbers.)
static const uint64_t kToday[6] = { 0xD230F5FE078380B3ull, 0x85A7B7098B73E03Dull, 0x32409D31E90E8CD1ull, 0x41C86D2BAFC8E453ull,
	0xBC8C1D65F6B16501ull, 0x96915E45B808CEE9ull };

int main(int argc, char** argv)
{
	(void)argc;
	(void)argv;
	FILE* f = fopen("../../src/vol/shaders/volumetrics.hlsl", "rb");
	if (!f)
	{
		printf("run from tests/vol (needs ../../src/vol/shaders/volumetrics.hlsl)\n");
		return 1;
	}
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		g_src.append(buf, n);
	fclose(f);
	HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
	g_compile = dc ? (PFN_Compile)GetProcAddress(dc, "D3DCompile") : nullptr;
	g_reflect = dc ? (PFN_Reflect)GetProcAddress(dc, "D3DReflect") : nullptr;
	if (!g_compile || !g_reflect)
	{
		printf("d3dcompiler_47.dll not found\n");
		return 1;
	}
	D3D_FEATURE_LEVEL fl;
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_dev, &fl, &g_ctx)))
	{
		printf("no WARP device\n");
		return 1;
	}
	const char* vsSrc = "float4 main(uint id : SV_VertexID) : SV_Position { float2 p = float2(id == 2 ? 3 : -1, id == 1 ? 3 : -1); return float4(p, 0, 1); }";
	ID3DBlob *vsCode = nullptr, *err = nullptr;
	g_compile(vsSrc, strlen(vsSrc), "vs", nullptr, nullptr, "main", "vs_4_0", 0, 0, &vsCode, &err);
	g_dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &g_vs);
	g_march = MakePass("PS_FogMarch");
	g_blur = MakePass("PS_FogBlur");
	{
		// A narrower fog blur (sigma 1), only measured (TAA section below): not used by the mod.
		std::string n = g_src;
		const char* from = "{ 0.8825, 0.6065, 0.3247, 0.1353 }; // exp(-k^2 / 8)";
		size_t at = n.find(from);
		n.replace(at, strlen(from), "{ 0.6065, 0.1353, 0.0111, 0.0003 };");
		g_blurNarrow = MakePass("PS_FogBlur", &n);
	}
	g_mask = MakePass("PS_ShaftsMask");
	g_radial = MakePass("PS_ShaftsBlur");
	g_comp = MakePass("PS_Composite");
	if (!g_march.ps || !g_blur.ps || !g_mask.ps || !g_radial.ps || !g_comp.ps)
		return 1;
	struct
	{
		D3D11_FILTER f;
		D3D11_TEXTURE_ADDRESS_MODE a;
	} const sm[6] = { { D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP }, { D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP },
		{ D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_WRAP }, { D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP },
		{ D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP }, { D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_BORDER } };
	for (int i = 0; i < 6; i++)
	{
		D3D11_SAMPLER_DESC d = {};
		d.Filter = sm[i].f;
		d.AddressU = d.AddressV = d.AddressW = sm[i].a;
		d.MaxLOD = 0;
		d.ComparisonFunc = D3D11_COMPARISON_NEVER;
		g_dev->CreateSamplerState(&d, &g_samplers[i]);
	}
	{
		D3D11_SAMPLER_DESC d = {};
		d.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		d.MaxLOD = D3D11_FLOAT32_MAX;
		d.ComparisonFunc = D3D11_COMPARISON_NEVER;
		g_dev->CreateSamplerState(&d, &g_noiseSampler);
	}
	// The noise volume the plugin uploads (src/vol/noise.h, 64^3 L8 with mips -> R8_UNORM).
	std::vector<std::vector<uint8_t>> noiseLevels;
	vol::MakeNoiseVolume(64, noiseLevels);
	{
		D3D11_TEXTURE3D_DESC d = {};
		d.Width = d.Height = d.Depth = 64;
		d.MipLevels = (UINT)noiseLevels.size();
		d.Format = DXGI_FORMAT_R8_UNORM;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		std::vector<D3D11_SUBRESOURCE_DATA> init(noiseLevels.size());
		for (size_t l = 0; l < noiseLevels.size(); l++)
		{
			const UINT n = 64u >> l;
			init[l] = { noiseLevels[l].data(), n, n * n };
		}
		ID3D11Texture3D* t = nullptr;
		if (FAILED(g_dev->CreateTexture3D(&d, init.data(), &t)) || FAILED(g_dev->CreateShaderResourceView(t, nullptr, &g_noiseSrv)))
		{
			printf("could not create the noise volume\n");
			return 1;
		}
	}
	D3D11_BLEND_DESC bd = {};
	bd.RenderTarget[0].BlendEnable = TRUE;
	bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
	bd.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_ALPHA;
	bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
	bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
	bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
	g_dev->CreateBlendState(&bd, &g_blendComposite);
	bd.RenderTarget[0].BlendEnable = FALSE;
	bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	g_dev->CreateBlendState(&bd, &g_blendOff);

	// Scene and shadow atlas.
	BuildTown();
	g_eye = { 0.0f, 0.0f, 1.7f };
	float az = 75.0f * 0.0174533f, el = 22.0f * 0.0174533f;
	g_sun = { cosf(az) * cosf(el), sinf(az) * cosf(el), sinf(el) };
	BuildShadowSpace();
	g_atlasTex = MakeTex(4 * TILE, TILE, DXGI_FORMAT_R32_FLOAT, false, g_atlas.data(), 4 * TILE * 4);
	const float white[4] = { 1, 1, 1, 1 };
	g_cloudTex = MakeTex(1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, false, white, 16);
	for (int i = 0; i < 2; i++)
	{
		g_fogT[i] = MakeTex(LW, LH, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
		g_shaftT[i] = MakeTex(SW, SH, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
	}

	// ---- Street view: shafts in the fog -----------------------------------------------------------
	SetCamera(0.0f, 4.0f);
	Settings s;
	Outputs lit = RunView(s);
	CheckAgainstReference(lit, s, "street");
	Settings ns = s;
	ns.shadows = false;
	Outputs unshadowed = RunView(ns);
	WritePngRgb("warp_street_scene.png", lit.scene, W, H);
	WritePngRgb("warp_street_composite.png", lit.composite, W, H);
	WritePngRgb("warp_street_fog.png", FogRgb(lit.fog, false), LW, LH, 2.0f);
	WritePngRgb("warp_street_fog_unshadowed.png", FogRgb(unshadowed.fog, false), LW, LH, 2.0f);
	WritePngRgb("warp_street_transmittance.png", FogRgb(lit.fog, true), LW, LH);
	Check(!AnyNan(lit.fog) && !AnyNan(lit.composite) && !AnyNan(unshadowed.fog), "street: no NaN / Inf");

	// In-scatter: shadowed <= unshadowed everywhere (same transmittance), and much less on average
	// (the street is mostly in the shadow of the sunny-side buildings).
	double sumLit = 0, sumUn = 0;
	int worse = 0;
	for (size_t i = 0; i < lit.fog.size(); i += 4)
	{
		sumLit += lit.fog[i + 1];
		sumUn += unshadowed.fog[i + 1];
		if (lit.fog[i + 1] > unshadowed.fog[i + 1] * 1.02f + 1e-3f)
			worse++;
		if (fabsf(lit.fog[i + 3] - unshadowed.fog[i + 3]) > 2e-3f)
			worse++;
	}
	Check(worse == 0, "street: shadowed in-scatter never above unshadowed, same transmittance", worse, 0);
	Check(sumLit < 0.8 * sumUn, "street: shadows remove a good share of the scattered sunlight", sumLit / sumUn, 0.8);

	// Transmittance on a sky pixel with a uniform fog (no height falloff): exp(-density x distance).
	Settings u = s;
	u.falloff = 0;
	u.shadows = false;
	Outputs uni = RunView(u);
	{
		UINT x = LW / 2, y = 2; // top of the screen: sky
		float T = uni.fog[((size_t)y * LW + x) * 4 + 3];
		float expect = expf(-u.density * u.maxDist);
		Check(fabsf(T - expect) < 0.01f, "uniform fog: sky transmittance = exp(-density x distance)", T, expect);
		// and on the street ahead (ground pixel low in the middle): exp(-density x distance to it)
		UINT fx = W / 2, fy = H - 20;
		V3 d = PixelDir((float)fx, (float)fy), nn;
		float t;
		Trace(g_eye, d, t, nn);
		UINT lx = fx / DIV, ly = fy / DIV;
		float Tg = uni.fog[((size_t)ly * LW + lx) * 4 + 3], eg = expf(-u.density * t);
		Check(fabsf(Tg - eg) < 0.01f, "uniform fog: ground transmittance = exp(-density x distance)", Tg, eg);
	}
	// Composite = scene * T + in-scatter (no shafts on screen in this view).
	{
		int bad = 0;
		for (UINT y = 0; y < H; y += 7)
			for (UINT x = 0; x < W; x += 7)
			{
				size_t i = ((size_t)y * W + x) * 4;
				// nearest fog-res pixel stands in for the upsampled value; compare loosely in flat areas
				size_t li = ((size_t)(y / DIV) * LW + x / DIV) * 4;
				bool sky = lit.depth[(size_t)y * W + x] >= 1.0f; // the sky is never dimmed
				float expect = lit.scene[i] * (sky ? 1.0f : lit.fog[li + 3]) + lit.fog[li];
				if (fabsf(lit.composite[i] - expect) > 0.08f)
					bad++;
			}
		Check(bad < 60, "street: composite = scene x transmittance + scattered light", bad, 60);
	}

	// ---- Sun view: screen-space shafts ------------------------------------------------------------
	// Opposite the alley at x = 18..23, looking up it towards the sun.
	g_eye = { 20.5f - 14.0f / tanf(az), -3.0f, 1.7f };
	BuildShadowSpace();
	g_atlasTex.srv->Release();
	g_atlasTex.t->Release();
	g_atlasTex = MakeTex(4 * TILE, TILE, DXGI_FORMAT_R32_FLOAT, false, g_atlas.data(), 4 * TILE * 4);
	SetCamera(75.0f, 12.0f);
	Outputs sun = RunView(s);
	CheckAgainstReference(sun, s, "sun");
	Settings so = s;
	so.view = 3;
	Outputs sunOnly = RunView(so);
	WritePngRgb("warp_sun_scene.png", sun.scene, W, H);
	WritePngRgb("warp_sun_composite.png", sun.composite, W, H);
	WritePngRgb("warp_sun_fog.png", FogRgb(sun.fog, false), LW, LH, 1.0f);
	WritePngRgb("warp_sun_shafts.png", sun.shafts, SW, SH, 2.0f);
	WritePngRgb("warp_sun_shafts_only.png", sunOnly.composite, W, H);
	Check(!AnyNan(sun.fog) && !AnyNan(sun.shafts) && !AnyNan(sun.composite), "sun: no NaN / Inf");
	{
		float sx = Dot(g_right, g_sun), sy = Dot(g_up, g_sun), sz = Dot(g_fwd, g_sun);
		int px = (int)((P00 * sx / sz * 0.5f + 0.5f) * SW), py = (int)((0.5f - P11 * sy / sz * 0.5f) * SH);
		float nearSun = 0, farAway = 0;
		for (int dy = -6; dy <= 6; dy++)
			for (int dx = -6; dx <= 6; dx++)
			{
				int x = px + dx, y = py + dy;
				if (x >= 0 && y >= 0 && x < (int)SW && y < (int)SH)
					nearSun = fmaxf(nearSun, sun.shafts[((size_t)y * SW + x) * 4 + 1]);
			}
		for (UINT y = 0; y < SH / 4; y++)
			for (UINT x = SW * 3 / 4; x < SW; x++)
				farAway = fmaxf(farAway, sun.shafts[((size_t)y * SW + x) * 4 + 1]);
		Check(nearSun > 0.05f && farAway < 0.25f * nearSun, "sun: shafts around the sun, not far from it", nearSun, farAway);
		// Looking towards the sun the fog's forward scattering is brighter than looking along the street.
		double sumSun = 0, sumStreet = 0;
		for (size_t i = 0; i < sun.fog.size(); i += 4)
		{
			sumSun += sun.fog[i];
			sumStreet += unshadowed.fog[i];
		}
		Check(sumSun > 0, "sun: fog scatters light", sumSun, sumStreet);
		// The sky, and the HDR sun in it, is never dimmed (the game's bloom blurs the scene as it is).
		int dimmed = 0, skyPx = 0;
		for (size_t p = 0; p < (size_t)W * H; p++)
			if (sun.depth[p] >= 1.0f)
			{
				skyPx++;
				for (int k = 0; k < 3; k++)
					if (sun.composite[p * 4 + k] < sun.scene[p * 4 + k] * 0.9999f)
						dimmed++;
			}
		Check(skyPx > 0 && dimmed == 0, "sun: sky and sun keep their full brightness", dimmed, skyPx);

		// SkyFog 1: the sky is seen through the fog like the scene (scene x T + light), except the sun
		// and the sky within 2 degrees of it, which keep their full brightness.
		Settings sf = s;
		sf.skyFog = 1.0f;
		sf.shafts = false; // the checks below leave the shafts out
		Outputs skyFog = RunView(sf, true);
		WritePngRgb("warp_sun_composite_skyfog.png", skyFog.composite, W, H);
		Check(!AnyNan(skyFog.composite), "sky fog: no NaN / Inf");
		int nearDimmed = 0, nearPx = 0, bad = 0, checked = 0, brighter = 0;
		for (UINT y = 0; y < H; y++)
			for (UINT x = 0; x < W; x++)
			{
				size_t p = (size_t)y * W + x;
				if (skyFog.depth[p] < 1.0f)
					continue;
				float towardsSun = Dot(PixelDir((float)x, (float)y), g_sun);
				for (int k = 0; k < 3; k++)
				{
					float out = skyFog.composite[p * 4 + k], in = skyFog.scene[p * 4 + k], old = sun.composite[p * 4 + k];
					if (out > old * 1.0001f + 1e-4f)
						brighter++; // only ever darker than (or as bright as) without SkyFog
					if (towardsSun > 0.99939f)
					{
						nearPx++;
						if (out < in * 0.9999f)
							nearDimmed++;
					}
				}
				// Away from the sun (past 8 degrees), on flat sky: scene x T + light, from the nearest fog-res pixel.
				if (towardsSun < 0.99f && x % 5 == 0 && y % 5 == 0)
				{
					size_t li = ((size_t)(y / DIV) * LW + x / DIV) * 4;
					float expect = skyFog.scene[p * 4 + 1] * skyFog.fog[li + 3] + skyFog.fog[li + 1];
					checked++;
					if (fabsf(skyFog.composite[p * 4 + 1] - expect) > 0.03f)
						bad++;
				}
			}
		Check(nearPx > 0 && nearDimmed == 0, "sky fog: the sun and the sky within 2 degrees keep their full brightness", nearDimmed, nearPx);
		Check(brighter == 0, "sky fog: the sky only ever loses light to it", brighter, 0);
		Check(checked > 0 && bad < checked / 20 + 1, "sky fog: sky away from the sun = scene x transmittance + scattered light", bad, checked);
	}

	// ---- Street view again with the cascades stretched as the shipped ini does (distance and splits
	// x 6): the fog reads the ranges, scales, offsets and fade from the game's constants, so it must
	// still match the ray-traced reference (the atlas is just coarser per metre).
	{
		SetCascades(6.0f, 6.0f);
		g_eye = { 0.0f, 0.0f, 1.7f };
		BuildShadowSpace();
		g_atlasTex.srv->Release();
		g_atlasTex.t->Release();
		g_atlasTex = MakeTex(4 * TILE, TILE, DXGI_FORMAT_R32_FLOAT, false, g_atlas.data(), 4 * TILE * 4);
		SetCamera(0.0f, 4.0f);
		Outputs stretched = RunView(s);
		CheckAgainstReference(stretched, s, "street, cascades x6");
		double sumSt = 0, sumUnSt = 0;
		for (size_t i = 0; i < stretched.march.size(); i += 4)
		{
			sumSt += stretched.march[i + 1];
			sumUnSt += unshadowed.march[i + 1];
		}
		Check(sumSt < 0.8 * sumUnSt, "street, cascades x6: shadows still remove a good share of the scattered sunlight", sumSt / sumUnSt, 0.8);
		SetCascades(1.0f, 1.0f);
	}

	// ---- Beams view: from the street, the low sun across the row of alleys, plugin defaults --------
	g_eye = { 0.0f, 0.0f, 1.7f };
	BuildShadowSpace();
	g_atlasTex.srv->Release();
	g_atlasTex.t->Release();
	g_atlasTex = MakeTex(4 * TILE, TILE, DXGI_FORMAT_R32_FLOAT, false, g_atlas.data(), 4 * TILE * 4);
	SetCamera(42.0f, 16.0f);
	Settings d = s;
	d.density = 0.0012f;
	Outputs beams = RunView(d);
	WritePngRgb("warp_beams_scene.png", beams.scene, W, H);
	WritePngRgb("warp_beams_composite.png", beams.composite, W, H);
	WritePngRgb("warp_beams_fog.png", FogRgb(beams.fog, false), LW, LH, 3.0f);
	Check(!AnyNan(beams.fog) && !AnyNan(beams.composite), "beams: no NaN / Inf");

	// ---- TAA -------------------------------------------------------------------------------------
	{
		// TAA off: the offset 0 gives exactly the old output (the street and beams views above).
		printf("TAA off, output hashes: street march %016llX fog %016llX composite %016llX; beams march %016llX fog %016llX composite %016llX\n",
			(unsigned long long)lit.hashMarch, (unsigned long long)lit.hashFog, (unsigned long long)lit.hashComposite,
			(unsigned long long)beams.hashMarch, (unsigned long long)beams.hashFog, (unsigned long long)beams.hashComposite);
		bool same = lit.hashMarch == kToday[0] && lit.hashFog == kToday[1] && lit.hashComposite == kToday[2] && beams.hashMarch == kToday[3] &&
			beams.hashFog == kToday[4] && beams.hashComposite == kToday[5];
		Check(same, "TAA off (noise offset 0): march, fog and composite bit for bit what the shaders gave before the offset");
	}
	{
		// Jitter: the march's ray through each full-res pixel, from the shader's own ViewRay / WorldDir,
		// against the direction through the jittered pixel (TAA's phase 8 offset).
		g_jitterPx[0] = -0.4375f;
		g_jitterPx[1] = 0.3888889f;
		std::string src = g_src + "\nfloat4 PS_TestRay(VPOS_T vposIn : VPOS) : COLOR\n{\n\treturn float4(WorldDir(ViewRay(PIXEL(vposIn))), 0);\n}\n";
		Pass ray = MakePass("PS_TestRay", &src);
		Tex out = MakeTex(W, H, DXGI_FORMAT_R32G32B32A32_FLOAT, true);
		double expected = 0; // what misses the jittered pixel by the offset does: the unjittered pixel's direction
		auto rayError = [&](bool fold) {
			float c[32][4] = {};
			c[0][0] = (float)W, c[0][1] = (float)H, c[0][2] = 1.0f / W, c[0][3] = 1.0f / H;
			c[2][0] = A, c[2][1] = B, c[2][2] = P00, c[2][3] = P11;
			c[3][0] = fold ? 2.0f * g_jitterPx[0] / W : 0.0f;
			c[3][1] = fold ? -2.0f * g_jitterPx[1] / H : 0.0f;
			memcpy(c[5], &g_right, 12);
			memcpy(c[6], &g_up, 12);
			memcpy(c[7], &g_fwd, 12);
			ID3D11ShaderResourceView* none[7] = {};
			Run(ray, c, out, none);
			std::vector<float> d = ReadBack32(out);
			double sum = 0, sumExpected = 0;
			int n = 0;
			for (UINT y = 0; y < H; y += 3)
				for (UINT x = 0; x < W; x += 3)
				{
					V3 truth = PixelDir(x - g_jitterPx[0], y - g_jitterPx[1]);
					const float* g = &d[((size_t)y * W + x) * 4];
					V3 e = V3{ g[0], g[1], g[2] } - truth, u = PixelDir((float)x, (float)y) - truth;
					// in pixels: radians x pixels per radian at the centre of the screen
					sum += sqrt(Dot(e, e)) * 0.5 * W * P00;
					sumExpected += sqrt(Dot(u, u)) * 0.5 * W * P00;
					n++;
				}
			expected = sumExpected / n;
			return sum / n;
		};
		double f = rayError(true), u = rayError(false);
		char what[200];
		snprintf(what, sizeof(what), "jittered by (-0.4375, 0.3889) px: march rays off the jittered pixel by (px) with the offset folded in, "
									 "without it (expected %.3f)",
			expected);
		Check(f < 0.01 && fabs(u - expected) < 0.05 * expected, what, f, u);
		g_jitterPx[0] = g_jitterPx[1] = 0;
		out.srv->Release();
		out.rtv->Release();
		out.t->Release();
	}
	{
		// Noise: the beams view (shafts through the alleys) over 16 frames with the per-frame offsets the
		// mod sets while TAA runs, against what the march converges to (64 frames, offsets evenly spaced,
		// unblurred). One frame of the shipped chain (march, blur across, blur down) against the average
		// of 16; the same with a narrower blur. (Measured: one frame of the narrower blur is about a
		// third further off, 16 averaged about a quarter closer, both around 0.1 % of the fog's level,
		// which nobody would see; so the mod keeps its one blur with and without TAA.)
		const int N = 16, R = 64;
		Settings b = d;
		Outputs first = RunView(b); // renders the scene once
		const size_t n = first.march.size();
		std::vector<float> ref(n, 0.0f), avg(n, 0.0f), avgNarrow(n, 0.0f), avgRaw(n, 0.0f);
		for (int i = 0; i < R; i++)
		{
			b.noise = (i + 0.5f) / R;
			Outputs o = RunView(b, true);
			for (size_t k = 0; k < n; k++)
				ref[k] += o.march[k] / R;
		}
		b.noise = 0;
		Outputs single = RunView(b, true);
		b.narrowBlur = true;
		Outputs singleNarrow = RunView(b, true);
		for (int f = 1; f <= N; f++)
		{
			const double g = 0.6180339887498949;
			b.noise = (float)(f * g - floor(f * g)); // core::Scene_NoiseOffset
			b.narrowBlur = false;
			Outputs o = RunView(b, true);
			b.narrowBlur = true;
			Outputs on = RunView(b, true);
			for (size_t k = 0; k < n; k++)
			{
				avg[k] += o.fog[k] / N;
				avgNarrow[k] += on.fog[k] / N;
				avgRaw[k] += o.march[k] / N;
			}
		}
		// Mean |error| of the in-scatter (rgb) and the transmittance.
		auto err = [&](const std::vector<float>& v, bool transmittance) {
			double sum = 0, level = 0;
			for (size_t k = 0; k < n; k += 4)
				for (int c = transmittance ? 3 : 0; c < (transmittance ? 4 : 3); c++)
				{
					sum += fabs(v[k + c] - ref[k + c]);
					level += ref[k + c];
				}
			return sum / level; // relative to the mean level
		};
		double e1 = err(single.fog, false), eAvg = err(avg, false), e1n = err(singleNarrow.fog, false), eAvgN = err(avgNarrow, false);
		double eRaw1 = err(single.march, false), eRaw = err(avgRaw, false);
		double t1 = err(single.fog, true), tAvg = err(avg, true);
		printf("fog in-scatter against what the march converges to (mean |error| / mean level), one frame -> %d averaged: blur %.4f -> %.4f; "
			   "narrow blur %.4f -> %.4f; unblurred %.4f -> %.4f; transmittance (blur) %.5f -> %.5f\n",
			N, e1, eAvg, e1n, eAvgN, eRaw1, eRaw, t1, tAvg);
		Check(eAvg < 0.9 * e1 && eRaw < 0.5 * eRaw1,
			"noise: the fog averaged over frames with the per-frame offsets is closer to it than one frame (blurred, unblurred)", eAvg / e1,
			eRaw / eRaw1);
	}

	auto rebuildAtlas = [&]() {
		BuildShadowSpace();
		g_atlasTex.srv->Release();
		g_atlasTex.t->Release();
		g_atlasTex = MakeTex(4 * TILE, TILE, DXGI_FORMAT_R32_FLOAT, false, g_atlas.data(), 4 * TILE * 4);
	};
	// Sums of the in-scatter (green) and the transmittance over a fog-res image.
	auto sums = [](const std::vector<float>& v, double& light, double& T) {
		light = T = 0;
		for (size_t i = 0; i < v.size(); i += 4)
		{
			light += v[i + 1];
			T += v[i + 3];
		}
	};

	// ---- Sky occlusion -----------------------------------------------------------------------------
	{
		g_eye = { 0.0f, 0.0f, 1.7f };
		rebuildAtlas();
		SetCamera(0.0f, 4.0f);
		Settings so = s;
		so.skyOcc = 0.8f;
		Outputs occ = RunView(so);
		CheckAgainstReference(occ, so, "street, sky occlusion 0.8");
		WritePngRgb("warp_street_fog_skyocclusion.png", FogRgb(occ.fog, false), LW, LH, 2.0f);
		// Sky light only: occlusion takes it away, never adds it; the transmittance stays.
		Settings amb = s;
		amb.sun = 0;
		Outputs a0 = RunView(amb, true);
		amb.skyOcc = 0.8f;
		Outputs a1 = RunView(amb, true);
		int above = 0, tDiff = 0;
		for (size_t i = 0; i < a0.march.size(); i += 4)
		{
			above += a1.march[i + 1] > a0.march[i + 1] * 1.0001f + 1e-7f;
			tDiff += a1.march[i + 3] != a0.march[i + 3];
		}
		double l0, t0, l1, t1;
		sums(a0.march, l0, t0);
		sums(a1.march, l1, t1);
		Check(above == 0 && tDiff == 0, "street, sky light only: occlusion never adds light, transmittance bit for bit the same", above, tDiff);
		Check(l1 < 0.97 * l0 && l1 > 0.3 * l0, "street, sky light only: the shaded street loses some of its sky light, not all", l1 / l0, 0);

		// Strength 0 / no shadow map: bit for bit the fog without the occlusion.
		Settings z = s;
		z.skyOcc = 0;
		Outputs base = RunView(z, true);
		Check(base.hashMarch == lit.hashMarch && base.hashComposite == lit.hashComposite, "sky occlusion 0: bit for bit the fog before it");
		Settings ns0 = s, ns1 = s;
		ns0.shadows = ns1.shadows = false;
		ns1.skyOcc = 1.0f;
		Outputs u0 = RunView(ns0, true), u1 = RunView(ns1, true);
		Check(u0.hashMarch == u1.hashMarch && u0.hashComposite == u1.hashComposite, "no shadow map: sky occlusion changes nothing");

		// A low roof over everything (a hall or a tomb): the cover's top is 5.5 m up, so at eye height
		// it is 3.8 m over the air: open = 3.8 / 20, sky light x (1 - 0.8 x 0.81) = 0.35 near the
		// camera, a little more along the rays going up to the roof.
		std::vector<Box> town = g_boxes;
		g_boxes.assign(1, Box{ { -400.0f, -400.0f, 5.0f }, { 400.0f, 400.0f, 5.5f } });
		rebuildAtlas();
		Settings h = s;
		h.sun = 0;
		Outputs h0 = RunView(h);
		h.skyOcc = 0.8f;
		Outputs h1 = RunView(h, true);
		CheckAgainstReference(h1, h, "under a roof, sky light only");
		sums(h0.march, l0, t0);
		sums(h1.march, l1, t1);
		Check(l1 > 0.3 * l0 && l1 < 0.5 * l0, "under a roof: the sky light drops to what the cover height says (about 0.35-0.45)", l1 / l0, 0);
		WritePngRgb("warp_roof_fog.png", FogRgb(h0.fog, false), LW, LH, 8.0f);
		WritePngRgb("warp_roof_fog_skyocclusion.png", FogRgb(h1.fog, false), LW, LH, 8.0f);
		// The sun low (4 degrees): the occlusion is faded out, the fog as without it.
		const V3 sunWas = g_sun;
		g_sun = Norm(V3{ cosf(az) * cosf(0.07f), sinf(az) * cosf(0.07f), sinf(0.07f) });
		rebuildAtlas();
		h.skyOcc = 0;
		Outputs lowA = RunView(h, true);
		h.skyOcc = 0.8f;
		Outputs lowB = RunView(h, true);
		Check(lowA.hashMarch == lowB.hashMarch, "sun at 4 degrees: sky occlusion faded out (bit for bit without it)");
		g_sun = sunWas;
		// An open plain: everything sunlit, nothing changes.
		g_boxes.clear();
		rebuildAtlas();
		h.sun = s.sun;
		h.skyOcc = 0;
		Outputs p0 = RunView(h);
		h.skyOcc = 1.0f;
		Outputs p1 = RunView(h, true);
		Check(p0.hashMarch == p1.hashMarch && p0.hashComposite == p1.hashComposite, "open plain: sky occlusion changes nothing (bit for bit)");
		g_boxes = town;
		rebuildAtlas();
	}

	// ---- Density noise ------------------------------------------------------------------------------
	{
		// The volume: 7 levels (64^3 .. 1^3), mean 0.5, tiles (the step across the wrap is like any other).
		bool levelsOk = noiseLevels.size() == 7;
		for (size_t l = 0; l < noiseLevels.size(); l++)
			levelsOk &= noiseLevels[l].size() == (size_t)(64 >> l) * (64 >> l) * (64 >> l);
		const std::vector<uint8_t>& v0 = noiseLevels[0];
		double mean = 0, sd = 0, stepIn = 0, stepWrap = 0;
		for (uint8_t b : v0)
			mean += b / 255.0;
		mean /= v0.size();
		for (uint8_t b : v0)
			sd += (b / 255.0 - mean) * (b / 255.0 - mean);
		sd = sqrt(sd / v0.size());
		for (int z = 0; z < 64; z++)
			for (int y = 0; y < 64; y++)
			{
				const uint8_t* row = &v0[((size_t)z * 64 + y) * 64];
				stepWrap += fabs(row[0] - row[63]) / 255.0;
				for (int x = 0; x < 63; x++)
					stepIn += fabs(row[x + 1] - row[x]) / 255.0 / 63.0;
			}
		Check(levelsOk && noiseLevels.back().size() == 1, "noise volume: 64^3 with a full mip chain down to 1^3");
		Check(fabs(mean - 0.5) < 0.01 && sd > 0.12 && sd < 0.2, "noise volume: mean 0.5, standard deviation about 1/6", mean, sd);
		Check(stepWrap < 1.3 * stepIn && stepWrap > 0.7 * stepIn, "noise volume: tiles (the step across the wrap like the others)", stepWrap, stepIn);
		Check(fabs(noiseLevels.back()[0] / 255.0 - 0.5) < 0.02, "noise volume: the last mip is the mean", noiseLevels.back()[0] / 255.0, 0.5);

		g_eye = { 0.0f, 0.0f, 1.7f };
		SetCamera(0.0f, 4.0f);
		Settings n = s;
		Outputs smooth = RunView(n);
		// Amount 0: bit for bit the fog without noise, whatever the other noise settings.
		n.noiseSize = 7.0f;
		n.time = 1234.5;
		Outputs zero = RunView(n, true);
		Check(zero.hashMarch == lit.hashMarch && zero.hashFog == lit.hashFog && zero.hashComposite == lit.hashComposite,
			"noise amount 0: bit for bit the fog without noise");
		// With noise: the same fog on average, but varying.
		n = s;
		n.noiseAmount = 0.5f;
		n.time = 100.0;
		Outputs wisp = RunView(n, true);
		WritePngRgb("warp_street_fog_noise.png", FogRgb(wisp.fog, false), LW, LH, 2.0f);
		WritePngRgb("warp_street_composite_noise.png", wisp.composite, W, H);
		Check(!AnyNan(wisp.march) && !AnyNan(wisp.fog) && !AnyNan(wisp.composite), "noise: no NaN / Inf");
		double ls, ts, lw, tw;
		sums(smooth.fog, ls, ts);
		sums(wisp.fog, lw, tw);
		double diff = 0;
		for (size_t i = 0; i < smooth.fog.size(); i += 4)
			diff += fabs(wisp.fog[i + 1] - smooth.fog[i + 1]);
		Check(fabs(lw / ls - 1) < 0.1 && fabs(tw / ts - 1) < 0.05, "noise 0.5: the fog's average in-scatter and transmittance stay", lw / ls, tw / ts);
		Check(diff / ls > 0.03, "noise 0.5: the fog varies (mean |change| / level)", diff / ls, 0.03);
		// Drift: one frame (60 fps) changes little, 20 s much more.
		auto change = [&](const Outputs& a, const Outputs& b) {
			double d = 0, level = 0;
			for (size_t i = 0; i < a.fog.size(); i += 4)
			{
				d += fabs(a.fog[i + 1] - b.fog[i + 1]);
				level += a.fog[i + 1];
			}
			return d / level;
		};
		n.time = 100.0 + 1.0 / 60.0;
		Outputs nextFrame = RunView(n, true);
		n.time = 120.0;
		Outputs later = RunView(n, true);
		const double cFrame = change(wisp, nextFrame), cLater = change(wisp, later);
		Check(cFrame < 0.01 && cLater > 10 * cFrame, "noise drift: a frame's worth changes the fog little, 20 s much more", cFrame, cLater);
		// After 10 days on the clock the noise still drifts smoothly (time and position wrapped in double).
		n.time = 864000.0;
		Outputs days = RunView(n, true);
		n.time = 864000.0 + 1.0 / 60.0;
		Outputs daysNext = RunView(n, true);
		Check(change(days, daysNext) < 0.01 && !AnyNan(days.fog), "noise drift after 10 days: still smooth", change(days, daysNext), 0.01);
		// Mip level by step length: one frame's march (unblurred, one jitter pattern) against what it
		// converges to over 8 step offsets. Noise detail finer than the steps (here 1 m wisps against
		// steps up to 15 m) turns into per-pixel error that changes as the noise drifts (shimmer); the
		// mip level filters it away. Sky light only, no shadows, so the noise is what varies.
		auto estimateError = [&](Settings e) {
			e.noise = 0;
			Outputs one = RunView(e, true);
			std::vector<double> conv(one.march.size(), 0.0);
			const int R = 8;
			for (int i = 0; i < R; i++)
			{
				e.noise = (i + 0.5f) / R;
				Outputs o = RunView(e, true);
				for (size_t k = 0; k < conv.size(); k++)
					conv[k] += o.march[k] / R;
			}
			double err = 0, level = 0;
			for (size_t k = 0; k < conv.size(); k += 4)
			{
				err += fabs(one.march[k + 1] - conv[k + 1]);
				level += conv[k + 1];
			}
			return err / level;
		};
		n.time = 100.0;
		n.sun = 0;
		n.shadows = false;
		n.noiseAmount = 0;
		const double eFlat = estimateError(n);
		n.noiseAmount = 1.0f;
		n.noiseSize = 4.0f;
		const double eMip = estimateError(n);
		n.noiseLodBias = -20.0f;
		const double eFine = estimateError(n);
		printf("one frame's march against its converged value (mean |error| / level): no noise %.4f, noise with mips %.4f, finest level "
			   "only %.4f\n",
			eFlat, eMip, eFine);
		Check(eMip < 0.75 * eFine, "noise: the mip level by step length filters detail the steps can't resolve", eMip, eFine);
	}

	printf("%s: %d failure(s)\n", g_fails ? "FAILED" : "ALL PASSED", g_fails);
	return g_fails ? 1 : 0;
}
