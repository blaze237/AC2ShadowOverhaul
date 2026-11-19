// Runs the real AO shaders (src/shaders/ao.hlsl) on a synthetic scene with D3D11's software
// renderer (WARP), so the GTAO maths is exercised without a D3D9 GPU. The HLSL is compiled for
// shader model 4 in backwards-compatibility mode with AO_D3D11 defined (pixel centres differ).
//
// The AO texture holds the unclamped estimate (open surfaces read about 1, sometimes a little more;
// the composite clamps).
//
// Scene (view space, metres, camera at the origin looking down +z, 90 degree horizontal fov, 16:9):
// floor y = -1.6, back wall z = 10, left wall x = -3, a box on the floor and a sphere by the left
// wall; above the walls is sky. Checks: open floor / open wall stay unoccluded, the creases and the
// floor at the box's foot darken, sky is untouched, and the composite removes exactly
// (1 - AO) x indirect. Writes ao_warp_ao.png / ao_warp_composite.png to look at.
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

static const UINT W = 640, H = 360, HW = W / 2, HH = H / 2;
static const float P00 = 1.0f, P11 = 16.0f / 9.0f, NEAR_ = 0.1f, FAR_ = 1000.0f;
static const float A = FAR_ / (FAR_ - NEAR_), B = -NEAR_ * FAR_ / (FAR_ - NEAR_);

typedef HRESULT(WINAPI* PFN_Compile)(const void*, SIZE_T, const char*, const D3D_SHADER_MACRO*, ID3DInclude*, const char*, const char*, UINT, UINT,
	ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI* PFN_Reflect)(const void*, SIZE_T, REFIID, void**);
static PFN_Compile g_compile;
static PFN_Reflect g_reflect;
static std::string g_src;

static const GUID kIID_Reflection = { 0x8d536ca1, 0x0cca, 0x4956, { 0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84 } };

