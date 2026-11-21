// Runs the real AO shaders (src/shaders/ao.hlsl) on a synthetic scene with D3D11's software
// renderer (WARP), so the GTAO maths is exercised without a D3D9 GPU. The HLSL is compiled for
// shader model 4 in backwards-compatibility mode with AO_D3D11 defined (pixel centres differ).
//
// The AO texture holds the unclamped estimate (open surfaces read about 1, sometimes a little more;
// the composite clamps).
//
// Scene (view space, metres, camera at the origin looking down +z, 90 degree horizontal fov, 16:9):
// floor y = -1.6, back wall z = 10, left wall x = -3, a box on the floor and a sphere by the left
// wall; above the walls is sky. The sun is up and behind the box. Checks: open floor / open wall stay
// unoccluded, the creases and the floor at the box's foot darken, the box casts a contact shadow
// onto the floor in front of it (and nothing else does), sky is untouched, and the composite
// removes exactly (1 - AO) x indirect + (1 - contact) x sun. Writes ao_warp_ao.png,
// ao_warp_contact.png and ao_warp_composite.png to look at.
//
// TAA (default scene only):
//  - TAA off: GTAO, blur and composite with the noise offsets at 0 give exactly the output of the
//    shaders before they took offsets (hashes of every target, recorded from those shaders);
//  - jitter: the scene rendered with one of TAA's sub-pixel offsets; the view positions the
//    geometry prep and GTAO reconstruct (their own ViewPos, through a test entry point appended to
//    the real source) match the ray-cast truth with the offset folded into P02 / P12, and are off
//    by the offset without it; the prep's normals on the floor and the left wall lose their tilt;
//  - noise: AO and contact shadows averaged over 16 frames with the per-frame offsets the mod sets
//    while TAA runs (core::Scene_NoiseOffsets) are closer to what GTAO converges to than one frame;
//    and the narrow blur used then is as good as the wide one on a single frame and better averaged.
//
// ao_warp_test pitched            - a second scene: camera looking down onto open ground with a box, a
//                                   10 cm post and a sunlit wall seen at a glancing angle (false
//                                   self-shadowing, halos behind thin objects)
// ao_warp_test replay <f32> <txt> - runs the shaders on a real frame from an F8 dump: the z/w target
//                                   (<dump>_T*.f32) and the AO constants line of the dump's .txt;
//                                   writes the AO and contact shadow pictures only
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

static UINT W = 1920, H = 1080, HW = W / 2, HH = H / 2; // pixel sizes close to the game's at 4K matter for the biases
static bool g_replay = false;
static const float P00 = 1.0f, P11 = 16.0f / 9.0f, NEAR_ = 0.1f, FAR_ = 1000.0f;
static const float A = FAR_ / (FAR_ - NEAR_), B = -NEAR_ * FAR_ / (FAR_ - NEAR_);

typedef HRESULT(WINAPI* PFN_Compile)(const void*, SIZE_T, const char*, const D3D_SHADER_MACRO*, ID3DInclude*, const char*, const char*, UINT, UINT,
	ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI* PFN_Reflect)(const void*, SIZE_T, REFIID, void**);
static PFN_Compile g_compile;
static PFN_Reflect g_reflect;
static std::string g_src, g_geoSrc;

static const GUID kIID_Reflection = { 0x8d536ca1, 0x0cca, 0x4956, { 0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84 } };

