// Tests the vertex shader jitter rewrite (src/vsjitter.cpp).
//
// For every shader (real game shaders from an F8 frame dump, plus synthetic ones compiled here
// with d3dcompiler_47): rewrite it, disassemble the result (the bytecode must still parse), and run
// both versions through a small vs_3_0 / vs_2_0 interpreter with random inputs and constants.
// Every output must match the original, except the position, which must be moved by exactly
// c255.xy * w. With c255 = 0 the position must be unchanged too. Shaders that must be left alone
// (full-screen passes, shaders using c255) are checked to be left alone.
//
// If a Direct3D 9 device can be created on this machine, the rewritten shaders are also created
// on it (the runtime validates the bytecode).
//
// Usage: vsjitter_test [dir-with-vs_*.cso]
#include "../src/vsjitter.h"
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>

typedef HRESULT(WINAPI* PFN_Compile)(const void*, SIZE_T, const char*, const D3D_SHADER_MACRO*, ID3DInclude*, const char*, const char*, UINT, UINT,
	ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI* PFN_Disasm)(const void*, SIZE_T, UINT, const char*, ID3DBlob**);
static PFN_Compile g_compile;
static PFN_Disasm g_disasm;

static int g_failures = 0;
#define CHECK(cond, ...)                \
	do                                  \
	{                                   \
		if (!(cond))                    \
		{                               \
			printf("  FAIL: " __VA_ARGS__); \
			printf("\n");               \
			g_failures++;               \
		}                               \
	} while (0)

// ---- Interpreter -------------------------------------------------------------------------------

struct Machine
{
	float r[32][4] = {}, v[16][4] = {}, c[256][4] = {}, o[16][4] = {}, rast[3][4] = {};
	int i[16][4] = {}, a0[4] = {}, aL = 0;
	bool b[16] = {}, p0[4] = {};
	std::string error;
};

static int RegType(DWORD t) { return (int)(((t >> 28) & 7) | ((t >> 8) & 0x18)); }
static int RegNum(DWORD t) { return (int)(t & 0x7FF); }

struct Interp
{
	const DWORD* code;
	Machine& m;
	std::vector<size_t> labels = std::vector<size_t>(64, 0);
	int steps = 0;

	Interp(const DWORD* c, Machine& mm) : code(c), m(mm) {}

	size_t Len(size_t pc)
	{
		DWORD t = code[pc];
		return (t & 0xFFFF) == 0xFFFE ? ((t >> 16) & 0x7FFF) : ((t >> 24) & 0xF);
	}

	// Reads a source operand at token index *p (advances past a relative address token).
	void Read(size_t& p, float out[4])
	{
		DWORD t = code[p++];
		int type = RegType(t), num = RegNum(t);
		if (t & 0x2000)
		{
			DWORD at = code[p++];
			int comp = (at >> 16) & 3;
			num += RegType(at) == 15 ? m.aL : m.a0[comp];
		}
		float base[4] = { 0, 0, 0, 0 };
		switch (type)
		{
		case 0: if (num < 32) memcpy(base, m.r[num], 16); break;
		case 1: if (num < 16) memcpy(base, m.v[num], 16); break;
		case 2: if (num >= 0 && num < 256) memcpy(base, m.c[num], 16); break; // out of range reads 0 (both runs alike)
		case 7: if (num < 16) for (int k = 0; k < 4; k++) base[k] = (float)m.i[num][k]; break;
		case 14: if (num < 16) for (int k = 0; k < 4; k++) base[k] = m.b[num] ? 1.0f : 0.0f; break;
		case 15: for (int k = 0; k < 4; k++) base[k] = (float)m.aL; break;
		case 3: for (int k = 0; k < 4; k++) base[k] = (float)m.a0[k]; break;
		default: m.error = "unsupported source register type " + std::to_string(type);
		}
		for (int k = 0; k < 4; k++)
			out[k] = base[(t >> (16 + 2 * k)) & 3];
		int mod = (t >> 24) & 0xF;
		for (int k = 0; k < 4; k++)
		{
			if (mod == 1) out[k] = -out[k];
			else if (mod == 0xB) out[k] = fabsf(out[k]);
			else if (mod == 0xC) out[k] = -fabsf(out[k]);
			else if (mod != 0) m.error = "unsupported source modifier " + std::to_string(mod);
		}
	}

