// Offline test for the soft shadow shader rewrite, run against the game's real shaders (.cso files
// exported by the F8 frame dump):
//  1. every shadow shader is rewritten at filter widths 1..4 (or the reason it isn't is printed),
//  2. D3DCompiler's disassembler accepts the rewritten bytecode,
//  3. the shadow section (first tap .. our result) is executed by a small ps_3_0 interpreter
//     against a random point-sampled shadow atlas: at width 1 our filter must reproduce the game's
//     own filter result, and at every width it must match a C reference box filter.
// Contact hardening (the PCSS variant, [SoftShadows] ContactHardening=1):
//  4. every shader the plain filter rewrites also gets it; the disassembler accepts it, it stays
//     within the instruction slots and temps left for the lighting rewrite on top of it, and it
//     matches a C reference of the same filter (blocker search, width from the cascade's scale,
//     box of that width) on the random atlas, with random cascade constants and settings;
//  5. on a synthetic scene (a ledge h metres above the ground, its edge across the atlas) the
//     penumbra is measured: it widens with h as h * PenumbraScale metres (clamped to the width
//     limits), and it is the same width in metres in two cascades of different resolution (no
//     seam at a cascade boundary), where the plain filter's width changes with the texel size;
//  6. the plain filter's shaders don't depend on the contact hardening settings (bit-identical).
#include "../src/common.cpp"
#include "../src/core/shaderinfo.cpp"
#include "../src/softshadows.cpp"
#include <stdio.h>
#include <math.h>
#include <string>
#include <vector>
#include <algorithm>

// ---- D3DCompiler disassembler ------------------------------------------------------------------
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
typedef HRESULT(WINAPI* PFN_Disasm)(const void*, SIZE_T, UINT, const char*, Blob**);
static PFN_Disasm g_disasm;

static bool Disassemble(const std::vector<DWORD>& code, std::string& text)
{
	Blob* b = nullptr;
	if (FAILED(g_disasm(code.data(), code.size() * 4, 0, nullptr, &b)) || !b)
		return false;
	text.assign((const char*)b->vt->Ptr(b), b->vt->Size(b));
	b->vt->Release(b);
	return true;
}

// ---- Mini interpreter ----------------------------------------------------------------------------
static const int AW = 128, AH = 32; // atlas: 4 cascades of 32x32
static float g_atlas[AH][AW];
static float g_randomAtlas[AH][AW]; // g_atlas as generated (the contact hardening scene overwrites it)

static float Fetch(float u, float v)
{
	int x = (int)floorf(u * AW), y = (int)floorf(v * AH);
	x = x < 0 ? 0 : x >= AW ? AW - 1 : x;
	y = y < 0 ? 0 : y >= AH ? AH - 1 : y;
	return g_atlas[y][x];
}

struct Machine
{
	float r[32][4], c[256][4], v[16][4] = {};
	float lastGameLrp = NAN;
};

static void ReadSrc(Machine& m, DWORD t, float out[4])
{
	int type = RegType(t), n = RegNum(t);
	const float* v = type == REG_TEMP ? m.r[n] : type == REG_CONST ? m.c[n] : type == REG_INPUT && n < 16 ? m.v[n] : nullptr;
	static const float zero[4] = {};
	if (!v)
		v = zero;
	for (int k = 0; k < 4; k++)
		out[k] = v[SwzComp(t, k)];
	int mod = SrcMod(t);
	for (int k = 0; k < 4; k++)
	{
		if (mod == 0xB || mod == 0xC)
			out[k] = fabsf(out[k]);
		if (mod == 1 || mod == 0xC)
			out[k] = -out[k];
	}
}