// From ao.hlsl, or (geo) from the shared geometry prep (src/core/shaders/geoprep.hlsl).
static ID3DBlob* CompileEntry(const char* entry, const char* profile, bool geo = false)
{
	const D3D_SHADER_MACRO defs[] = { { "AO_D3D11", "1" }, { "GEO_D3D11", "1" }, { nullptr, nullptr } };
	ID3DBlob *code = nullptr, *err = nullptr;
	const std::string& src = geo ? g_geoSrc : g_src;
	HRESULT hr = g_compile(src.data(), src.size(), geo ? "geoprep.hlsl" : "ao.hlsl", defs, nullptr, entry, profile,
		D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
	if (FAILED(hr))
		printf("%s: compile failed: %s\n", entry, err ? (const char*)err->GetBufferPointer() : "?");
	return code;
}

// Any source text (the real HLSL with test entry points appended or its sample counts raised).
static ID3DBlob* CompileText(const std::string& src, const char* name, const char* entry, const char* profile)
{
	const D3D_SHADER_MACRO defs[] = { { "AO_D3D11", "1" }, { "GEO_D3D11", "1" }, { nullptr, nullptr } };
	ID3DBlob *code = nullptr, *err = nullptr;
	HRESULT hr = g_compile(src.data(), src.size(), name, defs, nullptr, entry, profile,
		D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
	if (FAILED(hr))
		printf("%s: compile failed: %s\n", entry, err ? (const char*)err->GetBufferPointer() : "?");
	return code;
}

// Replaces every `from` in `src` with `to`.
static std::string Replace(std::string src, const char* from, const char* to)
{
	for (size_t at = src.find(from); at != std::string::npos; at = src.find(from, at + strlen(to)))
		src.replace(at, strlen(from), to);
	return src;
}

// Constant buffer contents for a shader, filled by variable name ($Globals, offsets from reflection).
struct Constants
{
	std::vector<uint8_t> data;
	std::vector<std::pair<std::string, UINT>> vars;
	std::vector<std::pair<std::string, UINT>> textures; // sampler name -> texture slot (assigned by use in SM4)
};
static bool Reflect(ID3DBlob* code, Constants& c)
{
	ID3D11ShaderReflection* r = nullptr;
	if (FAILED(g_reflect(code->GetBufferPointer(), code->GetBufferSize(), kIID_Reflection, (void**)&r)) || !r)
		return false;
	D3D11_SHADER_DESC sdsc = {};
	r->GetDesc(&sdsc);
	for (UINT i = 0; i < sdsc.BoundResources; i++)
	{
		D3D11_SHADER_INPUT_BIND_DESC b = {};
		r->GetResourceBindingDesc(i, &b);
		if (b.Type == D3D_SIT_TEXTURE)
			c.textures.push_back({ b.Name, b.BindPoint });
	}
	ID3D11ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByName("$Globals");
	D3D11_SHADER_BUFFER_DESC bd = {};
	if (cb && SUCCEEDED(cb->GetDesc(&bd)))
	{
		c.data.assign((bd.Size + 15) & ~15u, 0);
		for (UINT i = 0; i < bd.Variables; i++)
		{
			D3D11_SHADER_VARIABLE_DESC vd = {};
			cb->GetVariableByIndex(i)->GetDesc(&vd);
			c.vars.push_back({ vd.Name, vd.StartOffset });
		}
	}
	r->Release();
	return true;
}
static void Set(Constants& c, const char* name, const float v[4])
{
	for (auto& p : c.vars)
		if (p.first == name && p.second + 16 <= c.data.size())
			memcpy(&c.data[p.second], v, 16);
}

// ---- Scene -------------------------------------------------------------------------------------

static bool RayBox(const float o[3], const float d[3], const float lo[3], const float hi[3], float& t)
{
	float t0 = 0, t1 = 1e30f;
	for (int k = 0; k < 3; k++)
	{
		float inv = 1.0f / d[k], a = (lo[k] - o[k]) * inv, b = (hi[k] - o[k]) * inv;
		if (a > b)
			std::swap(a, b);
		t0 = fmaxf(t0, a);
		t1 = fminf(t1, b);
	}
	if (t0 > t1 || t0 <= 0)
		return false;
	t = t0;
	return true;
}

// Second scene ("pitched", like a rooftop view in the game): camera 6 m above flat ground looking
// 40 degrees down, a 2 x 1.5 x 2 m box on the ground, the sun high and to the side.
static bool g_pitched = false;
static const float kPitch = 0.6981f; // 40 degrees
static const float kRight[3] = { 1, 0, 0 }, kUp[3] = { 0, cosf(kPitch), sinf(kPitch) }, kFwd[3] = { 0, -sinf(kPitch), cosf(kPitch) };
static const float kEye[3] = { 0, 6, 0 };
static const float kSunWorld[3] = { 0.433f, 0.866f, 0.25f };
static const float kBoxLo[3] = { -1, 0, 8 }, kBoxHi[3] = { 1, 1.5f, 10 };
static const float kPoleLo[3] = { 1.5f, 0, 6 }, kPoleHi[3] = { 1.6f, 1.0f, 6.1f }; // a 10 cm thick post, like a leg

static float PitchedZ(float px, float py, float* hitWorld = nullptr)
{
	float ndcX = 2.0f * px / W - 1.0f, ndcY = 1.0f - 2.0f * py / H;
	float d[3];
	for (int k = 0; k < 3; k++)
		d[k] = kRight[k] * ndcX / P00 + kUp[k] * ndcY / P11 + kFwd[k];
	float best = 1e30f, t;
	if (d[1] < 0)
	{
		t = -kEye[1] / d[1];
		if (t < 200 && kEye[0] + d[0] * t > -3.0f)
			best = t;
	}
	if (d[0] < 0)
	{
		// a sunlit wall seen at a glancing angle: x = -3, facing +x, 10 m high, z 2..40
		t = (-3.0f - kEye[0]) / d[0];
		float y = kEye[1] + d[1] * t, z = kEye[2] + d[2] * t;
		if (t < best && y >= 0 && y <= 10 && z >= 2 && z <= 40)
			best = t;
	}
	if (RayBox(kEye, d, kBoxLo, kBoxHi, t) && t < best)
		best = t;
	if (RayBox(kEye, d, kPoleLo, kPoleHi, t) && t < best)
		best = t;
	if (best > 1e29f)
		return 0;
	if (hitWorld)
		for (int k = 0; k < 3; k++)
			hitWorld[k] = kEye[k] + d[k] * best;
	return best; // d has a forward component of 1, so t is the view z
}

// Which surface of the default scene a pixel shows (SURF_*), for the jitter checks.
enum
{
	SURF_SKY,
	SURF_FLOOR,
	SURF_BACK,
	SURF_LEFT,
	SURF_BOX,
	SURF_SPHERE
};
static int g_lastSurface = SURF_SKY;

static float SceneZ(float px, float py) // view z at a pixel (fractional: any point on the image plane), or 0 for sky
{
	if (g_pitched)
		return PitchedZ(px, py);
	float ndcX = 2.0f * px / W - 1.0f, ndcY = 1.0f - 2.0f * py / H;
	float d[3] = { ndcX / P00, ndcY / P11, 1.0f }, o[3] = { 0, 0, 0 };
	float best = 1e30f, t;
	g_lastSurface = SURF_SKY;
	auto consider = [&](float tt, bool ok, int surface) {
		if (ok && tt > 0 && tt < best)
		{
			best = tt;
			g_lastSurface = surface;
		}
	};
	if (d[1] < 0)
	{
		t = -1.6f / d[1];
		consider(t, d[0] * t > -3.0f && t < 10.0f, SURF_FLOOR);
	}
	t = 10.0f;
	consider(t, d[1] * t >= -1.6f && d[1] * t <= 3.0f && d[0] * t >= -3.0f, SURF_BACK);
	if (d[0] < 0)
	{
		t = -3.0f / d[0];
		consider(t, d[1] * t >= -1.6f && d[1] * t <= 3.0f && t <= 10.0f, SURF_LEFT);
	}
	const float lo[3] = { -0.5f, -1.6f, 4.0f }, hi[3] = { 0.7f, -0.6f, 5.2f };
	if (RayBox(o, d, lo, hi, t))
		consider(t, true, SURF_BOX);
	// sphere centre (-1.9, -0.8, 7.5), r 0.8
	const float c[3] = { -1.9f, -0.8f, 7.5f }, r = 0.8f;
	float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2], dc = d[0] * c[0] + d[1] * c[1] + d[2] * c[2];
	float cc = c[0] * c[0] + c[1] * c[1] + c[2] * c[2] - r * r, disc = dc * dc - dd * cc;
	if (disc >= 0)
		consider((dc - sqrtf(disc)) / dd, true, SURF_SPHERE);
	return best < 1e29f ? best : 0.0f; // d[2] = 1, so t is the view z
}

static void Project(float x, float y, float z, int& px, int& py)
{
	px = (int)lroundf((x / z * P00 + 1.0f) * 0.5f * W);
	py = (int)lroundf((1.0f - y / z * P11) * 0.5f * H);
}

// ---- PNG (stored, uncompressed) ----------------------------------------------------------------

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
static void WritePng(const char* path, const std::vector<uint8_t>& grey, UINT w, UINT h)
{
	std::vector<uint8_t> raw;
	for (UINT y = 0; y < h; y++)
	{
		raw.push_back(0);
		raw.insert(raw.end(), grey.begin() + (size_t)y * w, grey.begin() + (size_t)(y + 1) * w);
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
		(uint8_t)(h >> 8), (uint8_t)h, 8, 0, 0, 0, 0 };
	chunk("IHDR", ihdr);
	chunk("IDAT", z);
	chunk("IEND", {});
	fclose(f);
}

// ---- D3D11 helpers -----------------------------------------------------------------------------

struct Target
{
	ID3D11Texture2D* tex = nullptr;
	ID3D11RenderTargetView* rtv = nullptr;
	ID3D11ShaderResourceView* srv = nullptr;
};
static Target MakeTarget(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, const void* init = nullptr, UINT pitch = 0)
{
	Target t;
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

static std::vector<float> ReadBack(ID3D11Device* dev, ID3D11DeviceContext* ctx, const Target& t, UINT w, UINT h, int channels)
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
	std::vector<float> out((size_t)w * h * channels);
	for (UINT y = 0; y < h; y++)
		memcpy(&out[(size_t)y * w * channels], (const uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)w * channels * 4);
	ctx->Unmap(st, 0);
	st->Release();
	return out;
}

// FNV-1a over a target's texels (its full rows): two renders hash the same only if every bit agrees.
static uint64_t HashTarget(ID3D11Device* dev, ID3D11DeviceContext* ctx, const Target& t, UINT w, UINT h, UINT bytesPerTexel)
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
	uint64_t hsh = 1469598103934665603ull;
	for (UINT y = 0; y < h; y++)
	{
		const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
		for (size_t i = 0; i < (size_t)w * bytesPerTexel; i++)
			hsh = (hsh ^ row[i]) * 1099511628211ull;
	}
	ctx->Unmap(st, 0);
	st->Release();
	return hsh;
}

// A half-float RGBA target as floats.
static std::vector<float> ReadHalf(ID3D11Device* dev, ID3D11DeviceContext* ctx, const Target& t, UINT w, UINT h)
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
	std::vector<float> out((size_t)w * h * 4);
	for (UINT y = 0; y < h; y++)
		for (UINT x = 0; x < w * 4; x++)
		{
			uint16_t hbits = ((const uint16_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch))[x];
			uint32_t sign = (hbits >> 15) & 1, e = (hbits >> 10) & 31, mant = hbits & 1023;
			float v = e == 0 ? ldexpf((float)mant, -24) : ldexpf((float)(mant | 1024), (int)e - 25);
			out[(size_t)y * w * 4 + x] = sign ? -v : v;
		}
	ctx->Unmap(st, 0);
	st->Release();
	return out;
}

