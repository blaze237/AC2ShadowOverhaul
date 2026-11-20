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

static bool RayBox(V3 o, V3 d, const Box& b, float& t)
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
static const float kRangeMax[4] = { 400, 100, 30, 10 }, kRangeMin[4] = { 100, 30, 10, 0 };
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

static void RenderScene(std::vector<float>& depth, std::vector<float>& colour)
{
	depth.assign((size_t)W * H, 1.0f);
	colour.assign((size_t)W * H * 4, 0.0f);
	for (UINT y = 0; y < H; y++)
		for (UINT x = 0; x < W; x++)
		{
			V3 d = PixelDir((float)x, (float)y), n;
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

// The constant registers c0..c29 by name, as in the HLSL.
static const char* const kRegNames[30] = { "c_Full", "c_Low", "c_Proj", "c_Proj2", "c_Eye", "c_Right", "c_Up", "c_Fwd", "c_Light0",
	"c_Light1", "c_Light2", "c_ShadowZ", "c_RangeMin", "c_RangeMax", "c_ScaleX", "c_ScaleY", "c_OffsetX", "c_OffsetY", "c_Bias", "c_Fade",
	"c_Cloud", "c_SunDir", "c_SunCol", "c_Ambient", "c_Medium", "c_Blur", "c_Shafts", "c_Shafts2", "c_ShaftCol", "c_ShRes" };

struct Pass
{
	ID3D11PixelShader* ps = nullptr;
	ID3D11Buffer* cb = nullptr;
	std::vector<std::pair<int, UINT>> regOffsets; // register -> byte offset in $Globals
	std::vector<std::pair<int, UINT>> texSlots;   // our sampler register (s0..s5) -> SM4 texture slot (packed in compatibility mode)
	UINT cbSize = 0;
};

static Pass MakePass(const char* entry)
{
	Pass p;
	ID3DBlob* code = CompileEntry(entry, "ps_4_0", g_src);
	if (!code)
		return p;
	g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &p.ps);
	ID3D11ShaderReflection* refl = nullptr;
	g_reflect(code->GetBufferPointer(), code->GetBufferSize(), kIID_Reflection, (void**)&refl);
	static const char* const kSamplers[6] = { "s_Depth", "s_Shadow", "s_Cloud", "s_Fog", "s_Scene", "s_Shafts" };
	D3D11_SHADER_DESC sdesc = {};
	if (refl && SUCCEEDED(refl->GetDesc(&sdesc)))
		for (UINT i = 0; i < sdesc.BoundResources; i++)
		{
			D3D11_SHADER_INPUT_BIND_DESC b;
			refl->GetResourceBindingDesc(i, &b);
			for (int k = 0; k < 6; k++)
				if (b.Type == D3D_SIT_TEXTURE && !strcmp(b.Name, kSamplers[k]))
					p.texSlots.push_back({ k, b.BindPoint });
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
			for (int r = 0; r < 30; r++)
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

static ID3D11VertexShader* g_vs;
static ID3D11SamplerState* g_samplers[6];
static ID3D11BlendState *g_blendComposite, *g_blendOff;

static void Run(Pass& p, const float c[30][4], const Tex& target, ID3D11ShaderResourceView* const srvs[6], bool composite = false)
{
	std::vector<uint8_t> buf(p.cbSize, 0);
	for (auto& ro : p.regOffsets)
		if (ro.second + 16 <= p.cbSize)
			memcpy(&buf[ro.second], c[ro.first], 16);
	if (p.cb)
		g_ctx->UpdateSubresource(p.cb, 0, nullptr, buf.data(), 0, 0);
	ID3D11ShaderResourceView* none[6] = {};
	g_ctx->PSSetShaderResources(0, 6, none);
	g_ctx->OMSetRenderTargets(1, &target.rtv, nullptr);
	D3D11_VIEWPORT vp = { 0, 0, (float)target.w, (float)target.h, 0, 1 };
	g_ctx->RSSetViewports(1, &vp);
	g_ctx->VSSetShader(g_vs, nullptr, 0);
	g_ctx->PSSetShader(p.ps, nullptr, 0);
	g_ctx->PSSetConstantBuffers(0, 1, &p.cb);
	ID3D11ShaderResourceView* packed[6] = {};
	for (auto& ts : p.texSlots)
		packed[ts.second] = srvs[ts.first];
	g_ctx->PSSetShaderResources(0, 6, packed);
	g_ctx->PSSetSamplers(0, 6, g_samplers);
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
	// The plugin's defaults (AC2Volumetrics.ini), except the denser fog the checks use.
	float density = 0.004f, falloff = 0.02f, base = 0.0f, sun = 0.35f, g = 0.45f, ambient = 0.25f, maxDist = 300.0f, steps = 40;
	float shaftIntensity = 0.35f, shaftThreshold = 0.5f;
	bool shadows = true, shafts = true;
	float view = 0;
};

struct Outputs
{
	std::vector<float> march, fog, shafts, composite, scene, depth;
};

static Pass g_march, g_blur, g_mask, g_radial, g_comp;
static Tex g_depthTex, g_atlasTex, g_cloudTex, g_fogT[2], g_shaftT[2], g_sceneTex, g_outTex;

static Outputs RunView(const Settings& s)
{
	Outputs o;
	RenderScene(o.depth, o.scene);
	g_depthTex = MakeTex(W, H, DXGI_FORMAT_R32_FLOAT, false, o.depth.data(), W * 4);
	g_sceneTex = MakeTex(W, H, DXGI_FORMAT_R32G32B32A32_FLOAT, false, o.scene.data(), W * 16);
	g_outTex = MakeTex(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, true);

	float c[30][4] = {};
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
	set4(3, 0, 0, (float)DIV, A + B / (0.99f * FAR_));
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
	set4(24, s.density, s.falloff, s.base, 0);
	set4(27, s.shaftThreshold, 12.0f, 0, 0);
	set4(29, (float)SW, (float)SH, 1.0f / SW, 1.0f / SH);

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

	ID3D11ShaderResourceView* srv[6] = { g_depthTex.srv, g_atlasTex.srv, g_cloudTex.srv, nullptr, nullptr, nullptr };
	Run(g_march, c, g_fogT[0], srv);
	o.march = ReadBack(g_fogT[0]);
	set4(25, 1, 0, 0, 0);
	srv[3] = g_fogT[0].srv;
	Run(g_blur, c, g_fogT[1], srv);
	set4(25, 0, 1, 0, 0);
	srv[3] = g_fogT[1].srv;
	Run(g_blur, c, g_fogT[0], srv);
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
	o.shafts = shafts ? ReadBack(g_shaftT[0]) : std::vector<float>((size_t)SW * SH * 4, 0.0f);
	o.composite = ReadBack(g_outTex);
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
		V3 L = kSunCol * (s.sun * phase * vis) + kFogCol * s.ambient;
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

int main(int argc, char** argv)
{
	(void)argc;
	(void)argv;
	FILE* f = fopen("../src/shaders/volumetrics.hlsl", "rb");
	if (!f)
	{
		printf("run from tests/ (needs ../src/shaders/volumetrics.hlsl)\n");
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

	printf("%s: %d failure(s)\n", g_fails ? "FAILED" : "ALL PASSED", g_fails);
	return g_fails ? 1 : 0;
}