	float* DstPtr(DWORD t)
	{
		int type = RegType(t), num = RegNum(t);
		switch (type)
		{
		case 0: return num < 32 ? m.r[num] : nullptr;
		case 6: return num < 16 ? m.o[num] : nullptr;
		case 4: return num < 3 ? m.rast[num] : nullptr;
		case 3: return nullptr; // a0 handled by mova
		}
		m.error = "unsupported destination register type " + std::to_string(type);
		return nullptr;
	}

	void Write(DWORD dst, const float res[4], bool predicated)
	{
		int mask = (dst >> 16) & 0xF;
		bool sat = (dst >> 20) & 1;
		float* d = DstPtr(dst);
		if (!d)
			return;
		for (int k = 0; k < 4; k++)
			if ((mask >> k) & 1 && (!predicated || m.p0[k]))
			{
				float x = res[k];
				if (sat)
					x = x < 0 ? 0 : x > 1 ? 1 : x;
				d[k] = x;
			}
	}

	// Skips to the matching else/endif (or endrep/endloop) from pc (an if/rep/loop); returns its pc.
	size_t SkipBlock(size_t pc, bool stopAtElse)
	{
		int depth = 0;
		for (size_t q = pc + 1 + Len(pc);; q += 1 + Len(q))
		{
			int op = code[q] & 0xFFFF;
			if (op == 0x28 || op == 0x29 || op == 0x26 || op == 0x1B)
				depth++;
			else if (op == 0x2B || op == 0x27 || op == 0x1D)
			{
				if (depth == 0)
					return q;
				depth--;
			}
			else if (op == 0x2A && depth == 0 && stopAtElse)
				return q;
			if (code[q] == 0x0000FFFF)
				return q;
		}
	}

	bool Run()
	{
		// Labels first.
		for (size_t pc = 1; code[pc] != 0x0000FFFF; pc += 1 + Len(pc))
			if ((code[pc] & 0xFFFF) == 0x1E)
				labels[RegNum(code[pc + 1]) & 63] = pc;
		return Exec(1, 0);
	}