static ID3DBlob* CompileEntry(const char* entry, const char* profile)
{
	const D3D_SHADER_MACRO defs[] = { { "AO_D3D11", "1" }, { nullptr, nullptr } };
	ID3DBlob *code = nullptr, *err = nullptr;
	HRESULT hr = g_compile(g_src.data(), g_src.size(), "ao.hlsl", defs, nullptr, entry, profile,
		D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
	if (FAILED(hr))
		printf("%s: compile failed: %s\n", entry, err ? (const char*)err->GetBufferPointer() : "?");
	return code;
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

static float SceneZ(float px, float py) // view z at a pixel, or 0 for sky
{
	float ndcX = 2.0f * px / W - 1.0f, ndcY = 1.0f - 2.0f * py / H;
	float d[3] = { ndcX / P00, ndcY / P11, 1.0f }, o[3] = { 0, 0, 0 };
	float best = 1e30f, t;
	auto consider = [&](float tt, bool ok) {
		if (ok && tt > 0 && tt < best)
			best = tt;
	};
	if (d[1] < 0)
	{
		t = -1.6f / d[1];
		consider(t, d[0] * t > -3.0f && t < 10.0f);
	}
	t = 10.0f;
	consider(t, d[1] * t >= -1.6f && d[1] * t <= 3.0f && d[0] * t >= -3.0f);
	if (d[0] < 0)
	{
		t = -3.0f / d[0];
		consider(t, d[1] * t >= -1.6f && d[1] * t <= 3.0f && t <= 10.0f);
	}
	const float lo[3] = { -0.5f, -1.6f, 4.0f }, hi[3] = { 0.7f, -0.6f, 5.2f };
	if (RayBox(o, d, lo, hi, t))
		consider(t, true);
	// sphere centre (-1.9, -0.8, 7.5), r 0.8
	const float c[3] = { -1.9f, -0.8f, 7.5f }, r = 0.8f;
	float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2], dc = d[0] * c[0] + d[1] * c[1] + d[2] * c[2];
	float cc = c[0] * c[0] + c[1] * c[1] + c[2] * c[2] - r * r, disc = dc * dc - dd * cc;
	if (disc >= 0)
		consider((dc - sqrtf(disc)) / dd, true);
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

int main()
{
	FILE* f = fopen("../src/shaders/ao.hlsl", "rb");
	if (!f)
		return printf("run from tests/\n"), 1;
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

	ID3D11Device* dev = nullptr;
	ID3D11DeviceContext* ctx = nullptr;
	D3D_FEATURE_LEVEL fl;
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx)))
		return printf("no WARP device\n"), 1;

	ID3DBlob* vsCode = CompileEntry("VS_Fullscreen", "vs_4_0");
	ID3DBlob* gtaoCode = CompileEntry("PS_GTAO", "ps_4_0");
	ID3DBlob* blurCode = CompileEntry("PS_Blur", "ps_4_0");
	ID3DBlob* compCode = CompileEntry("PS_Composite", "ps_4_0");
	if (!vsCode || !gtaoCode || !blurCode || !compCode)
		return 1;
	ID3D11VertexShader* vs = nullptr;
	ID3D11PixelShader *gtao = nullptr, *blur = nullptr, *comp = nullptr;
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
	std::vector<float> depth((size_t)W * H), indirect((size_t)W * H * 4, 0.5f), scene((size_t)W * H * 4, 1.0f);
	for (UINT y = 0; y < H; y++)
		for (UINT x = 0; x < W; x++)
		{
			float z = SceneZ((float)x, (float)y);
			depth[(size_t)y * W + x] = z > 0 ? A + B / z : 1.0f;
		}
	Target tDepth = MakeTarget(dev, W, H, DXGI_FORMAT_R32_FLOAT, depth.data(), W * 4);
	Target tInd = MakeTarget(dev, W, H, DXGI_FORMAT_R32G32B32A32_FLOAT, indirect.data(), W * 16);
	Target tScene = MakeTarget(dev, W, H, DXGI_FORMAT_R32G32B32A32_FLOAT, scene.data(), W * 16);
	Target tAo[2] = { MakeTarget(dev, HW, HH, DXGI_FORMAT_R16G16_FLOAT), MakeTarget(dev, HW, HH, DXGI_FORMAT_R16G16_FLOAT) };

	D3D11_SAMPLER_DESC smp = {};
	smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	smp.MaxLOD = D3D11_FLOAT32_MAX;
	ID3D11SamplerState* point = nullptr;
	dev->CreateSamplerState(&smp, &point);
	ID3D11SamplerState* samplers[3] = { point, point, point };
	D3D11_BLEND_DESC bl = {};
	bl.RenderTarget[0].BlendEnable = TRUE;
	bl.RenderTarget[0].SrcBlend = bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	bl.RenderTarget[0].DestBlend = bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
	bl.RenderTarget[0].BlendOp = bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_REV_SUBTRACT;
	bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
	ID3D11BlendState* subtract = nullptr;
	dev->CreateBlendState(&bl, &subtract);

	const float radius = 1.5f, strength = 1.0f;
	const float cFull[4] = { (float)W, (float)H, 1.0f / W, 1.0f / H }, cHalf[4] = { (float)HW, (float)HH, 1.0f / HW, 1.0f / HH };
	const float cProj[4] = { A, B, P00, P11 }, cProj2[4] = { 0, 0, 0, 0 };
	const float cParams[4] = { radius, 0.5f * P00 * W, 0.1f * H, 0.6f };
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
		ID3D11Buffer* cb = nullptr;
		if (!c.data.empty())
		{
			D3D11_BUFFER_DESC cbd = { (UINT)c.data.size(), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
			D3D11_SUBRESOURCE_DATA cd = { c.data.data(), 0, 0 };
			dev->CreateBuffer(&cbd, &cd, &cb);
		}
		ID3D11ShaderResourceView* none[3] = {};
		ctx->PSSetShaderResources(0, 3, none);
		ctx->OMSetRenderTargets(1, &out.rtv, nullptr);
		static const char* const names[3] = { "s_Depth", "s_AO", "s_Indirect" };
		for (auto& t : c.textures)
			for (int k = 0; k < 3; k++)
				if (t.first == names[k] && t.second < 3)
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
		ctx->PSSetSamplers(0, 3, samplers);
		float zero[4] = {};
		ctx->OMSetBlendState(blend ? subtract : nullptr, zero, 0xFFFFFFFF);
		ctx->Draw(3, 0);
		if (cb)
			cb->Release();
	};
	const float across[4] = { strength, 0, 1, 0 }, down[4] = { strength, 0, 0, 1 };
	ID3D11ShaderResourceView* s0[3] = { tDepth.srv, nullptr, nullptr };
	pass(gtaoCode, gtao, tAo[0], HW, HH, s0, across, false);
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
	ID3D11ShaderResourceView* s1[3] = { tDepth.srv, tAo[0].srv, nullptr };
	pass(blurCode, blur, tAo[1], HW, HH, s1, across, false);
	ID3D11ShaderResourceView* s2[3] = { tDepth.srv, tAo[1].srv, nullptr };
	pass(blurCode, blur, tAo[0], HW, HH, s2, down, false);
	ID3D11ShaderResourceView* s3[3] = { tDepth.srv, tAo[0].srv, tInd.srv };
	pass(compCode, comp, tScene, W, H, s3, across, true);

	// The blurred AO (R16G16_FLOAT, read as halves).
	std::vector<float> ao((size_t)HW * HH);
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
				uint16_t hbits = ((const uint16_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch))[x * 2];
				uint32_t sign = (hbits >> 15) & 1, e = (hbits >> 10) & 31, mant = hbits & 1023;
				float v = e == 0 ? ldexpf((float)mant, -24) : ldexpf((float)(mant | 1024), (int)e - 25);
				ao[(size_t)y * HW + x] = sign ? -v : v;
			}
		ctx->Unmap(st, 0);
		st->Release();
	}
	std::vector<float> result = ReadBack(dev, ctx, tScene, W, H, 4);

	std::vector<uint8_t> grey(ao.size()), comp8((size_t)W * H);
	for (size_t i = 0; i < ao.size(); i++)
		grey[i] = (uint8_t)lroundf(fminf(fmaxf(ao[i], 0), 1) * 255);
	for (size_t i = 0; i < comp8.size(); i++)
		comp8[i] = (uint8_t)lroundf(fminf(fmaxf(result[i * 4], 0), 1) * 255);
	WritePng("ao_warp_ao.png", grey, HW, HH);
	WritePng("ao_warp_composite.png", comp8, W, H);

	auto aoAt = [&](float x, float y, float z) {
		int px, py;
		Project(x, y, z, px, py);
		// average a 3x3 block of AO texels around the point
		float s = 0;
		int c = 0;
		for (int dy = -1; dy <= 1; dy++)
			for (int dx = -1; dx <= 1; dx++)
			{
				int ax = px / 2 + dx, ay = py / 2 + dy;
				if (ax >= 0 && ay >= 0 && ax < (int)HW && ay < (int)HH)
				{
					s += ao[(size_t)ay * HW + ax];
					c++;
				}
			}
		return c ? s / c : -1.0f;
	};
	int fails = 0;
	auto check = [&](const char* what, float v, float lo, float hi) {
		bool ok = v >= lo && v <= hi;
		fails += !ok;
		printf("  %-44s AO %.3f (expected %.2f..%.2f): %s\n", what, v, lo, hi, ok ? "yes" : "NO");
	};
	printf("ao (WARP): GTAO, blur and composite on a synthetic scene\n");
	check("open floor", aoAt(2.0f, -1.6f, 4.5f), 0.9f, 1.15f);
	check("open back wall", aoAt(1.5f, 1.0f, 10.0f), 0.9f, 1.15f);
	check("floor / left wall crease", aoAt(-2.95f, -1.6f, 6.0f), 0.2f, 0.75f);
	check("back wall just above the floor", aoAt(2.5f, -1.5f, 10.0f), 0.2f, 0.85f);
	check("floor at the front foot of the box", aoAt(0.1f, -1.6f, 3.95f), 0.2f, 0.85f);
	check("box top (open)", aoAt(0.1f, -0.6f, 4.6f), 0.85f, 1.15f);
	// Sky stays 1 in the AO and untouched in the composite.
	int skyX = W / 2, skyY = 4;
	check("sky", ao[(size_t)(skyY / 2) * HW + skyX / 2], 1.0f, 1.0f);
	// Composite: scene 1, indirect 0.5 -> 1 - (1 - AO) * 0.5 (AO upsampled; compare in an open area and a crease)
	{
		int px, py;
		Project(-2.95f, -1.6f, 6.0f, px, py);
		float got = result[((size_t)py * W + px) * 4];
		float expect = 1.0f - (1.0f - ao[(size_t)(py / 2) * HW + px / 2]) * 0.5f;
		bool ok = fabsf(got - expect) < 0.06f && got < 0.95f;
		fails += !ok;
		printf("  %-44s %.3f (expected about %.3f): %s\n", "composite in the crease = 1 - (1 - AO) x 0.5", got, expect, ok ? "yes" : "NO");
		float sky = result[((size_t)skyY * W + skyX) * 4];
		fails += sky != 1.0f;
		printf("  %-44s %.3f: %s\n", "composite leaves the sky alone", sky, sky == 1.0f ? "yes" : "NO");
	}
	printf("ao (WARP): %s (pictures: tests/ao_warp_ao.png, tests/ao_warp_composite.png)\n", fails ? "FAILED" : "all ok");
	return fails ? 3 : 0;
}