// The per-frame noise offsets the mod sets while TAA runs (core/scene.cpp Scene_NoiseOffsets: R2
// over TAA's frame counter).
static void R2(unsigned frame, float out[2])
{
	const double a1 = 0.7548776662466927, a2 = 0.5698402909980532, n = (double)frame;
	out[0] = (float)(n * a1 - floor(n * a1));
	out[1] = (float)(n * a2 - floor(n * a2));
}

// Hashes of the targets with the noise offsets at 0, recorded from the shaders before the offsets
// were added (src/shaders/ao.hlsl and src/core/shaders/geoprep.hlsl of commit 7a64ec5): without TAA
// nothing may change, not one bit. (Recorded on WARP; a different WARP version could round
// differently, in which case the shaders of that commit give the new numbers.)
static const uint64_t kTodayGeo = 0x9F106A3B0750CED6ull, kTodayGtao = 0xA04D21D48D7613D5ull, kTodayAo = 0x8AD77C5CD594AE52ull,
					  kTodayComposite = 0x2471F8EB7678CC5Aull;

int main(int argc, char** argv)
{
	g_pitched = argc > 1 && strcmp(argv[1], "pitched") == 0;
	g_replay = argc > 3 && strcmp(argv[1], "replay") == 0;
	std::vector<float> replayDepth;
	float rc[8][4] = {};
	if (g_replay)
	{
		FILE* f = fopen(argv[2], "rb");
		uint32_t hdr[3] = {};
		if (!f || fread(hdr, 4, 3, f) != 3 || hdr[0] != 0x46324341u)
			return printf("cannot read %s\n", argv[2]), 1;
		W = hdr[1];
		H = hdr[2];
		HW = (W + 1) / 2;
		HH = (H + 1) / 2;
		replayDepth.resize((size_t)W * H);
		fread(replayDepth.data(), 4, replayDepth.size(), f);
		fclose(f);
		std::string txt;
		if (FILE* t = fopen(argv[3], "rb"))
		{
			char b[4096];
			size_t n;
			while ((n = fread(b, 1, sizeof(b), t)) > 0)
				txt.append(b, n);
			fclose(t);
		}
		for (int i = 0; i < 8; i++)
		{
			char key[16];
			snprintf(key, sizeof(key), "c%d = (", i);
			size_t at = txt.find(key);
			if (at == std::string::npos || sscanf(txt.c_str() + at + strlen(key), "%f, %f, %f, %f", &rc[i][0], &rc[i][1], &rc[i][2], &rc[i][3]) != 4)
				return printf("no AO constants (%s) in %s\n", key, argv[3]), 1;
		}
		printf("replay: %ux%u, sun (%.3f, %.3f, %.3f), contact length %.2f m\n", W, H, rc[6][0], rc[6][1], rc[6][2], rc[6][3]);
	}
	for (int k = 0; k < 2; k++)
	{
		FILE* f = fopen(k ? "../src/core/shaders/geoprep.hlsl" : "../src/shaders/ao.hlsl", "rb");
		if (!f)
			return printf("run from tests/\n"), 1;
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

	ID3D11Device* dev = nullptr;
	ID3D11DeviceContext* ctx = nullptr;
	D3D_FEATURE_LEVEL fl;
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx)))
		return printf("no WARP device\n"), 1;

	ID3DBlob* vsCode = CompileEntry("VS_Fullscreen", "vs_4_0");
	ID3DBlob* gtaoCode = CompileEntry("PS_GTAO", "ps_4_0");
	ID3DBlob* blurCode = CompileEntry("PS_Blur", "ps_4_0");
	ID3DBlob* compCode = CompileEntry("PS_Composite", "ps_4_0");
	ID3DBlob* geoCode = CompileEntry("PS_GeoPrep", "ps_4_0", true);
	if (!vsCode || !gtaoCode || !blurCode || !compCode || !geoCode)
		return 1;
	ID3D11VertexShader* vs = nullptr;
	ID3D11PixelShader *gtao = nullptr, *blur = nullptr, *comp = nullptr, *geoPs = nullptr;
	dev->CreatePixelShader(geoCode->GetBufferPointer(), geoCode->GetBufferSize(), nullptr, &geoPs);
	dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs);
	dev->CreatePixelShader(gtaoCode->GetBufferPointer(), gtaoCode->GetBufferSize(), nullptr, &gtao);
	dev->CreatePixelShader(blurCode->GetBufferPointer(), blurCode->GetBufferSize(), nullptr, &blur);
	dev->CreatePixelShader(compCode->GetBufferPointer(), compCode->GetBufferSize(), nullptr, &comp);
	D3D11_INPUT_ELEMENT_DESC el = { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
	ID3D11InputLayout* layout = nullptr;
	dev->CreateInputLayout(&el, 1, vsCode->GetBufferPointer(), vsCode->GetBufferSize(), &layout);
	const float tri[3][4] = { { -1, -1, 0, 1 }, { -1, 3, 0, 1 }, { 3, -1, 0, 1 } };
	D3D11_BUFFER_DESC bd = { sizeof(tri), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
	D3D11_SUBRESOURCE_DATA sd = { tri, 0, 0 };
	ID3D11Buffer* vb = nullptr;
	dev->CreateBuffer(&bd, &sd, &vb);

	// Inputs: z/w of the scene, indirect light = 0.5 everywhere, scene colour = 1.
	std::vector<float> depth((size_t)W * H), indirect((size_t)W * H * 4, 0.5f), sunLight((size_t)W * H * 4, 0.3f),
		scene((size_t)W * H * 4, 1.0f);
	for (UINT y = 0; y < H; y++)
		for (UINT x = 0; x < W; x++)
		{
			if (g_replay)
			{
				depth[(size_t)y * W + x] = replayDepth[(size_t)y * W + x];
				continue;
			}
			float z = SceneZ((float)x, (float)y);
			depth[(size_t)y * W + x] = z > 0 ? A + B / z : 1.0f;
		}
	Target tDepth = MakeTarget(dev, W, H, DXGI_FORMAT_R32_FLOAT, depth.data(), W * 4);
	Target tInd = MakeTarget(dev, W, H, DXGI_FORMAT_R32G32B32A32_FLOAT, indirect.data(), W * 16);
	Target tSun = MakeTarget(dev, W, H, DXGI_FORMAT_R32G32B32A32_FLOAT, sunLight.data(), W * 16);
	Target tScene = MakeTarget(dev, W, H, DXGI_FORMAT_R32G32B32A32_FLOAT, scene.data(), W * 16);
	Target tAo[2] = { MakeTarget(dev, HW, HH, DXGI_FORMAT_R16G16B16A16_FLOAT), MakeTarget(dev, HW, HH, DXGI_FORMAT_R16G16B16A16_FLOAT) };
	Target tGeo = MakeTarget(dev, HW, HH, DXGI_FORMAT_R16G16B16A16_FLOAT); // the shared geometry prep, as the mod makes it

	D3D11_SAMPLER_DESC smp = {};
	smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	smp.MaxLOD = D3D11_FLOAT32_MAX;
	ID3D11SamplerState* point = nullptr;
	dev->CreateSamplerState(&smp, &point);
	ID3D11SamplerState* samplers[5] = { point, point, point, point, point };
	D3D11_BLEND_DESC bl = {};
	bl.RenderTarget[0].BlendEnable = TRUE;
	bl.RenderTarget[0].SrcBlend = bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	bl.RenderTarget[0].DestBlend = bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
	bl.RenderTarget[0].BlendOp = bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_REV_SUBTRACT;
	bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
	ID3D11BlendState* subtract = nullptr;
	dev->CreateBlendState(&bl, &subtract);

	float radius = 1.5f, strength = 1.0f;
	float cFull[4] = { (float)W, (float)H, 1.0f / W, 1.0f / H }, cHalf[4] = { (float)HW, (float)HH, 1.0f / HW, 1.0f / HH };
	float cProj[4] = { A, B, P00, P11 }, cProj2[4] = { 0, 0, 0, 0 };
	float cParams[4] = { radius, 0.5f * P00 * W, 0.1f * H, 0.6f };
	// Towards the sun: up and away from the camera (behind the box), contact length 0.5 m. Contact
	// parameters as the mod sets them.
	const float sunLen = sqrtf(0.0f + 0.7f * 0.7f + 0.7f * 0.7f);
	float cSun[4] = { 0.0f, 0.7f / sunLen, 0.7f / sunLen, 0.5f };
	float cContact[4] = { 0.05f, 0.01f, 1.0f, 0.002f }; // as src/ao.cpp sets them
	if (g_replay)
	{
		float* dst[7] = { cFull, cHalf, cProj, cProj2, cParams, nullptr, cSun };
		for (int i = 0; i < 7; i++)
			if (dst[i])
				memcpy(dst[i], rc[i], 16);
		memcpy(cContact, rc[7], 16);
		strength = rc[5][0] > 0 ? rc[5][0] : 1.0f;
		radius = cParams[0];
	}
	if (g_pitched)
	{
		const float* axes[3] = { kRight, kUp, kFwd };
		for (int k = 0; k < 3; k++)
			cSun[k] = axes[k][0] * kSunWorld[0] + axes[k][1] * kSunWorld[1] + axes[k][2] * kSunWorld[2];
		float l = sqrtf(cSun[0] * cSun[0] + cSun[1] * cSun[1] + cSun[2] * cSun[2]);
		for (int k = 0; k < 3; k++)
			cSun[k] /= l;
	}
	auto pass = [&](ID3DBlob* code, ID3D11PixelShader* ps, const Target& out, UINT w, UINT h, ID3D11ShaderResourceView* const* srvs,
					const float params2[4], bool blend) {
		Constants c;
		Reflect(code, c);
		Set(c, "c_Full", cFull);
		Set(c, "c_Half", cHalf);
		Set(c, "c_Proj", cProj);
		Set(c, "c_Proj2", cProj2);
		Set(c, "c_Params", cParams);
		Set(c, "c_Params2", params2);
		Set(c, "c_Sun", cSun);
		Set(c, "c_Contact", cContact);
		ID3D11Buffer* cb = nullptr;
		if (!c.data.empty())
		{
			D3D11_BUFFER_DESC cbd = { (UINT)c.data.size(), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
			D3D11_SUBRESOURCE_DATA cd = { c.data.data(), 0, 0 };
			dev->CreateBuffer(&cbd, &cd, &cb);
		}
		ID3D11ShaderResourceView* none[5] = {};
		ctx->PSSetShaderResources(0, 5, none);
		ctx->OMSetRenderTargets(1, &out.rtv, nullptr);
		static const char* const names[5] = { "s_Depth", "s_AO", "s_Indirect", "s_Sun", "s_Geo" };
		for (auto& t : c.textures)
			for (int k = 0; k < 5; k++)
				if (t.first == names[k] && t.second < 5)
					ctx->PSSetShaderResources(t.second, 1, &srvs[k]);
		D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
		ctx->RSSetViewports(1, &vp);
		UINT stride = 16, offset = 0;
		ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
		ctx->IASetInputLayout(layout);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ctx->VSSetShader(vs, nullptr, 0);
		ctx->PSSetShader(ps, nullptr, 0);
		ctx->PSSetConstantBuffers(0, 1, &cb);
		ctx->PSSetSamplers(0, 5, samplers);
		float zero[4] = {};
		ctx->OMSetBlendState(blend ? subtract : nullptr, zero, 0xFFFFFFFF);
		ctx->Draw(3, 0);
		if (cb)
			cb->Release();
	};
	const float across[4] = { strength, 0, 1, 0 }, down[4] = { strength, 0, 0, 1 };
	ID3D11ShaderResourceView* sGeo[5] = { tDepth.srv, nullptr, nullptr, nullptr, nullptr };
	pass(geoCode, geoPs, tGeo, HW, HH, sGeo, across, false);
	ID3D11ShaderResourceView* s0[5] = { tDepth.srv, nullptr, nullptr, nullptr, tGeo.srv };
	pass(gtaoCode, gtao, tAo[0], HW, HH, s0, across, false);
	const uint64_t hashGeo = HashTarget(dev, ctx, tGeo, HW, HH, 8), hashGtao = HashTarget(dev, ctx, tAo[0], HW, HH, 8);
	if (getenv("AO_DEBUG_COLUMN"))
	{
		// Raw GTAO (before the blur) down one half-res column: AO and view z.
		D3D11_TEXTURE2D_DESC d;
		tAo[0].tex->GetDesc(&d);
		d.BindFlags = 0;
		d.Usage = D3D11_USAGE_STAGING;
		d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ID3D11Texture2D* st = nullptr;
		dev->CreateTexture2D(&d, nullptr, &st);
		ctx->CopyResource(st, tAo[0].tex);
		D3D11_MAPPED_SUBRESOURCE m;
		ctx->Map(st, 0, D3D11_MAP_READ, 0, &m);
		int col = atoi(getenv("AO_DEBUG_COLUMN"));
		auto half = [](uint16_t h) {
			uint32_t e = (h >> 10) & 31, mant = h & 1023;
			float v = e == 0 ? ldexpf((float)mant, -24) : ldexpf((float)(mant | 1024), (int)e - 25);
			return (h >> 15) ? -v : v;
		};
		for (UINT y = 100; y < 130; y++)
		{
			const uint16_t* row = (const uint16_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch);
			printf("  row %u: ao %.3f z %.2f\n", y, half(row[col * 2]), half(row[col * 2 + 1]));
		}
		ctx->Unmap(st, 0);
		st->Release();
	}
	ID3D11ShaderResourceView* s1[5] = { tDepth.srv, tAo[0].srv, nullptr, nullptr, nullptr };
	pass(blurCode, blur, tAo[1], HW, HH, s1, across, false);
	ID3D11ShaderResourceView* s2[5] = { tDepth.srv, tAo[1].srv, nullptr, nullptr, nullptr };
	pass(blurCode, blur, tAo[0], HW, HH, s2, down, false);
	ID3D11ShaderResourceView* s3[5] = { tDepth.srv, tAo[0].srv, tInd.srv, tSun.srv, nullptr };
	pass(compCode, comp, tScene, W, H, s3, across, true);
	const uint64_t hashAo = HashTarget(dev, ctx, tAo[0], HW, HH, 8), hashComposite = HashTarget(dev, ctx, tScene, W, H, 16);

	// The blurred AO and contact shadows (R16G16B16A16_FLOAT, read as halves).
	std::vector<float> ao((size_t)HW * HH), contact((size_t)HW * HH);
	{
		D3D11_TEXTURE2D_DESC d;
		tAo[0].tex->GetDesc(&d);
		d.BindFlags = 0;
		d.Usage = D3D11_USAGE_STAGING;
		d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ID3D11Texture2D* st = nullptr;
		dev->CreateTexture2D(&d, nullptr, &st);
		ctx->CopyResource(st, tAo[0].tex);
		D3D11_MAPPED_SUBRESOURCE m;
		ctx->Map(st, 0, D3D11_MAP_READ, 0, &m);
		for (UINT y = 0; y < HH; y++)
			for (UINT x = 0; x < HW; x++)
			{
				const uint16_t* row = (const uint16_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch);
				auto half = [](uint16_t hbits) {
					uint32_t sign = (hbits >> 15) & 1, e = (hbits >> 10) & 31, mant = hbits & 1023;
					float v = e == 0 ? ldexpf((float)mant, -24) : ldexpf((float)(mant | 1024), (int)e - 25);
					return sign ? -v : v;
				};
				ao[(size_t)y * HW + x] = half(row[x * 4]);
				contact[(size_t)y * HW + x] = half(row[x * 4 + 2]);
			}
		ctx->Unmap(st, 0);
		st->Release();
	}
	std::vector<float> result = ReadBack(dev, ctx, tScene, W, H, 4);

	std::vector<uint8_t> grey(ao.size()), greyCs(ao.size()), comp8((size_t)W * H);
	for (size_t i = 0; i < ao.size(); i++)
	{
		grey[i] = (uint8_t)lroundf(fminf(fmaxf(ao[i], 0), 1) * 255);
		greyCs[i] = (uint8_t)lroundf(fminf(fmaxf(contact[i], 0), 1) * 255);
	}
	for (size_t i = 0; i < comp8.size(); i++)
		comp8[i] = (uint8_t)lroundf(fminf(fmaxf(result[i * 4], 0), 1) * 255);
	WritePng("ao_warp_ao.png", grey, HW, HH);
	WritePng("ao_warp_contact.png", greyCs, HW, HH);
	WritePng("ao_warp_composite.png", comp8, W, H);

	auto at = [&](const std::vector<float>& img, float x, float y, float z) {
		int px, py;
		Project(x, y, z, px, py);
		// average a 3x3 block of half-res texels around the point
		float s = 0;
		int c = 0;
		for (int dy = -1; dy <= 1; dy++)
			for (int dx = -1; dx <= 1; dx++)
			{
				int ax = px / 2 + dx, ay = py / 2 + dy;
				if (ax >= 0 && ay >= 0 && ax < (int)HW && ay < (int)HH)
				{
					s += img[(size_t)ay * HW + ax];
					c++;
				}
			}
		return c ? s / c : -1.0f;
	};
	auto aoAt = [&](float x, float y, float z) { return at(ao, x, y, z); };
	auto csAt = [&](float x, float y, float z) { return at(contact, x, y, z); };
	int fails = 0;
	auto check = [&](const char* what, float v, float lo, float hi) {
		bool ok = v >= lo && v <= hi;
		fails += !ok;
		printf("  %-44s %.3f (expected %.2f..%.2f): %s\n", what, v, lo, hi, ok ? "yes" : "NO");
	};
	if (g_replay)
	{
		printf("replay: wrote tests/ao_warp_ao.png (AO, half res) and tests/ao_warp_contact.png (contact shadows, half res)\n");
		return 0;
	}
	if (g_pitched)
	{
		// Open ground (more than 2.5 m from the box, sun side or not) must not be contact-shadowed.
		int open = 0, shadowed = 0, wall = 0, wallShadowed = 0;
		for (UINT y = 0; y < HH; y++)
			for (UINT x = 0; x < HW; x++)
			{
				float w[3];
				if (PitchedZ(x * 2.0f, y * 2.0f, w) <= 0)
					continue;
				if (w[0] < -2.99f)
				{
					if (w[1] > 1.0f) // the wall, away from its foot
					{
						wall++;
						wallShadowed += contact[(size_t)y * HW + x] < 0.9f;
					}
					continue;
				}
				if (w[1] > 0.01f)
					continue;
				float dx = fmaxf(fmaxf(kBoxLo[0] - w[0], w[0] - kBoxHi[0]), 0), dz = fmaxf(fmaxf(kBoxLo[2] - w[2], w[2] - kBoxHi[2]), 0);
				float px_ = fmaxf(fmaxf(kPoleLo[0] - w[0], w[0] - kPoleHi[0]), 0), pz = fmaxf(fmaxf(kPoleLo[2] - w[2], w[2] - kPoleHi[2]), 0);
				if (sqrtf(dx * dx + dz * dz) < 2.5f || sqrtf(px_ * px_ + pz * pz) < 1.5f)
					continue;
				open++;
				shadowed += contact[(size_t)y * HW + x] < 0.9f;
			}
		float frac = open ? (float)shadowed / open : 1.0f;
		// Contact shadow at a ground point (world space), from the half-res result.
		auto groundContact = [&](float x, float zw) {
			const float* axes[3] = { kRight, kUp, kFwd };
			float wp[3] = { x, 0, zw }, v[3];
			for (int k = 0; k < 3; k++)
				v[k] = axes[k][0] * (wp[0] - kEye[0]) + axes[k][1] * (wp[1] - kEye[1]) + axes[k][2] * (wp[2] - kEye[2]);
			int px, py;
			Project(v[0], v[1], v[2], px, py);
			return contact[(size_t)(py / 2) * HW + px / 2];
		};
		// The sun is towards +x +z: shadows fall towards -x -z (the camera side, which is visible).
		float shadowSide = groundContact(0.0f, 7.95f);       // at the foot of the box's front face
		float poleShadow = groundContact(1.45f, 5.95f);      // at the foot of the post, sun-away side
		float behindPole = fmaxf(groundContact(1.55f, 6.35f), groundContact(1.55f, 6.6f)); // beyond the post: lit
		float behindPoleMin = fminf(groundContact(1.55f, 6.35f), groundContact(1.55f, 6.6f));
		printf("  box shadow at its foot %.2f (expected < 0.8), post shadow at its foot %.2f (expected < 0.7), ground behind the post "
			   "%.2f (expected > 0.9; a halo here is the leg artefact)\n",
			shadowSide, poleShadow, behindPoleMin);
		(void)behindPole;
		float wallFrac = wall ? (float)wallShadowed / wall : 1.0f;
		bool ok = frac < 0.01f && wallFrac < 0.01f && shadowSide < 0.8f && poleShadow < 0.7f && behindPoleMin > 0.9f;
		printf("ao (WARP, pitched camera): open ground contact-shadowed %.1f%%, glancing wall %.1f%% (expected < 1%% each): %s "
			   "(picture: tests/ao_warp_contact.png)\n",
			100.0f * frac, 100.0f * wallFrac, ok ? "yes" : "NO");
		return ok ? 0 : 3;
	}
	// The shared geometry prep: every texel against the same rule on the CPU (from the same z/w
	// values), and at the probe points against the scene's real normals.
	{
		std::vector<float> geoHalf(HW * HH * 4);
		{
			D3D11_TEXTURE2D_DESC d;
			tGeo.tex->GetDesc(&d);
			d.BindFlags = 0;
			d.Usage = D3D11_USAGE_STAGING;
			d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			ID3D11Texture2D* st = nullptr;
			dev->CreateTexture2D(&d, nullptr, &st);
			ctx->CopyResource(st, tGeo.tex);
			D3D11_MAPPED_SUBRESOURCE m;
			ctx->Map(st, 0, D3D11_MAP_READ, 0, &m);
			auto half = [](uint16_t hbits) {
				uint32_t sign = (hbits >> 15) & 1, e = (hbits >> 10) & 31, mant = hbits & 1023;
				float v = e == 0 ? ldexpf((float)mant, -24) : ldexpf((float)(mant | 1024), (int)e - 25);
				return sign ? -v : v;
			};
			for (UINT y = 0; y < HH; y++)
				for (UINT x = 0; x < HW * 4; x++)
					geoHalf[(size_t)y * HW * 4 + x] = half(((const uint16_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch))[x]);
			ctx->Unmap(st, 0);
			st->Release();
		}
		auto zAt = [&](int x, int y) {
			x = x < 0 ? 0 : x >= (int)W ? W - 1 : x;
			y = y < 0 ? 0 : y >= (int)H ? H - 1 : y;
			return B / (depth[(size_t)y * W + x] - A);
		};
		auto viewPos = [&](int x, int y, float z, float p[3]) {
			p[0] = (x * 2.0f / W - 1.0f) / P00 * z;
			p[1] = (1.0f - y * 2.0f / H) / P11 * z;
			p[2] = z;
		};
		int texels = 0, zBad = 0, nBad = 0;
		for (UINT j = 0; j < HH; j++)
			for (UINT i = 0; i < HW; i++)
			{
				const float* g = &geoHalf[((size_t)j * HW + i) * 4];
				int x = 2 * i, y = 2 * j;
				if (depth[(size_t)y * W + x] >= 1.0f)
				{
					zBad += g[3] != 0.0f;
					continue;
				}
				texels++;
				float z = zAt(x, y), P[3], l[3], r[3], u[3], dn[3], dx[3], dy[3];
				viewPos(x, y, z, P);
				viewPos(x - 1, y, zAt(x - 1, y), l);
				viewPos(x + 1, y, zAt(x + 1, y), r);
				viewPos(x, y - 1, zAt(x, y - 1), u);
				viewPos(x, y + 1, zAt(x, y + 1), dn);
				bool right = fabsf(r[2] - P[2]) < fabsf(P[2] - l[2]), down_ = fabsf(dn[2] - P[2]) < fabsf(P[2] - u[2]);
				for (int k = 0; k < 3; k++)
				{
					dx[k] = right ? r[k] - P[k] : P[k] - l[k];
					dy[k] = down_ ? dn[k] - P[k] : P[k] - u[k];
				}
				float N[3] = { dx[1] * dy[2] - dx[2] * dy[1], dx[2] * dy[0] - dx[0] * dy[2], dx[0] * dy[1] - dx[1] * dy[0] };
				float len = sqrtf(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]);
				float flip = N[0] * P[0] + N[1] * P[1] + N[2] * P[2] > 0 ? -1.0f : 1.0f;
				zBad += !(fabsf(g[3] - z) <= 1e-3f * z);
				float err = 0;
				for (int k = 0; k < 3; k++)
					err = fmaxf(err, fabsf(g[k] - flip * N[k] / len));
				// Ties between the two sides of an axis can go either way in float: a few silhouette texels.
				nBad += err > 4e-3f;
			}
		float nFrac = texels ? (float)nBad / texels : 1.0f;
		bool ok = texels > 0 && zBad == 0 && nFrac < 0.002f;
		fails += !ok;
		printf("  shared geometry prep: %d texels, %d with z off by more than 1/1000, %.3f%% with the normal off the CPU reconstruction "
			   "(same rule) by more than 0.004: %s\n",
			texels, zBad, 100.0f * nFrac, ok ? "yes" : "NO");
		struct Probe
		{
			const char* what;
			float x, y, z, n[3];
		} const probes[] = {
			{ "floor", 2.0f, -1.6f, 4.5f, { 0, 1, 0 } },
			{ "back wall", 1.5f, 1.0f, 10.0f, { 0, 0, -1 } },
			{ "left wall", -3.0f, 0.5f, 7.0f, { 1, 0, 0 } },
			{ "box top", 0.1f, -0.6f, 4.6f, { 0, 1, 0 } },
		};
		for (const Probe& pr : probes)
		{
			int px, py;
			Project(pr.x, pr.y, pr.z, px, py);
			const float* g = &geoHalf[((size_t)(py / 2) * HW + px / 2) * 4];
			float err = fmaxf(fmaxf(fabsf(g[0] - pr.n[0]), fabsf(g[1] - pr.n[1])), fabsf(g[2] - pr.n[2]));
			bool pok = err < 0.01f;
			fails += !pok;
			printf("  shared geometry prep normal on the %-10s (%.3f, %.3f, %.3f): %s\n", pr.what, g[0], g[1], g[2], pok ? "yes" : "NO");
		}
	}
	printf("ao (WARP): GTAO, contact shadows, blur and composite on a synthetic scene (AO values)\n");
	check("open floor", aoAt(2.0f, -1.6f, 4.5f), 0.9f, 1.15f);
	check("open back wall", aoAt(1.5f, 1.0f, 10.0f), 0.9f, 1.15f);
	check("floor / left wall crease", aoAt(-2.95f, -1.6f, 6.0f), 0.2f, 0.75f);
	check("back wall just above the floor", aoAt(2.5f, -1.5f, 10.0f), 0.2f, 0.85f);
	check("floor at the front foot of the box", aoAt(0.1f, -1.6f, 3.95f), 0.2f, 0.85f);
	check("box top (open)", aoAt(0.1f, -0.6f, 4.6f), 0.85f, 1.15f);
	// Sky stays 1 in the AO and untouched in the composite.
	int skyX = W / 2, skyY = 4;
	check("sky", ao[(size_t)(skyY / 2) * HW + skyX / 2], 1.0f, 1.0f);
	printf("  contact shadows (1 = lit), sun up and behind the box:\n");
	check("floor just in front of the box", csAt(0.1f, -1.6f, 3.85f), 0.0f, 0.5f);
	check("open floor", csAt(2.0f, -1.6f, 4.5f), 0.95f, 1.0f);
	check("box top", csAt(0.1f, -0.6f, 4.6f), 0.95f, 1.0f);
	check("floor beside the sphere, away from the sun", csAt(-1.0f, -1.6f, 6.5f), 0.95f, 1.0f);
	check("back wall (faces away from the sun)", csAt(1.5f, 1.0f, 10.0f), 1.0f, 1.0f);
	// Composite: scene 1, indirect 0.5, sun 0.3 -> 1 - (1 - AO) x 0.5 - (1 - contact) x 0.3 (upsampled;
	// compare in a crease and in the contact shadow)
	{
		const float pts[2][3] = { { -2.95f, -1.6f, 6.0f }, { 0.1f, -1.6f, 3.85f } };
		const char* names[2] = { "composite in the crease", "composite in the contact shadow" };
		for (int i = 0; i < 2; i++)
		{
			int px, py;
			Project(pts[i][0], pts[i][1], pts[i][2], px, py);
			float got = result[((size_t)py * W + px) * 4];
			size_t t = (size_t)(py / 2) * HW + px / 2;
			float expect = 1.0f - (1.0f - fminf(fmaxf(ao[t], 0), 1)) * 0.5f - (1.0f - fminf(fmaxf(contact[t], 0), 1)) * 0.3f;
			bool ok = fabsf(got - expect) < 0.08f && got < 0.95f;
			fails += !ok;
			printf("  %-44s %.3f (1 - (1-AO) x 0.5 - (1-contact) x 0.3 = %.3f): %s\n", names[i], got, expect, ok ? "yes" : "NO");
		}
		float sky = result[((size_t)skyY * W + skyX) * 4];
		fails += sky != 1.0f;
		printf("  %-44s %.3f: %s\n", "composite leaves the sky alone", sky, sky == 1.0f ? "yes" : "NO");
	}
	// ---- TAA ------------------------------------------------------------------------------------
	printf("ao (WARP) with TAA:\n");
	{
		// TAA off: offsets 0 give exactly the old output.
		printf("  TAA off, output hashes: geometry prep %016llX, GTAO %016llX, blurred AO %016llX, composite %016llX\n",
			(unsigned long long)hashGeo, (unsigned long long)hashGtao, (unsigned long long)hashAo, (unsigned long long)hashComposite);
		bool same = hashGeo == kTodayGeo && hashGtao == kTodayGtao && hashAo == kTodayAo && hashComposite == kTodayComposite;
		fails += !same;
		printf("  TAA off (noise offsets 0): every target bit for bit what the shaders gave before the offsets: %s\n", same ? "yes" : "NO");
	}
	{
		// Jitter: TAA's phase 8 offset (-0.4375, 0.3889) pixels. Pixel q of the jittered render shows the
		// unjittered image at q - o.
		const float jx = -0.4375f, jy = 0.3888889f, jndc[2] = { 2.0f * jx / W, -2.0f * jy / H };
		std::vector<float> depthJ((size_t)W * H);
		std::vector<uint8_t> surfJ((size_t)W * H);
		for (UINT y = 0; y < H; y++)
			for (UINT x = 0; x < W; x++)
			{
				float z = SceneZ(x - jx, y - jy);
				depthJ[(size_t)y * W + x] = z > 0 ? A + B / z : 1.0f;
				surfJ[(size_t)y * W + x] = (uint8_t)g_lastSurface;
			}
		Target tDepthJ = MakeTarget(dev, W, H, DXGI_FORMAT_R32_FLOAT, depthJ.data(), W * 4);
		Target tPos = MakeTarget(dev, W, H, DXGI_FORMAT_R32G32B32A32_FLOAT);
		// The reconstruction each pass uses, through a test entry point appended to its real source.
		const char* entry = "\nfloat4 PS_TestViewPos(VPOS_T vposIn : VPOS) : COLOR\n{\n\tfloat2 px = PIXEL(vposIn);\n"
							"\treturn float4(ViewPos(px, FetchZ(px)), 0);\n}\n";
		ID3DBlob* posGeo = CompileText(g_geoSrc + entry, "geoprep.hlsl", "PS_TestViewPos", "ps_4_0");
		ID3DBlob* posAo = CompileText(g_src + entry, "ao.hlsl", "PS_TestViewPos", "ps_4_0");
		if (!posGeo || !posAo)
			return 1;
		ID3D11PixelShader *psPosGeo = nullptr, *psPosAo = nullptr;
		dev->CreatePixelShader(posGeo->GetBufferPointer(), posGeo->GetBufferSize(), nullptr, &psPosGeo);
		dev->CreatePixelShader(posAo->GetBufferPointer(), posAo->GetBufferSize(), nullptr, &psPosAo);
		ID3D11ShaderResourceView* sJ[5] = { tDepthJ.srv, nullptr, nullptr, nullptr, nullptr };
		const float saved[4] = { cProj2[0], cProj2[1], cProj2[2], cProj2[3] };
		// Mean distance (in pixels at that depth) of the reconstructed positions from the ray-cast truth.
		auto posError = [&](ID3DBlob* code, ID3D11PixelShader* ps, bool fold) {
			cProj2[0] = fold ? jndc[0] : 0.0f;
			cProj2[1] = fold ? jndc[1] : 0.0f;
			pass(code, ps, tPos, W, H, sJ, across, false);
			std::vector<float> pos = ReadBack(dev, ctx, tPos, W, H, 4);
			double sum = 0;
			int n = 0;
			for (UINT y = 0; y < H; y += 3)
				for (UINT x = 0; x < W; x += 3)
				{
					float z = SceneZ(x - jx, y - jy);
					if (z <= 0)
						continue;
					float ndcX = 2.0f * (x - jx) / W - 1.0f, ndcY = 1.0f - 2.0f * (y - jy) / H;
					const float* p = &pos[((size_t)y * W + x) * 4];
					double ex = (p[0] - ndcX / P00 * z) * P00 / z * 0.5 * W, ey = (p[1] - ndcY / P11 * z) * P11 / z * 0.5 * H;
					sum += sqrt(ex * ex + ey * ey);
					n++;
				}
			return n ? sum / n : 1e9;
		};
		double gF = posError(posGeo, psPosGeo, true), gU = posError(posGeo, psPosGeo, false);
		double aF = posError(posAo, psPosAo, true), aU = posError(posAo, psPosAo, false);
		const double jl = sqrt((double)jx * jx + (double)jy * jy);
		bool okPos = gF < 0.01 && aF < 0.01 && fabs(gU - jl) < 0.05 && fabs(aU - jl) < 0.05;
		fails += !okPos;
		printf("  jittered by (%.4f, %.4f) px: view positions off the truth by %.4f px (geometry prep) / %.4f px (GTAO) with the offset "
			   "folded in, %.4f / %.4f px without (expected %.4f): %s\n",
			jx, jy, gF, aF, gU, aU, jl, okPos ? "yes" : "NO");

		// The prep's normals on the floor and the left wall: without the offset, the positions are
		// sheared (x by -o.x / P00 * z, y by -o.y / P11 * z), which tilts those planes towards z.
		auto tilt = [&](bool fold, double& floorZ, double& wallZ) {
			cProj2[0] = fold ? jndc[0] : 0.0f;
			cProj2[1] = fold ? jndc[1] : 0.0f;
			pass(geoCode, geoPs, tGeo, HW, HH, sJ, across, false);
			std::vector<float> g = ReadHalf(dev, ctx, tGeo, HW, HH);
			double sf = 0, sw = 0;
			int nf = 0, nw = 0;
			for (UINT j = 2; j + 2 < HH; j++)
				for (UINT i = 2; i + 2 < HW; i++)
				{
					int x = 2 * i, y = 2 * j, surf = surfJ[(size_t)y * W + x];
					if (surf != SURF_FLOOR && surf != SURF_LEFT)
						continue;
					bool flat = true; // all neighbours the reconstruction uses on the same plane
					for (int dy = -1; dy <= 1 && flat; dy++)
						for (int dx = -1; dx <= 1 && flat; dx++)
							flat = surfJ[(size_t)(y + dy) * W + x + dx] == surf;
					if (!flat)
						continue;
					double nz = g[((size_t)j * HW + i) * 4 + 2];
					if (surf == SURF_FLOOR)
						sf += nz, nf++;
					else
						sw += nz, nw++;
				}
			floorZ = nf ? sf / nf : 1;
			wallZ = nw ? sw / nw : 1;
		};
		double fF, wF, fU, wU;
		tilt(true, fF, wF);
		tilt(false, fU, wU);
		// Expected without: the floor's normal (0, 1, 0) gets z = -o.y / P11 (o in NDC), the left wall's
		// (1, 0, 0) z = -o.x / P00.
		const double expF = -jndc[1] / P11, expW = -jndc[0] / P00;
		bool okN = fabs(fF) < 0.1 * fabs(expF) && fabs(wF) < 0.1 * fabs(expW) && fabs(fU - expF) < 0.2 * fabs(expF) &&
			fabs(wU - expW) < 0.2 * fabs(expW);
		fails += !okN;
		printf("  normal tilt (mean z of the prep's normals) on the floor %.6f, left wall %.6f with the offset folded in; %.6f, %.6f "
			   "without (expected %.6f, %.6f): %s\n",
			fF, wF, fU, wU, expF, expW, okN ? "yes" : "NO");
		memcpy(cProj2, saved, sizeof(saved));
		tDepthJ.tex->Release();
		tPos.tex->Release();
	}
	{
		// Noise: one frame against 16 frames averaged with the offsets the mod sets while TAA runs.
		// The reference is what the same GTAO converges to over many frames: the mean of 64 frames
		// with offsets on an 8 x 8 grid, unblurred. (Not GTAO with more slices and steps: its horizons
		// are maxima over the steps, so 3 x 6 samples stay a little below 24 x 48 however many frames
		// are averaged; that difference is the method's, and TAA can't change it. What TAA removes is
		// the noise around this mean.)
		const float saved[4] = { cProj2[0], cProj2[1], cProj2[2], cProj2[3] };
		pass(geoCode, geoPs, tGeo, HW, HH, sGeo, across, false);
		// The narrow blur the mod uses while TAA averages the frames (sigma 1 instead of 2).
		ID3DBlob* lightCode = CompileEntry("PS_BlurNarrow", "ps_4_0");
		if (!lightCode)
			return 1;
		ID3D11PixelShader* lightPs = nullptr;
		dev->CreatePixelShader(lightCode->GetBufferPointer(), lightCode->GetBufferSize(), nullptr, &lightPs);
		// One frame of the mod's chain (GTAO, blur across, blur down) with offsets `o`; blur 0 stops
		// after GTAO, 1 = the blur without TAA, 2 = the narrow one. Returns AO (r) and contact (b).
		auto frame = [&](const float o[2], int blurred) {
			cProj2[2] = o[0];
			cProj2[3] = o[1];
			pass(gtaoCode, gtao, tAo[0], HW, HH, s0, across, false);
			if (blurred)
			{
				pass(blurred == 2 ? lightCode : blurCode, blurred == 2 ? lightPs : blur, tAo[1], HW, HH, s1, across, false);
				pass(blurred == 2 ? lightCode : blurCode, blurred == 2 ? lightPs : blur, tAo[0], HW, HH, s2, down, false);
			}
			return ReadHalf(dev, ctx, tAo[0], HW, HH);
		};
		const size_t texels = (size_t)HW * HH * 4;
		std::vector<float> ref(texels, 0.0f);
		for (int i = 0; i < 64; i++)
		{
			const float o[2] = { (i % 8 + 0.5f) / 8, (i / 8 + 0.5f) / 8 };
			std::vector<float> r = frame(o, 0);
			for (size_t k = 0; k < texels; k++)
				ref[k] += r[k] / 64;
		}
		const int N = 16;
		const float zero[2] = { 0, 0 };
		std::vector<float> single = frame(zero, 1), singleRaw = frame(zero, 0), singleLight = frame(zero, 2);
		std::vector<float> avgBlur(texels, 0.0f), avgRaw(texels, 0.0f), avgLight(texels, 0.0f), prevBlur, prevRaw, prevLight;
		double flickerBlur = 0, flickerRaw = 0, flickerLight = 0;
		for (int f = 0; f < N; f++)
		{
			float o[2];
			R2((unsigned)f + 1, o);
			std::vector<float> b = frame(o, 1), raw = frame(o, 0), light = frame(o, 2);
			for (size_t k = 0; k < texels; k++)
			{
				avgBlur[k] += b[k] / N;
				avgRaw[k] += raw[k] / N;
				avgLight[k] += light[k] / N;
			}
			for (size_t k = 0; f > 0 && k < texels; k += 4)
			{
				flickerBlur += fabs(b[k] - prevBlur[k]);
				flickerRaw += fabs(raw[k] - prevRaw[k]);
				flickerLight += fabs(light[k] - prevLight[k]);
			}
			prevBlur = b;
			prevRaw = raw;
			prevLight = light;
		}
		// Mean |error| over the texels that aren't sky (AO clamped as the composite does); contact
		// shadows only where the reference or the one-frame result is shadowed (most texels are lit
		// and exact either way).
		auto err = [&](const std::vector<float>& v, int ch) {
			double sum = 0;
			int n = 0;
			for (size_t k = 0; k < texels; k += 4)
			{
				size_t t = k / 4;
				if (depth[(t / HW * 2) * W + (t % HW) * 2] >= 1.0f)
					continue;
				if (ch == 2 && ref[k + 2] > 0.99f && singleRaw[k + 2] > 0.99f)
					continue;
				float a = v[k + ch], b = ref[k + ch];
				if (ch == 0)
				{
					a = fminf(fmaxf(a, 0), 1);
					b = fminf(fmaxf(b, 0), 1);
				}
				sum += fabs(a - b);
				n++;
			}
			return n ? sum / n : 1e9;
		};
		double e1 = err(single, 0), eAvg = err(avgBlur, 0), e1Raw = err(singleRaw, 0), eRaw = err(avgRaw, 0);
		double e1Light = err(singleLight, 0), eLight = err(avgLight, 0);
		double c1 = err(single, 2), cAvg = err(avgBlur, 2), c1Raw = err(singleRaw, 2), cRaw = err(avgRaw, 2);
		double c1Light = err(singleLight, 2), cLight = err(avgLight, 2);
		const double changes = (double)HW * HH * (N - 1);
		printf("  AO against what GTAO converges to (mean |error|), one frame -> %d frames averaged: blur without TAA (sigma 2) %.4f -> "
			   "%.4f (%.0f%%); narrow blur with TAA (sigma 1) %.4f -> %.4f; no blur %.4f -> %.4f\n",
			N, e1, eAvg, 100.0 * eAvg / e1, e1Light, eLight, e1Raw, eRaw);
		printf("  contact shadows (shadowed texels): blur without TAA %.4f -> %.4f; narrow blur %.4f -> %.4f; no blur %.4f -> %.4f\n", c1,
			cAvg, c1Light, cLight, c1Raw, cRaw);
		printf("  AO frame-to-frame change with the offsets (what TAA has to absorb): blur without TAA %.4f, narrow %.4f, none %.4f\n",
			flickerBlur / changes, flickerLight / changes, flickerRaw / changes);
		// The contact shadow noise is checked before the blur: blurred, its error is the blur's
		// spread of the shadow edge, which no averaging changes.
		bool okNoise = eAvg < 0.8 * e1 && cRaw < 0.8 * c1Raw;
		fails += !okNoise;
		printf("  averaging frames with the per-frame offsets gets closer to it than one frame: %s\n", okNoise ? "yes" : "NO");
		// The narrow blur pays only if a single frame (just after a disocclusion, or in fast motion,
		// where TAA keeps little history) is no worse than with the wide blur, and the average is better.
		bool okNarrow = e1Light < 1.15 * e1 && eLight < 0.85 * eAvg && c1Light < c1 && cLight < cAvg;
		fails += !okNarrow;
		printf("  narrow blur with TAA: one frame no worse than the wide blur (AO %.4f vs %.4f, contact %.4f vs %.4f), averaged better "
			   "(AO %.4f vs %.4f, contact %.4f vs %.4f): %s\n",
			e1Light, e1, c1Light, c1, eLight, eAvg, cLight, cAvg, okNarrow ? "yes" : "NO");
		memcpy(cProj2, saved, sizeof(saved));
	}
	printf("ao (WARP): %s (pictures: tests/ao_warp_ao.png, ao_warp_contact.png, ao_warp_composite.png)\n", fails ? "FAILED" : "all ok");
	return fails ? 3 : 0;
}