	// Executes from pc until ret/end (or an unmatched block end when depth > 0). Returns false on error.
	bool Exec(size_t pc, int callDepth)
	{
		struct Loop { size_t start; int left; };
		std::vector<Loop> loops;
		while (true)
		{
			if (++steps > 200000)
				return m.error = "too many steps", false;
			DWORD tok = code[pc];
			if (tok == 0x0000FFFF)
				return true;
			int op = tok & 0xFFFF;
			size_t len = Len(pc);
			size_t next = pc + 1 + len;
			bool pred = (tok >> 28) & 1;
			if (op == 0xFFFE || op == 0x1F || op == 0x51 || op == 0x30 || op == 0x2F || op == 0)
			{
				pc = next;
				continue;
			}
			size_t p = pc + 1;
			float s[4][4];
			float res[4] = { 0, 0, 0, 0 };
			auto dst = [&]() { return code[pc + 1]; };
			auto srcs = [&](int n) {
				p = pc + 2;
				if (pred)
					p++; // predicate token after the destination
				for (int k = 0; k < n; k++)
					Read(p, s[k]);
			};
			switch (op)
			{
			case 0x01: srcs(1); memcpy(res, s[0], 16); Write(dst(), res, pred); break; // mov
			case 0x02: srcs(2); for (int k = 0; k < 4; k++) res[k] = s[0][k] + s[1][k]; Write(dst(), res, pred); break;
			case 0x04: srcs(3); for (int k = 0; k < 4; k++) res[k] = s[0][k] * s[1][k] + s[2][k]; Write(dst(), res, pred); break;
			case 0x05: srcs(2); for (int k = 0; k < 4; k++) res[k] = s[0][k] * s[1][k]; Write(dst(), res, pred); break;
			case 0x06: srcs(1); for (int k = 0; k < 4; k++) res[k] = 1.0f / s[0][3]; Write(dst(), res, pred); break; // rcp (replicate swizzle)
			case 0x07: srcs(1); for (int k = 0; k < 4; k++) res[k] = 1.0f / sqrtf(fabsf(s[0][3])); Write(dst(), res, pred); break;
			case 0x08: srcs(2); { float d = s[0][0] * s[1][0] + s[0][1] * s[1][1] + s[0][2] * s[1][2]; for (int k = 0; k < 4; k++) res[k] = d; } Write(dst(), res, pred); break;
			case 0x09: srcs(2); { float d = s[0][0] * s[1][0] + s[0][1] * s[1][1] + s[0][2] * s[1][2] + s[0][3] * s[1][3]; for (int k = 0; k < 4; k++) res[k] = d; } Write(dst(), res, pred); break;
			case 0x0A: srcs(2); for (int k = 0; k < 4; k++) res[k] = s[0][k] < s[1][k] ? s[0][k] : s[1][k]; Write(dst(), res, pred); break;
			case 0x0B: srcs(2); for (int k = 0; k < 4; k++) res[k] = s[0][k] >= s[1][k] ? s[0][k] : s[1][k]; Write(dst(), res, pred); break;
			case 0x0C: srcs(2); for (int k = 0; k < 4; k++) res[k] = s[0][k] < s[1][k] ? 1.0f : 0.0f; Write(dst(), res, pred); break;
			case 0x0D: srcs(2); for (int k = 0; k < 4; k++) res[k] = s[0][k] >= s[1][k] ? 1.0f : 0.0f; Write(dst(), res, pred); break;
			case 0x0E: srcs(1); for (int k = 0; k < 4; k++) res[k] = exp2f(s[0][3]); Write(dst(), res, pred); break;
			case 0x0F: srcs(1); for (int k = 0; k < 4; k++) res[k] = log2f(fabsf(s[0][3])); Write(dst(), res, pred); break;
			case 0x12: srcs(3); for (int k = 0; k < 4; k++) res[k] = s[0][k] * (s[1][k] - s[2][k]) + s[2][k]; Write(dst(), res, pred); break;
			case 0x13: srcs(1); for (int k = 0; k < 4; k++) res[k] = s[0][k] - floorf(s[0][k]); Write(dst(), res, pred); break;
			case 0x20: srcs(2); for (int k = 0; k < 4; k++) res[k] = powf(fabsf(s[0][3]), s[1][3]); Write(dst(), res, pred); break;
			case 0x23: srcs(1); for (int k = 0; k < 4; k++) res[k] = fabsf(s[0][k]); Write(dst(), res, pred); break;
			case 0x24: srcs(1); { float f = 1.0f / sqrtf(s[0][0] * s[0][0] + s[0][1] * s[0][1] + s[0][2] * s[0][2]); for (int k = 0; k < 4; k++) res[k] = s[0][k] * f; } Write(dst(), res, pred); break;
			case 0x25: srcs(1); res[0] = cosf(s[0][3]); res[1] = sinf(s[0][3]); Write(dst(), res, pred); break; // sincos (replicate swizzle)
			case 0x14: case 0x15: case 0x16: case 0x17: case 0x18: // m4x4 m4x3 m3x4 m3x3 m3x2
			{
				int rows = op == 0x14 || op == 0x16 ? 4 : op == 0x18 ? 2 : 3, cols = op == 0x14 || op == 0x15 ? 4 : 3;
				p = pc + 2;
				Read(p, s[0]);
				DWORD mt = code[p];
				for (int rr = 0; rr < rows; rr++)
				{
					float row[4];
					size_t q = p;
					DWORD t2 = (mt & ~0x7FFu) | (DWORD)(RegNum(mt) + rr);
					// read the incremented register through a temporary token stream
					DWORD tmp[2] = { t2, code[p + 1] };
					const DWORD* saved = code;
					code = tmp;
					size_t z = 0;
					Read(z, row);
					code = saved;
					(void)q;
					float d = 0;
					for (int k = 0; k < cols; k++)
						d += s[0][k] * row[k];
					res[rr] = d;
				}
				Write(dst(), res, pred);
				break;
			}
			case 0x2E: // mova
			{
				srcs(1);
				DWORD d = dst();
				for (int k = 0; k < 4; k++)
					if ((d >> (16 + k)) & 1)
						m.a0[k] = (int)floorf(s[0][k] + 0.5f);
				break;
			}
			case 0x5F: // texldl from a vertex texture: zeros
				srcs(2);
				Write(dst(), res, pred);
				break;
			case 0x28: // if b
			{
				int bn = RegNum(code[pc + 1]);
				if (!m.b[bn & 15])
				{
					size_t q = SkipBlock(pc, true);
					next = q + 1 + Len(q);
				}
				break;
			}
			case 0x29: // ifc
			{
				srcs(0);
				p = pc + 1;
				Read(p, s[0]);
				Read(p, s[1]);
				int cmp = (tok >> 16) & 7;
				float a = s[0][0], bb = s[1][0];
				bool t = cmp == 1 ? a > bb : cmp == 2 ? a == bb : cmp == 3 ? a >= bb : cmp == 4 ? a < bb : cmp == 5 ? a != bb : cmp == 6 ? a <= bb : false;
				if (!t)
				{
					size_t q = SkipBlock(pc, true);
					next = q + 1 + Len(q);
				}
				break;
			}
			case 0x2A: // else reached from the taken branch: skip to endif
			{
				int depth = 0;
				size_t q = next;
				for (;; q += 1 + Len(q))
				{
					int o2 = code[q] & 0xFFFF;
					if (o2 == 0x28 || o2 == 0x29)
						depth++;
					else if (o2 == 0x2B)
					{
						if (depth == 0)
							break;
						depth--;
					}
				}
				next = q + 1;
				break;
			}
			case 0x2B: break; // endif
			case 0x26: // rep i#
			{
				int n = m.i[RegNum(code[pc + 1]) & 15][0];
				if (n <= 0)
				{
					size_t q = SkipBlock(pc, false);
					next = q + 1;
				}
				else
					loops.push_back({ next, n });
				break;
			}
			case 0x27: // endrep
				if (!loops.empty() && --loops.back().left > 0)
					next = loops.back().start;
				else if (!loops.empty())
					loops.pop_back();
				break;
			case 0x19: // call l#
				if (callDepth > 4)
					return m.error = "call depth", false;
				if (!Exec(labels[RegNum(code[pc + 1]) & 63] + 2, callDepth + 1))
					return false;
				break;
			case 0x1C: // ret
				return true;
			case 0x1E: // label reached by falling through main: end
				return true;
			default:
				return m.error = "unsupported opcode " + std::to_string(op), false;
			}
			if (!m.error.empty())
				return false;
			pc = next;
		}
	}
};