// Executes [from, to) of the token stream. Returns false on an unsupported instruction.
static bool Run(Machine& m, const std::vector<DWORD>& code, size_t from, size_t to, const SoftShadowsLayout& L, std::string& err)
{
	for (size_t i = from; i < to;)
	{
		DWORD tok = code[i];
		int op = (int)(tok & 0xFFFF);
		size_t len = op == OP_COMMENT ? ((tok >> 16) & 0x7FFF) : ((tok >> 24) & 0xF);
		const DWORD* p = &code[i + 1];
		i += 1 + len;
		if (op == OP_COMMENT || op == OP_DEF || op == OP_DCL || op == OP_NOP)
			continue;
		float a[4] = {}, b[4] = {}, cc[4] = {}, res[4] = {};
		if (len >= 2)
			ReadSrc(m, p[1], a);
		if (len >= 3)
			ReadSrc(m, p[2], b);
		if (len >= 4)
			ReadSrc(m, p[3], cc);
		switch (op)
		{
		case OP_MOV: for (int k = 0; k < 4; k++) res[k] = a[k]; break;
		case OP_ADD: for (int k = 0; k < 4; k++) res[k] = a[k] + b[k]; break;
		case OP_MUL: for (int k = 0; k < 4; k++) res[k] = a[k] * b[k]; break;
		case OP_MAD: for (int k = 0; k < 4; k++) res[k] = a[k] * b[k] + cc[k]; break;
		case OP_MIN: for (int k = 0; k < 4; k++) res[k] = a[k] < b[k] ? a[k] : b[k]; break;
		case OP_MAX: for (int k = 0; k < 4; k++) res[k] = a[k] > b[k] ? a[k] : b[k]; break;
		case OP_FRC: for (int k = 0; k < 4; k++) res[k] = a[k] - floorf(a[k]); break;
		case OP_RCP: for (int k = 0; k < 4; k++) res[k] = 1.0f / a[0]; break;
		case OP_RSQ: for (int k = 0; k < 4; k++) res[k] = 1.0f / sqrtf(fabsf(a[0])); break;
		case OP_CMP: for (int k = 0; k < 4; k++) res[k] = a[k] >= 0 ? b[k] : cc[k]; break;
		case OP_LRP: for (int k = 0; k < 4; k++) res[k] = a[k] * (b[k] - cc[k]) + cc[k]; break;
		case 0x08: res[0] = res[1] = res[2] = res[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; break;
		case 0x09: res[0] = res[1] = res[2] = res[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; break;
		case OP_TEXLD:
		case OP_TEXLDL:
			if (RegNum(p[2]) != L.sampler)
			{
				err = "other texture sampled in region";
				return false;
			}
			res[0] = Fetch(a[0], a[1]);
			res[1] = res[2] = res[3] = 1.0f;
			break;
		default:
			char buf[64];
			snprintf(buf, sizeof(buf), "unsupported opcode 0x%X", op);
			err = buf;
			return false;
		}
		DWORD d = p[0];
		if (RegType(d) != REG_TEMP)
		{
			err = "non-temp destination in region";
			return false;
		}
		bool sat = ((d >> 20) & 1) != 0;
		for (int k = 0; k < 4; k++)
			if (DstMask(d) & (1 << k))
				m.r[RegNum(d)][k] = sat ? (res[k] < 0 ? 0 : res[k] > 1 ? 1 : res[k]) : res[k];
		if (op == OP_LRP && RegNum(d) == L.outReg && (DstMask(d) & (1 << L.outComp)))
			m.lastGameLrp = m.r[L.outReg][L.outComp];
	}
	return true;
}

// Reference: box filter of N texels centred like the game's 1-texel filter, taps clamped to the tile.
static float Reference(float u, float v, float depth, int N)
{
	float x = u * AW, y = v * AH;
	float ax = x + 0.5f - N * 0.5f, ay = y + 0.5f - N * 0.5f;
	float fx = ax - floorf(ax), fy = ay - floorf(ay), bx = floorf(ax), by = floorf(ay);
	float tileLo = floorf(u * 4) / 4, lo = tileLo + 0.5f / AW, hi = tileLo + 0.25f - 0.5f / AW;
	float sum = 0;
	for (int j = 0; j <= N; j++)
		for (int i = 0; i <= N; i++)
		{
			float su = (bx + i + 0.5f) / AW;
			su = su < lo ? lo : su > hi ? hi : su;
			float sv = (by + j + 0.5f) / AH;
			float lit = Fetch(su, sv) - depth >= 0 ? 1.0f : 0.0f;
			float wx = i == 0 ? 1 - fx : i == N ? fx : 1, wy = j == 0 ? 1 - fy : j == N ? fy : 1;
			if (N == 1)
				wx = i == 0 ? 1 - fx : fx, wy = j == 0 ? 1 - fy : fy;
			sum += wx * wy * lit;
		}
	return sum / (N * N);
}

static uint32_t g_rng = 12345;
static float Rand() { return (g_rng = g_rng * 1664525u + 1013904223u) / 4294967296.0f; }

// ---- Contact hardening -----------------------------------------------------------------------

static size_t TokenLen(DWORD tk)
{
	int op = tk & 0xFFFF;
	return op == OP_COMMENT ? ((tk >> 16) & 0x7FFF) : ((tk >> 24) & 0xF);
}

// The inserted filter in the rewritten code: from where the first tap was (after our defs) to just
// past our mov after the game's lrp. 0 if not found.
static bool FilterRegion(const std::vector<DWORD>& code, const std::vector<DWORD>& out, const SoftShadowsLayout& L, size_t& start, size_t& end)
{
	start = L.insertAt + (L.pcss ? 4 : 3) * 6;
	size_t lrpLen = 1 + ((code[L.outputAt] >> 24) & 0xF);
	for (size_t k = start; k + lrpLen + 2 < out.size(); k++)
		if (memcmp(&out[k], &code[L.outputAt], lrpLen * 4) == 0)
		{
			end = k + lrpLen + 3; // lrp + our mov (3 tokens)
			return true;
		}
	return false;
}

// The game's per-cascade shadow constants (components = cascades).
struct Cascades
{
	float sx[4], ox[4], nf[2];
};

// Runs the inserted filter alone for one sample: the uv and receiver depth it reads, the atlas size
// and cascade constants. Every other register is random, so a filter that read anything else (or a
// temp before writing it) would not match its reference.
static bool RunFilter(const std::vector<DWORD>& out, size_t start, size_t end, const SoftShadowsLayout& L, const Cascades& cs, float u, float v,
	float depth, float& result, std::string& err)
{
	Machine m;
	for (int a = 0; a < 32; a++)
		for (int k = 0; k < 4; k++)
			m.r[a][k] = Rand() * 2 - 1;
	for (int a = 0; a < 256; a++)
		for (int k = 0; k < 4; k++)
			m.c[a][k] = Rand();
	for (size_t k = 1; k < out.size() && out[k] != 0xFFFF; k += 1 + TokenLen(out[k]))
		if ((out[k] & 0xFFFF) == OP_DEF)
			memcpy(m.c[RegNum(out[k + 1])], &out[k + 2], 16);
	m.c[L.sizeConst][0] = AW;
	m.c[L.sizeConst][1] = AH;
	for (int k = 0; k < 4; k++)
	{
		m.c[L.scalesX][k] = cs.sx[k];
		m.c[L.offsetsX][k] = cs.ox[k];
	}
	m.c[L.nearFar][0] = cs.nf[0];
	m.c[L.nearFar][1] = cs.nf[1];
	m.r[L.uvReg][L.uvComp[0]] = u;
	m.r[L.uvReg][L.uvComp[1]] = v;
	if (L.depthType == REG_TEMP)
		m.r[L.depthReg][L.depthComp] = depth;
	else
		m.v[L.depthReg][L.depthComp] = depth;
	if (!Run(m, out, start, end, L, err))
		return false;
	result = m.r[L.tempBase + 9][0];
	if (m.r[L.outReg][L.outComp] != result)
	{
		err = "result not moved over the game's";
		return false;
	}
	return true;
}

static float Sat(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

// C reference of the contact hardening filter (see softshadows.cpp).
static float PcssReference(float u, float v, float depth, const Cascades& cs, const SoftShadowsPcss& p, float* widthOut = nullptr)
{
	float x = u * AW - 1.5f, y = v * AH - 1.5f;
	float bx = floorf(x), by = floorf(y), fx = x - bx, fy = y - by;
	float tileLo = floorf(u * 4) / 4, lo = tileLo + 0.5f / AW, hi = tileLo + 0.25f - 0.5f / AW;
	float B[5][5], sum = 0, count = 0;
	for (int j = 0; j <= 4; j++)
		for (int i = 0; i <= 4; i++)
		{
			float su = (bx + i + 0.5f) / AW;
			su = su < lo ? lo : su > hi ? hi : su;
			float d = Fetch(su, (by + j + 0.5f) / AH) - depth;
			B[j][i] = d < 0 ? 1.0f : 0.0f;
			if (d < 0)
				sum -= d, count++;
		}
	float metres = (count ? sum / count : 0) / fabsf(cs.nf[0]);
	float tile = floorf(u * 4), scale = 0;
	for (int k = 0; k < 4; k++)
		if (floorf(cs.ox[k] * 4 + 0.125f) == tile)
			scale += cs.sx[k];
	float w = metres * fabsf(scale) * AW * p.penumbraScale;
	w = fminf(fmaxf(w, fminf(p.minWidth, p.maxWidth)), p.maxWidth);
	if (widthOut)
		*widthOut = w;
	float lx = fx + 2 - w / 2, ly = fy + 2 - w / 2, blocked = 0;
	for (int j = 0; j <= 4; j++)
		for (int i = 0; i <= 4; i++)
			blocked += (Sat(lx + w - i) - Sat(lx - i)) * (Sat(ly + w - j) - Sat(ly - j)) * B[j][i];
	return Sat(1 - blocked / (w * w));
}

// The synthetic scene: ground at depth `ground` everywhere, and a ledge h metres above it (closer
// to the light: depth ground + h * nf.x, nf.x < 0) over world x < 0. Every tile holds the scene as
// its cascade maps it (u = x * sx[k] + ox[k], offsets at the tiles' centres).
static const float kGround = 0.8f;
static void FillLedge(const Cascades& cs, float h)
{
	for (int y = 0; y < AH; y++)
		for (int x = 0; x < AW; x++)
		{
			int k = x / (AW / 4);
			float wx = ((x + 0.5f) / AW - cs.ox[k]) / cs.sx[k];
			g_atlas[y][x] = wx < 0 ? kGround + h * cs.nf[0] : kGround;
		}
}

// Penumbra width in metres in cascade k: how far apart world x is where the ground under the
// ledge's edge is 10% and 90% lit (the shadow fades linearly across a box filter's width).
static bool MeasurePenumbra(const std::vector<DWORD>& out, size_t start, size_t end, const SoftShadowsLayout& L, const Cascades& cs, int k,
	float& width, std::string& err)
{
	const float step = 0.25f / (cs.sx[k] * AW) / 8; // 1/32 texel
	float prevX = 0, prev = 0, x10 = NAN, x90 = NAN;
	for (float x = -1.2f; x <= 1.2f; x += step)
	{
		float lit;
		if (!RunFilter(out, start, end, L, cs, x * cs.sx[k] + cs.ox[k], 0.5f, kGround, lit, err))
			return false;
		if (x > -1.2f)
		{
			if (prev < 0.1f && lit >= 0.1f && x10 != x10)
				x10 = prevX + (0.1f - prev) / (lit - prev) * (x - prevX);
			if (prev < 0.9f && lit >= 0.9f && x90 != x90)
				x90 = prevX + (0.9f - prev) / (lit - prev) * (x - prevX);
		}
		prevX = x;
		prev = lit;
	}
	if (x10 != x10 || x90 != x90)
	{
		err = "no shadow edge found";
		return false;
	}
	width = (x90 - x10) / 0.8f;
	return true;
}

int main(int argc, char** argv)
{
	const char* dir = argc > 1 ? argv[1] : "../re/shaders/dump";
	LogInit("softshadow_test.log", true);
	HMODULE dc = LoadLibraryA("D3DCompiler_43.dll");
	g_disasm = dc ? (PFN_Disasm)GetProcAddress(dc, "D3DDisassemble") : nullptr;
	if (!g_disasm)
	{
		printf("D3DCompiler_43 not available\n");
		return 1;
	}
	for (int y = 0; y < AH; y++)
		for (int x = 0; x < AW; x++)
			g_atlas[y][x] = Rand();
	memcpy(g_randomAtlas, g_atlas, sizeof(g_atlas));

	char pattern[MAX_PATH];
	snprintf(pattern, sizeof(pattern), "%s\\*.cso", dir);
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE)
	{
		printf("no .cso files in %s\n", dir);
		return 1;
	}
	int shaders = 0, rewritten = 0, failures = 0, pcssOk = 0;
	do
	{
		char path[MAX_PATH];
		snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
		FILE* f = fopen(path, "rb");
		std::vector<DWORD> code(fd.nFileSizeLow / 4 + 1, 0);
		fread(code.data(), 1, fd.nFileSizeLow, f);
		fclose(f);
		shaders++;

		std::string line;
		bool ok = true, any = false;
		for (int N = 1; N <= 4 && ok; N++)
		{
			std::vector<DWORD> out;
			std::string reason;
			if (!SoftShadows_Rewrite(code.data(), N, out, reason))
			{
				line = "skipped: " + reason;
				break;
			}
			any = true;
			SoftShadowsLayout L = g_softShadowsLastLayout;
			{
				auto count = [](const std::vector<DWORD>& c) {
					int n = 0;
					for (size_t k = 1; k < c.size() && c[k] != 0xFFFF;)
					{
						int op = c[k] & 0xFFFF;
						size_t len = op == OP_COMMENT ? ((c[k] >> 16) & 0x7FFF) : ((c[k] >> 24) & 0xF);
						if (op != OP_COMMENT && op != OP_DEF && op != OP_DCL && op != OP_DEFI && op != OP_DEFB)
							n++;
						k += 1 + len;
					}
					return n;
				};
				static int maxSlots[5] = {};
				int n = count(out);
				if (n > maxSlots[N])
				{
					maxSlots[N] = n;
					printf("  [w%d max instructions so far: %d (original %d)]\n", N, n, count(code));
				}
			}
			std::string text;
			if (!Disassemble(out, text) || text.find("error") != std::string::npos)
			{
				line += " disasm FAILED";
				ok = false;
				break;
			}
			if (N == 3 && strncmp(fd.cFileName, "81DFCBE3", 8) == 0)
			{
				FILE* o = fopen("softshadow_81DFCBE3_w3.txt", "w");
				fwrite(text.data(), 1, text.size(), o);
				fclose(o);
			}
			// Region in the rewritten code: inserted block starts where the first tap was, shifted by
			// the 3 defs (6 tokens each); run until just past our mov after the game's lrp.
			size_t start = L.insertAt + 18;
			size_t lrpNew = start + (out.size() - code.size()); // placeholder, recomputed below
			(void)lrpNew;
			// Find the end: our mov is the instruction right after the game's lrp (same tokens as original).
			size_t lrpLen = 1 + ((code[L.outputAt] >> 24) & 0xF);
			size_t inserted = out.size() - (code.size()) ; (void)inserted;
			// Locate by scanning for the original lrp tokens after start.
			size_t endRun = 0;
			for (size_t k = start; k + lrpLen + 2 < out.size(); k++)
				if (memcmp(&out[k], &code[L.outputAt], lrpLen * 4) == 0)
				{
					endRun = k + lrpLen + 3; // lrp + our mov (3 tokens)
					break;
				}
			if (!endRun)
			{
				line += " lrp not found in output";
				ok = false;
				break;
			}

			size_t uvWriteEnd = 0;
			for (size_t k = 1; k < start;)
			{
				DWORD tk = out[k];
				int op = tk & 0xFFFF;
				size_t len = op == OP_COMMENT ? ((tk >> 16) & 0x7FFF) : ((tk >> 24) & 0xF);
				if (len >= 1 && op != OP_COMMENT && op != OP_DEF && op != OP_DCL && RegType(out[k + 1]) == REG_TEMP &&
					RegNum(out[k + 1]) == L.uvReg && (DstMask(out[k + 1]) & ((1 << L.uvComp[0]) | (1 << L.uvComp[1]))))
					uvWriteEnd = k + 1 + len;
				k += 1 + len;
			}
			if (!uvWriteEnd)
				uvWriteEnd = start;
			int mism = 0, refBad = 0, trials = 4000;
			float worst = 0;
			for (int trial = 0; trial < trials; trial++)
			{
				Machine m;
				for (int a = 0; a < 32; a++)
					for (int k = 0; k < 4; k++)
						m.r[a][k] = Rand() * 2 - 1;
				for (int a = 0; a < 256; a++)
					for (int k = 0; k < 4; k++)
						m.c[a][k] = Rand();
				// defs
				for (size_t k = 1; k < out.size();)
				{
					DWORD tk = out[k];
					int op = tk & 0xFFFF;
					size_t len = op == OP_COMMENT ? ((tk >> 16) & 0x7FFF) : ((tk >> 24) & 0xF);
					if (tk == 0xFFFF)
						break;
					if (op == OP_DEF)
						memcpy(m.c[RegNum(out[k + 1])], &out[k + 2], 16);
					k += 1 + len;
				}
				m.c[L.sizeConst][0] = AW;
				m.c[L.sizeConst][1] = AH;
				// Run constant-only instructions before the region (e.g. "rcp r0.x, c86.x" at the top of
				// the shader, which the game's tap offsets use), then start right after the instruction
				// that produced the filter uv, so both filters see the same real inputs.
				std::string err;
				for (size_t k = 1; k < uvWriteEnd;)
				{
					DWORD tk = out[k];
					int op = tk & 0xFFFF;
					size_t len = op == OP_COMMENT ? ((tk >> 16) & 0x7FFF) : ((tk >> 24) & 0xF);
					bool constOnly = len >= 2 && op != OP_DEF && op != OP_DCL && op != OP_COMMENT && RegType(out[k + 1]) == REG_TEMP;
					for (size_t q = 2; constOnly && q <= len; q++)
						constOnly = RegType(out[k + q]) == REG_CONST;
					if (constOnly)
						Run(m, out, k, k + 1 + len, L, err);
					k += 1 + len;
				}
				int tile = (int)(Rand() * 4);
				float margin = trial < trials / 2 ? 6.0f / AW : 0.0f;
				float u = tile * 0.25f + margin + Rand() * (0.25f - 2 * margin);
				float v = 6.0f / AH + Rand() * (1 - 12.0f / AH);
				float depth = Rand();
				m.r[L.uvReg][L.uvComp[0]] = u;
				m.r[L.uvReg][L.uvComp[1]] = v;
				if (L.depthType == REG_TEMP)
					m.r[L.depthReg][L.depthComp] = depth;
				if (!Run(m, out, uvWriteEnd, start, L, err))
				{
					line += " interp(pre): " + err;
					ok = false;
					break;
				}
				// The values the filters actually see at the insertion point.
				u = m.r[L.uvReg][L.uvComp[0]];
				v = m.r[L.uvReg][L.uvComp[1]];
				if (L.depthType == REG_TEMP)
					depth = m.r[L.depthReg][L.depthComp];
				if (!Run(m, out, start, endRun, L, err))
				{
					line += " interp: " + err;
					ok = false;
					break;
				}
				float ours = m.r[L.tempBase + 9][0], game = m.lastGameLrp, final_ = m.r[L.outReg][L.outComp];
				float ref = Reference(u, v, depth, N);
				if (final_ != ours)
					refBad++;
				if (fabsf(ours - ref) > 1e-4f)
				{
					refBad++;
					worst = fmaxf(worst, fabsf(ours - ref));
				}
				if (N == 1 && margin > 0 && fabsf(ours - game) > 1e-4f)
					mism++;
			}
			char buf[160];
			snprintf(buf, sizeof(buf), " w%d:%s%s", N, refBad ? "REF-FAIL" : "ok", (N == 1 && mism) ? "(game-mismatch)" : "");
			line += buf;
			if (refBad || mism)
			{
				snprintf(buf, sizeof(buf), "[ref bad %d worst %.3f, game mismatch %d]", refBad, worst, mism);
				line += buf;
				ok = false;
			}
		}
		// ---- Contact hardening ----
		if (any && ok)
		{
			std::vector<DWORD> plainBefore, plainAfter, pc;
			std::string why;
			SoftShadows_Rewrite(code.data(), 3, plainBefore, why);
			const SoftShadowsPcss defaults;
			if (!SoftShadows_RewritePcss(code.data(), defaults, pc, why))
			{
				line += " pcss:FAILED (" + why + ")";
				ok = false;
			}
			SoftShadowsLayout L = g_softShadowsLastLayout;
			std::string text, err;
			size_t start = 0, end = 0;
			if (ok && (!Disassemble(pc, text) || text.find("error") != std::string::npos))
			{
				line += " pcss:disasm FAILED";
				ok = false;
			}
			if (ok && !FilterRegion(code, pc, L, start, end))
			{
				line += " pcss:lrp not found in output";
				ok = false;
			}
			if (ok)
			{
				// Slots, and temps: at most 10 more, and 3 left for the lighting rewrite.
				int slots = SoftShadows_InstructionSlots(pc), maxTemp = -1;
				for (size_t k = 1; k < pc.size() && pc[k] != 0xFFFF; k += 1 + TokenLen(pc[k]))
				{
					int op = pc[k] & 0xFFFF;
					if (op == OP_COMMENT || op == OP_DEF || op == OP_DCL)
						continue;
					for (size_t q = 1; q <= TokenLen(pc[k]); q++)
						if ((pc[k + q] & 0x80000000u) && RegType(pc[k + q]) == REG_TEMP)
							maxTemp = std::max(maxTemp, RegNum(pc[k + q]));
				}
				static int maxSlots = 0;
				if (slots > maxSlots)
				{
					maxSlots = slots;
					printf("  [pcss max instruction slots so far: %d (w4 %d)]\n", slots, SoftShadows_InstructionSlots(plainBefore));
				}
				if (slots > 512 - 48 || maxTemp > L.tempBase + 9 || maxTemp > 28)
				{
					char buf[96];
					snprintf(buf, sizeof(buf), " pcss:TOO BIG (%d slots, r%d)", slots, maxTemp);
					line += buf;
					ok = false;
				}
			}
			// The C reference on the random atlas: random cascade constants (the cascades' order in
			// the atlas shuffled; offsets at the tiles' centres or starts) and settings.
			int refBad = 0;
			float worst = 0, wMin = 9, wMax = 0;
			for (int set = 0; set < 4 && ok; set++)
			{
				SoftShadowsPcss p;
				p.penumbraScale = set == 0 ? defaults.penumbraScale : 0.005f + Rand() * 0.3f;
				p.minWidth = set == 0 ? defaults.minWidth : 1 + Rand() * 3;
				p.maxWidth = set == 0 ? defaults.maxWidth : 1 + Rand() * 3;
				std::vector<DWORD> o;
				if (!SoftShadows_RewritePcss(code.data(), p, o, why) || !FilterRegion(code, o, g_softShadowsLastLayout, start, end))
				{
					line += " pcss:settings rewrite FAILED";
					ok = false;
					break;
				}
				const SoftShadowsLayout& Lp = g_softShadowsLastLayout;
				for (int trial = 0; trial < 1500 && ok; trial++)
				{
					Cascades cs;
					int order[4] = { 0, 1, 2, 3 };
					for (int k = 3; k > 0; k--)
						std::swap(order[k], order[(int)(Rand() * (k + 1)) % (k + 1)]);
					bool centres = Rand() < 0.5f;
					for (int k = 0; k < 4; k++)
					{
						cs.sx[k] = (0.5f + Rand() * 7) / AW * (Rand() < 0.5f ? -1 : 1);
						cs.ox[k] = (order[k] + (centres ? 0.5f : 0.0f)) * 0.25f + (Rand() - 0.5f) * 2.0f / AW;
					}
					cs.nf[0] = (0.005f + Rand() * 0.05f) * (Rand() < 0.5f ? -1 : 1);
					cs.nf[1] = Rand();
					int tile = (int)(Rand() * 4);
					float margin = 6.0f / AW;
					float u = tile * 0.25f + margin + Rand() * (0.25f - 2 * margin);
					float v = 6.0f / AH + Rand() * (1 - 12.0f / AH);
					float depth = Rand(), ours, w;
					if (!RunFilter(o, start, end, Lp, cs, u, v, depth, ours, err))
					{
						line += " pcss:interp: " + err;
						ok = false;
						break;
					}
					float ref = PcssReference(u, v, depth, cs, SoftShadows_ClampPcss(p), &w);
					wMin = std::min(wMin, w);
					wMax = std::max(wMax, w);
					if (!(fabsf(ours - ref) <= 2e-4f))
					{
						refBad++;
						worst = std::max(worst, fabsf(ours - ref));
					}
				}
			}
			if (ok && refBad)
			{
				char buf[96];
				snprintf(buf, sizeof(buf), " pcss:REF-FAIL[%d, worst %.4f]", refBad, worst);
				line += buf;
				ok = false;
			}
			// The ledge: penumbra width against its height, in two cascades (8 and 4 texels per
			// metre, the second next to the first in the atlas).
			if (ok)
			{
				SoftShadowsPcss p;
				p.penumbraScale = 0.1f;
				p.minWidth = 1;
				p.maxWidth = 4;
				std::vector<DWORD> o;
				SoftShadows_RewritePcss(code.data(), p, o, why);
				const SoftShadowsLayout Lp = g_softShadowsLastLayout;
				FilterRegion(code, o, Lp, start, end);
				Cascades cs;
				const float tpm[4] = { 8, 4, 2, 1 };
				for (int k = 0; k < 4; k++)
				{
					cs.sx[k] = tpm[k] / AW;
					cs.ox[k] = 0.125f + 0.25f * k;
				}
				cs.nf[0] = -0.02f;
				cs.nf[1] = 0.5f;
				const float heights[] = { 0.5f, 1, 2, 3, 4, 10 };
				float prevW = 0, w3[2] = {};
				std::string growth;
				for (float h : heights)
				{
					FillLedge(cs, h);
					for (int k = 0; k < 2 && ok; k++)
					{
						if (k == 1 && h != 3)
							continue;
						float width;
						if (!MeasurePenumbra(o, start, end, Lp, cs, k, width, err))
						{
							line += " pcss:ledge: " + err;
							ok = false;
							break;
						}
						// Expected: h * scale metres, clamped to 1..4 texels of this cascade.
						float expect = std::min(std::max(h * p.penumbraScale, p.minWidth / tpm[k]), p.maxWidth / tpm[k]);
						if (fabsf(width - expect) > 0.01f + 0.02f * expect)
						{
							char buf[96];
							snprintf(buf, sizeof(buf), " pcss:PENUMBRA h=%.1f c%d %.3f m (expected %.3f)", h, k, width, expect);
							line += buf;
							ok = false;
						}
						if (k == 0)
						{
							if (width < prevW - 0.005f)
								ok = false, line += " pcss:PENUMBRA SHRINKS";
							prevW = width;
							char buf[32];
							snprintf(buf, sizeof(buf), "%s%.2f", growth.empty() ? "" : "/", width);
							growth += buf;
						}
						if (h == 3)
							w3[k] = width;
					}
				}
				memcpy(g_atlas, g_randomAtlas, sizeof(g_atlas));
				if (ok && fabsf(w3[0] - w3[1]) > 0.01f)
				{
					char buf[96];
					snprintf(buf, sizeof(buf), " pcss:SEAM (%.3f m in cascade 0, %.3f m in cascade 1)", w3[0], w3[1]);
					line += buf;
					ok = false;
				}
				if (ok)
				{
					char buf[160];
					snprintf(buf, sizeof(buf), " pcss:ok (widths %.2f-%.2f texels; ledge 0.5..10 m: %s m; cascades 0/1 at 3 m: %.3f/%.3f m)", wMin,
						wMax, growth.c_str(), w3[0], w3[1]);
					line += buf;
				}
			}
			// The plain filter doesn't depend on the contact hardening settings.
			SoftShadowsPcss other;
			other.penumbraScale = 0.3f;
			other.minWidth = 2;
			other.maxWidth = 3;
			SoftShadows_SetContactHardening(true);
			SoftShadows_SetPcss(other);
			SoftShadows_PcssBuilt(nullptr);
			SoftShadows_Rewrite(code.data(), 3, plainAfter, why);
			SoftShadows_SetContactHardening(false);
			if (plainAfter != plainBefore)
			{
				line += " PLAIN FILTER CHANGED BY CONTACT HARDENING SETTINGS";
				ok = false;
			}
			if (ok)
				pcssOk++;
		}
		if (any)
			rewritten++;
		if (!ok)
			failures++;
		printf("%.8s %s\n", fd.cFileName, line.c_str());
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	printf("\n%d shaders, %d rewritten (%d with contact hardening), %d with failures\n", shaders, rewritten, pcssOk, failures);
	return failures ? 3 : 0;
}