// ---- Checks ------------------------------------------------------------------------------------

static unsigned g_rng = 12345;
static float Rand(float lo, float hi)
{
	g_rng = g_rng * 1664525u + 1013904223u;
	return lo + (hi - lo) * ((g_rng >> 8) / 16777216.0f);
}

static bool SameFloat(float a, float b, float tol)
{
	if (isnan(a) || isnan(b))
		return isnan(a) && isnan(b);
	if (isinf(a) || isinf(b))
		return a == b;
	return fabsf(a - b) <= tol * (1.0f + fabsf(a) + fabsf(b));
}

static IDirect3DDevice9* g_dev;
static int g_devChecked = 0;

static std::string Disassemble(const std::vector<DWORD>& code)
{
	ID3DBlob* dis = nullptr;
	if (FAILED(g_disasm(code.data(), code.size() * 4, 0, nullptr, &dis)) || !dis)
		return "";
	std::string s((const char*)dis->GetBufferPointer());
	dis->Release();
	return s;
}

// expect: 1 = must be rewritten, 0 = must be left alone, -1 = either (real shaders)
static void TestShader(const char* name, const DWORD* code, int expect, bool verbose)
{
	std::vector<DWORD> out;
	VsJitterInfo info;
	std::string reason;
	bool ok = VsJitter_Rewrite(code, out, info, reason);
	if (verbose || (expect >= 0 && ok != (expect == 1)))
		printf("%-28s %s%s\n", name, ok ? "rewritten" : "left alone: ", ok ? "" : reason.c_str());
	if (expect >= 0)
		CHECK(ok == (expect == 1), "%s: expected %s", name, expect ? "a rewrite" : "no rewrite");
	if (!ok)
		return;
	std::string dis = Disassemble(out);
	CHECK(!dis.empty(), "%s: the rewritten shader doesn't disassemble", name);
	CHECK(dis.find("c255") != std::string::npos, "%s: no c255 in the rewritten shader", name);
	if (verbose)
	{
		size_t at = dis.find("c255");
		size_t from = dis.rfind('\n', dis.rfind('\n', at) - 1);
		printf("    ...%s\n", dis.substr(from, dis.find('\n', dis.find('\n', at) + 1) - from).c_str());
	}
	if (g_dev)
	{
		IDirect3DVertexShader9* vs = nullptr;
		HRESULT hr = g_dev->CreateVertexShader(out.data(), &vs);
		CHECK(SUCCEEDED(hr), "%s: Direct3D 9 rejected the rewritten shader (0x%08lX)", name, hr);
		if (vs)
			vs->Release();
		g_devChecked++;
	}

	for (int trial = 0; trial < 64; trial++)
	{
		Machine a, b;
		for (int k = 0; k < 16; k++)
			for (int j = 0; j < 4; j++)
				a.v[k][j] = Rand(-2, 2);
		for (int k = 0; k < 256; k++)
			for (int j = 0; j < 4; j++)
				a.c[k][j] = Rand(-1, 1);
		for (int k = 0; k < 16; k++)
		{
			a.b[k] = Rand(0, 1) < 0.5f;
			a.i[k][0] = (int)Rand(0, 4);
		}
		const bool zero = trial % 4 == 0;
		a.c[kJitterReg][0] = zero ? 0.0f : Rand(-0.001f, 0.001f);
		a.c[kJitterReg][1] = zero ? 0.0f : Rand(-0.001f, 0.001f);
		a.c[kJitterReg][2] = a.c[kJitterReg][3] = 0;
		b = a;
		Interp ia(code, a), ib(out.data(), b);
		bool ra = ia.Run(), rb = ib.Run();
		if (!ra || !rb)
		{
			CHECK(false, "%s: interpreter: %s", name, (!ra ? a.error : b.error).c_str());
			return;
		}
		float(*posA) = info.posType == 6 ? a.o[info.posReg] : a.rast[0];
		float(*posB) = info.posType == 6 ? b.o[info.posReg] : b.rast[0];
		float ex = posA[0] + a.c[kJitterReg][0] * posA[3], ey = posA[1] + a.c[kJitterReg][1] * posA[3];
		bool good = SameFloat(posB[0], ex, 1e-6f) && SameFloat(posB[1], ey, 1e-6f) && SameFloat(posB[2], posA[2], 0) &&
			SameFloat(posB[3], posA[3], 0);
		if (zero)
			good = good && SameFloat(posB[0], posA[0], 0) && SameFloat(posB[1], posA[1], 0);
		CHECK(good, "%s: position (%g %g %g %g) -> (%g %g %g %g), expected (%g %g %g %g)", name, posA[0], posA[1], posA[2], posA[3], posB[0],
			posB[1], posB[2], posB[3], ex, ey, posA[2], posA[3]);
		for (int k = 0; k < 16; k++)
			for (int j = 0; j < 4; j++)
				if (!(info.posType == 6 && k == info.posReg) && !SameFloat(a.o[k][j], b.o[k][j], 0))
				{
					CHECK(false, "%s: output o%d.%c changed", name, k, "xyzw"[j]);
					return;
				}
		if (!good)
			return;
	}
}

static std::vector<DWORD> Compile(const char* src, const char* entry, const char* profile)
{
	ID3DBlob *code = nullptr, *err = nullptr;
	HRESULT hr = g_compile(src, strlen(src), "synthetic", nullptr, nullptr, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
	if (FAILED(hr))
	{
		printf("compile %s failed: %s\n", entry, err ? (const char*)err->GetBufferPointer() : "?");
		exit(2);
	}
	std::vector<DWORD> v((const DWORD*)code->GetBufferPointer(), (const DWORD*)code->GetBufferPointer() + code->GetBufferSize() / 4);
	code->Release();
	return v;
}

static const char* kSynthetic = R"(
float4x4 g_WorldViewProj : register(c0);
float4x4 g_World : register(c8);
float4 g_Extra : register(c12);
float4 g_Bad : register(c255);
bool g_Flag : register(b3);
float4 g_Bones[60] : register(c20);

struct Out { float4 pos : POSITION; float4 clip : TEXCOORD0; float2 uv : TEXCOORD1; };

Out Basic(float4 p : POSITION, float2 uv : TEXCOORD0)
{
	Out o;
	o.pos = mul(p, g_WorldViewProj);
	o.clip = o.pos;
	o.uv = uv * g_Extra.xy;
	return o;
}

Out Branchy(float4 p : POSITION, float2 uv : TEXCOORD0)
{
	Out o;
	float4 w = mul(p, g_World);
	if (g_Flag)
		w.xyz += g_Extra.xyz * sin(w.x);
	o.pos = mul(w, g_WorldViewProj);
	o.clip = w;
	o.uv = uv;
	return o;
}

Out Skinned(float4 p : POSITION, float4 idx : BLENDINDICES, float4 wt : BLENDWEIGHT)
{
	Out o;
	float4 s = 0;
	for (int k = 0; k < 2; k++)
	{
		int i = (int)(idx[k] * 3);
		s.x += dot(p, g_Bones[i]) * wt[k];
		s.y += dot(p, g_Bones[i + 1]) * wt[k];
		s.z += dot(p, g_Bones[i + 2]) * wt[k];
	}
	s.w = 1;
	o.pos = mul(s, g_WorldViewProj);
	o.clip = s;
	o.uv = 0;
	return o;
}

float4 Fullscreen(float4 p : POSITION) : POSITION { return p; }

float4 UsesC255(float4 p : POSITION) : POSITION { return mul(p, g_WorldViewProj) + g_Bad; }
)";

// Hand-assembled vs_3_0 with a subroutine writing the position: main = call l0; ret; l0: dp4 o0...; ret.
static std::vector<DWORD> WithSubroutine()
{
	auto dst = [](int type, int num, int mask) { return 0x80000000u | ((DWORD)(type & 7) << 28) | ((DWORD)(type & 0x18) << 8) | ((DWORD)mask << 16) | (DWORD)num; };
	auto src = [](int type, int num) { return 0x80000000u | ((DWORD)(type & 7) << 28) | ((DWORD)(type & 0x18) << 8) | (0xE4u << 16) | (DWORD)num; };
	std::vector<DWORD> c = { 0xFFFE0300 };
	c.insert(c.end(), { 0x0200001F, 0x80000000, dst(1, 0, 0xF) });  // dcl_position v0
	c.insert(c.end(), { 0x0200001F, 0x80000000, dst(6, 0, 0xF) });  // dcl_position o0
	c.insert(c.end(), { 0x01000019, src(18, 0) });                  // call l0
	c.insert(c.end(), { 0x0000001C });                              // ret
	c.insert(c.end(), { 0x0100001E, src(18, 0) });                  // label l0
	for (int k = 0; k < 4; k++)
		c.insert(c.end(), { 0x03000009, dst(6, 0, 1 << k), src(1, 0), src(2, k) }); // dp4 o0.?, v0, c#
	c.insert(c.end(), { 0x0000001C, 0x0000FFFF });
	return c;
}

int main(int argc, char** argv)
{
	HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
	g_compile = (PFN_Compile)GetProcAddress(dc, "D3DCompile");
	g_disasm = (PFN_Disasm)GetProcAddress(dc, "D3DDisassemble");
	if (!g_compile || !g_disasm)
		return printf("d3dcompiler_47.dll not found\n"), 2;

	// A Direct3D 9 device, if this machine has one (validates the bytecode for real).
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	if (d3d)
	{
		HWND wnd = CreateWindowA("STATIC", "vsjitter", WS_POPUP, 0, 0, 64, 64, NULL, NULL, NULL, NULL);
		D3DPRESENT_PARAMETERS pp = {};
		pp.Windowed = TRUE;
		pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
		pp.BackBufferFormat = D3DFMT_UNKNOWN;
		pp.hDeviceWindow = wnd;
		const D3DDEVTYPE types[] = { D3DDEVTYPE_HAL, D3DDEVTYPE_REF };
		for (D3DDEVTYPE t : types)
			if (!g_dev && SUCCEEDED(d3d->CreateDevice(0, t, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &g_dev)))
				printf("Direct3D 9 device (%s) available: rewritten shaders are created on it too.\n", t == D3DDEVTYPE_HAL ? "HAL" : "REF");
	}
	if (!g_dev)
		printf("No Direct3D 9 device on this machine: bytecode checked by disassembly and interpretation only.\n");

	printf("== synthetic shaders\n");
	TestShader("Basic (vs_3_0)", Compile(kSynthetic, "Basic", "vs_3_0").data(), 1, true);
	TestShader("Branchy (vs_3_0)", Compile(kSynthetic, "Branchy", "vs_3_0").data(), 1, false);
	TestShader("Skinned (vs_3_0)", Compile(kSynthetic, "Skinned", "vs_3_0").data(), 1, false);
	TestShader("Basic (vs_2_0)", Compile(kSynthetic, "Basic", "vs_2_0").data(), 1, true);
	TestShader("Subroutine (vs_3_0)", WithSubroutine().data(), 1, true);
	TestShader("Fullscreen (vs_3_0)", Compile(kSynthetic, "Fullscreen", "vs_3_0").data(), 0, true);
	TestShader("UsesC255 (vs_3_0)", Compile(kSynthetic, "UsesC255", "vs_3_0").data(), 0, true);

	const char* dir = argc > 1 ? argv[1] : "../re/shaders";
	std::string pattern = std::string(dir) + "\\vs_*.cso";
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
	int real = 0, rewritten = 0;
	if (h == INVALID_HANDLE_VALUE)
		printf("== no game shaders in %s (export them with AC2ShadowOverhaul's F8 frame dump); skipped\n", dir);
	else
	{
		printf("== game shaders from %s\n", dir);
		do
		{
			std::string path = std::string(dir) + "\\" + fd.cFileName;
			FILE* f = fopen(path.c_str(), "rb");
			if (!f)
				continue;
			std::vector<DWORD> code(fd.nFileSizeLow / 4 + 1, 0x0000FFFF);
			fread(code.data(), 1, fd.nFileSizeLow, f);
			fclose(f);
			int before = g_failures;
			std::vector<DWORD> out;
			VsJitterInfo info;
			std::string reason;
			bool ok = VsJitter_Rewrite(code.data(), out, info, reason);
			if (!ok)
				printf("%-28s left alone: %s\n", fd.cFileName, reason.c_str());
			TestShader(fd.cFileName, code.data(), -1, false);
			real++;
			rewritten += ok && g_failures == before;
		} while (FindNextFileA(h, &fd));
		FindClose(h);
		printf("%d game shaders: %d rewritten and verified\n", real, rewritten);
	}
	if (g_dev)
		printf("%d rewritten shaders accepted by Direct3D 9\n", g_devChecked);
	printf(g_failures ? "FAILED (%d)\n" : "all passed\n", g_failures);
	return g_failures ? 1 : 0;
}
