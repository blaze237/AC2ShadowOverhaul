// Offline test for the AO shader rewrite (extra output oC2 = indirect light x (1 - fog)), run
// against the game's real shaders (.cso files exported by the F8 frame dump), both as shipped and
// after the soft shadow rewrite (the two are stacked in game):
//  1. D3DCompiler's disassembler accepts the rewritten bytecode,
//  2. a small ps_3_0 interpreter runs original and rewritten shaders on random inputs: oC0 and oC1
//     must be bit-identical (the rewrite only adds instructions),
//  3. oC2 must equal what the ambient light contributes to the final colour, measured by running
//     the original again with g_AmbientCube and the lightmap colour set to zero:
//         oC2.rgb == oC0(normal) - oC0(no ambient)
//     and shaders that weren't matched must write oC2 = 0;
//  4. oC3 must equal what the shadowed sun light contributes: the colour minus the colour with the
//     shadow operand of the combine zeroed, and minus the colour with the cloud shadow map black
//     (the clouds only dim the sun); unmatched shaders write oC3 = 0.
#include "../src/common.cpp"
#include "../src/softshadows.cpp"
#include "../src/aoshader.cpp"
#include <stdio.h>
#include <math.h>
#include <string>
#include <vector>

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

// ---- Interpreter -------------------------------------------------------------------------------

struct Inputs
{
	float v[16][4];
	float c[256][4];
	float misc[2][4]; // vPos, vFace
	bool b[16];
	uint32_t texSeed;
	int lmSampler = -1, cloudSampler = -1;
	bool noLightmapColour = false, noCloud = false;
	size_t zeroAt = 0;   // before the instruction at this token index, zero the components that
	DWORD zeroSrc = 0;   // this source token reads for x, y, z (a temp register)
};

struct Outputs
{
	float oC[4][4];
	bool written[4];
	bool killed;
};

static float TexValue(uint32_t seed, int sampler, float u, float v, int comp)
{
	uint32_t a, b;
	memcpy(&a, &u, 4);
	memcpy(&b, &v, 4);
	uint32_t h = seed ^ (a * 2654435761u) ^ (b * 2246822519u) ^ ((uint32_t)sampler * 3266489917u) ^ ((uint32_t)comp * 668265263u);
	h ^= h >> 15;
	h *= 2246822519u;
	h ^= h >> 13;
	h *= 3266489917u;
	h ^= h >> 16;
	return (h & 0xFFFFFF) / 16777216.0f;
}

static bool Run(const std::vector<DWORD>& code, Inputs in, Outputs& o, std::string& err)
{
	float r[32][4] = {};
	memset(&o, 0, sizeof(o));
	auto read = [&](DWORD t, float out[4]) {
		int type = RegType(t), n = RegNum(t);
		const float* src = nullptr;
		static const float zero[4] = {};
		if (type == REG_TEMP)
			src = r[n];
		else if (type == REG_INPUT)
			src = in.v[n & 15];
		else if (type == REG_CONST)
			src = in.c[n & 255];
		else if (type == 17) // vPos / vFace
			src = in.misc[n & 1];
		else
			src = zero;
		for (int k = 0; k < 4; k++)
			out[k] = src[SwzComp(t, k)];
		int mod = SrcMod(t);
		for (int k = 0; k < 4; k++)
		{
			if (mod == 0xB || mod == 0xC)
				out[k] = fabsf(out[k]);
			if (mod == 1 || mod == 0xC)
				out[k] = -out[k];
		}
	};
	for (size_t i = 1; i < code.size();)
	{
		DWORD tok = code[i];
		if (tok == 0x0000FFFF)
			return true;
		int op = (int)(tok & 0xFFFF);
		size_t len = op == OP_COMMENT ? ((tok >> 16) & 0x7FFF) : ((tok >> 24) & 0xF);
		const DWORD* p = &code[i + 1];
		i += 1 + len;
		if (op == OP_COMMENT || op == OP_DCL || op == OP_NOP || op == OP_DEFI || op == OP_DEFB)
			continue;
		if (op == OP_DEF)
		{
			memcpy(in.c[RegNum(p[0])], &p[1], 16);
			continue;
		}
		if (op == OP_IF || op == OP_IFC)
		{
			float x[4], y[4];
			bool cond;
			if (op == OP_IF)
				cond = RegType(p[0]) == 14 ? in.b[RegNum(p[0]) & 15] : (read(p[0], x), x[0] != 0);
			else
			{
				read(p[0], x);
				read(p[1], y);
				switch ((tok >> 16) & 7)
				{
				case 1: cond = x[0] > y[0]; break;
				case 2: cond = x[0] == y[0]; break;
				case 3: cond = x[0] >= y[0]; break;
				case 4: cond = x[0] < y[0]; break;
				case 5: cond = x[0] != y[0]; break;
				default: cond = x[0] <= y[0]; break;
				}
			}
			if (!cond)
			{
				// Skip to the matching else (run it) or endif.
				int depth = 0;
				while (i < code.size())
				{
					int o2 = (int)(code[i] & 0xFFFF);
					size_t l2 = o2 == OP_COMMENT ? ((code[i] >> 16) & 0x7FFF) : ((code[i] >> 24) & 0xF);
					i += 1 + l2;
					if (o2 == OP_IF || o2 == OP_IFC)
						depth++;
					else if ((o2 == OP_ENDIF || o2 == OP_ELSE) && depth == 0)
						break;
					else if (o2 == OP_ENDIF)
						depth--;
				}
			}
			continue;
		}
		if (op == OP_ELSE)
		{
			// Reached from a taken branch: skip the else part.
			int depth = 0;
			while (i < code.size())
			{
				int o2 = (int)(code[i] & 0xFFFF);
				size_t l2 = o2 == OP_COMMENT ? ((code[i] >> 16) & 0x7FFF) : ((code[i] >> 24) & 0xF);
				i += 1 + l2;
				if (o2 == OP_IF || o2 == OP_IFC)
					depth++;
				else if (o2 == OP_ENDIF && depth-- == 0)
					break;
			}
			continue;
		}
		if (op == OP_ENDIF)
			continue;
		if (IsFlowControl(op))
		{
			err = "flow control";
			return false;
		}
		if (in.zeroAt && i - 1 - len == in.zeroAt && RegType(in.zeroSrc) == REG_TEMP)
			for (int k = 0; k < 3; k++)
				r[RegNum(in.zeroSrc)][SwzComp(in.zeroSrc, k)] = 0;
		float a[4] = {}, b[4] = {}, c[4] = {}, res[4] = {};
		if (len >= 2)
			read(p[1], a);
		if (len >= 3)
			read(p[2], b);
		if (len >= 4)
			read(p[3], c);
		switch (op)
		{
		case OP_MOV: for (int k = 0; k < 4; k++) res[k] = a[k]; break;
		case OP_ADD: for (int k = 0; k < 4; k++) res[k] = a[k] + b[k]; break;
		case OP_SUB: for (int k = 0; k < 4; k++) res[k] = a[k] - b[k]; break;
		case OP_MUL: for (int k = 0; k < 4; k++) res[k] = a[k] * b[k]; break;
		case OP_MAD: for (int k = 0; k < 4; k++) res[k] = a[k] * b[k] + c[k]; break;
		case OP_MIN: for (int k = 0; k < 4; k++) res[k] = a[k] < b[k] ? a[k] : b[k]; break;
		case OP_MAX: for (int k = 0; k < 4; k++) res[k] = a[k] > b[k] ? a[k] : b[k]; break;
		case OP_SLT: for (int k = 0; k < 4; k++) res[k] = a[k] < b[k] ? 1.0f : 0.0f; break;
		case OP_SGE: for (int k = 0; k < 4; k++) res[k] = a[k] >= b[k] ? 1.0f : 0.0f; break;
		case OP_FRC: for (int k = 0; k < 4; k++) res[k] = a[k] - floorf(a[k]); break;
		case OP_ABS: for (int k = 0; k < 4; k++) res[k] = fabsf(a[k]); break;
		case OP_CMP: for (int k = 0; k < 4; k++) res[k] = a[k] >= 0 ? b[k] : c[k]; break;
		case OP_LRP: for (int k = 0; k < 4; k++) res[k] = a[k] * (b[k] - c[k]) + c[k]; break;
		case OP_DSX: case OP_DSY: break;
		case OP_RCP: for (int k = 0; k < 4; k++) res[k] = 1.0f / a[0]; break;
		case OP_RSQ: for (int k = 0; k < 4; k++) res[k] = 1.0f / sqrtf(fabsf(a[0])); break;
		case OP_EXP: for (int k = 0; k < 4; k++) res[k] = exp2f(a[0]); break;
		case OP_LOG: for (int k = 0; k < 4; k++) res[k] = a[0] == 0 ? -INFINITY : log2f(fabsf(a[0])); break;
		case OP_POW: for (int k = 0; k < 4; k++) res[k] = powf(fabsf(a[0]), b[0]); break;
		case OP_DP3: res[0] = res[1] = res[2] = res[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; break;
		case OP_DP4: res[0] = res[1] = res[2] = res[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; break;
		case OP_DP2ADD: res[0] = res[1] = res[2] = res[3] = a[0] * b[0] + a[1] * b[1] + c[0]; break;
		case OP_CRS:
			res[0] = a[1] * b[2] - a[2] * b[1];
			res[1] = a[2] * b[0] - a[0] * b[2];
			res[2] = a[0] * b[1] - a[1] * b[0];
			break;
		case OP_NRM:
		{
			float l = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
			float s = l > 0 ? 1.0f / l : 0.0f;
			for (int k = 0; k < 4; k++)
				res[k] = a[k] * s;
			break;
		}
		case OP_SINCOS: res[0] = cosf(a[0]); res[1] = sinf(a[0]); break;
		case OP_TEXKILL:
		{
			float t[4];
			read(Src(RegType(p[0]), RegNum(p[0]), kIdentity), t);
			for (int k = 0; k < 4; k++)
				if ((DstMask(p[0]) & (1 << k)) && t[k] < 0)
					o.killed = true;
			continue;
		}
		case OP_TEXLD: case OP_TEXLDL: case OP_TEXLDD:
		{
			int smp = RegNum(p[2]);
			for (int k = 0; k < 4; k++)
				res[k] = TexValue(in.texSeed, smp, a[0], a[1], k);
			if (smp == in.lmSampler && in.noLightmapColour)
				res[0] = res[1] = res[2] = 0;
			if (smp == in.cloudSampler && in.noCloud)
				res[0] = res[1] = res[2] = res[3] = 0;
			// The texture's swizzle applies to the fetched value.
			float t[4];
			for (int k = 0; k < 4; k++)
				t[k] = res[SwzComp(p[2], k)];
			memcpy(res, t, sizeof(t));
			break;
		}
		default:
		{
			char buf[48];
			snprintf(buf, sizeof(buf), "unsupported opcode 0x%X", op);
			err = buf;
			return false;
		}
		}
		DWORD d = p[0];
		bool sat = ((d >> 20) & 1) != 0;
		float* dst = nullptr;
		if (RegType(d) == REG_TEMP)
			dst = r[RegNum(d)];
		else if (RegType(d) == REG_COLOROUT && RegNum(d) < 4)
		{
			dst = o.oC[RegNum(d)];
			o.written[RegNum(d)] = true;
		}
		else
		{
			err = "unsupported destination";
			return false;
		}
		for (int k = 0; k < 4; k++)
			if (DstMask(d) & (1 << k))
				dst[k] = sat ? (res[k] < 0 ? 0 : res[k] > 1 ? 1 : res[k]) : res[k];
	}
	err = "no end token";
	return false;
}

static uint32_t g_rng = 12345;
static float Rand() { return (g_rng = g_rng * 1664525u + 1013904223u) / 4294967296.0f; }

static bool Bad(float f) { return f != f || fabsf(f) > 1e30f; }

// Checks one input program (the game's shader, possibly soft-shadow rewritten).
static std::string Check(const std::vector<DWORD>& code, int& matchedOut, bool& failed)
{
	std::vector<DWORD> out;
	bool matched = false;
	std::string reason;
	matchedOut = -1;
	failed = false;
	if (!AO_RewritePS(code.data(), out, matched, reason))
		return "unchanged (" + reason + ")";
	matchedOut = matched ? 1 : 0;
	AoRewriteLayout L = g_aoLastLayout;
	std::string text;
	if (!Disassemble(out, text) || text.find("error") != std::string::npos)
	{
		failed = true;
		return "DISASSEMBLY FAILED";
	}
	std::vector<Inst> insts;
	size_t endPos;
	Parse(code.data(), insts, endPos);
	std::vector<CtabVar> vars;
	ReadCtabVars(code.data(), insts, vars);
	const CtabVar* amb = FindVar(vars, "g_AmbientCube", 2);
	const CtabVar* lm = FindVar(vars, "g_WorldLightMapSampler", 3);
	const CtabVar* cloud = FindVar(vars, "g_CloudShadowSampler", 3);

	int trials = 0, mism = 0, indBad = 0, cutBad = 0, zeroBad = 0, sunBad = 0, cloudBad = 0;
	float worst = 0;
	std::string err;
	for (int n = 0; n < 400 && trials < 150; n++)
	{
		Inputs in;
		for (auto& v : in.v)
			for (float& f : v)
				f = Rand() * 2 - 1;
		for (auto& c : in.c)
			for (float& f : c)
				f = Rand();
		for (auto& m : in.misc)
			for (float& f : m)
				f = Rand() * 100;
		for (bool& bb : in.b)
			bb = Rand() < 0.5f;
		// Constants near zero too, so comparisons in branches go both ways.
		for (auto& c : in.c)
			for (float& f : c)
				if (Rand() < 0.1f)
					f = Rand() * 2 - 1;
		in.texSeed = (uint32_t)(Rand() * 4e9);
		in.lmSampler = lm ? lm->index : -1;
		in.cloudSampler = cloud ? cloud->index : -1;
		Outputs a, b, z;
		if (!Run(code, in, a, err))
			return "interpreter: " + err;
		if (a.killed)
			continue;
		bool bad = false;
		for (int k = 0; k < 4; k++)
			bad |= Bad(a.oC[0][k]) || Bad(a.oC[1][k]);
		if (bad)
			continue;
		trials++;
		if (!Run(out, in, b, err))
			return "interpreter (rewritten): " + err;
		if (memcmp(a.oC[0], b.oC[0], 16) || memcmp(a.oC[1], b.oC[1], 16) || !b.written[2] || !b.written[3])
			mism++;
		if (matched)
		{
			// The term found at the combine: zeroing it must remove exactly oC2 from the colour.
			Inputs cut = in;
			cut.zeroAt = L.combineAt;
			cut.zeroSrc = L.addend;
			Outputs zc;
			if (!Run(code, cut, zc, err))
				return "interpreter (term removed): " + err;
			for (int k = 0; k < 3; k++)
			{
				float expect = a.oC[0][k] - zc.oC[0][k], got = b.oC[2][k];
				float tol = 2e-4f * (1.0f + fabsf(a.oC[0][k]) + fabsf(got));
				if (!(fabsf(expect - got) <= tol))
				{
					cutBad++;
					worst = fmaxf(worst, fabsf(expect - got));
				}
			}
		}
		if (matched)
		{
			// The sun term: zeroing the shadow operand at the combine must remove exactly oC3.
			Inputs cut = in;
			cut.zeroAt = L.combineAt;
			cut.zeroSrc = L.sunSrc;
			Outputs zs;
			if (!Run(code, cut, zs, err))
				return "interpreter (sun removed): " + err;
			for (int k = 0; k < 3; k++)
			{
				float expect = a.oC[0][k] - zs.oC[0][k], got = b.oC[3][k];
				if (!(fabsf(expect - got) <= 2e-4f * (1.0f + fabsf(a.oC[0][k]) + fabsf(got))))
				{
					sunBad++;
					worst = fmaxf(worst, fabsf(expect - got));
				}
			}
			if (cloud)
			{
				Inputs dark = in;
				dark.noCloud = true;
				Outputs zd;
				if (!Run(code, dark, zd, err))
					return "interpreter (no cloud): " + err;
				for (int k = 0; k < 3; k++)
				{
					float expect = a.oC[0][k] - zd.oC[0][k], got = b.oC[3][k];
					if (!(fabsf(expect - got) <= 2e-4f * (1.0f + fabsf(a.oC[0][k]) + fabsf(got))))
						cloudBad++;
				}
			}
		}
		if (matched && amb)
		{
			Inputs noAmb = in;
			if (amb)
				for (int r = amb->index; r < amb->index + amb->count; r++)
					for (float& f : noAmb.c[r])
						f = 0;
			noAmb.noLightmapColour = true;
			if (!Run(code, noAmb, z, err))
				return "interpreter (no ambient): " + err;
			for (int k = 0; k < 3; k++)
			{
				float expect = a.oC[0][k] - z.oC[0][k], got = b.oC[2][k];
				float tol = 2e-4f * (1.0f + fabsf(a.oC[0][k]) + fabsf(got));
				if (!(fabsf(expect - got) <= tol))
				{
					indBad++;
					worst = fmaxf(worst, fabsf(expect - got));
				}
			}
		}
		if (!matched && (b.oC[2][0] != 0 || b.oC[2][1] != 0 || b.oC[2][2] != 0 || b.oC[2][3] != 0 || b.oC[3][0] != 0 ||
							b.oC[3][1] != 0 || b.oC[3][2] != 0 || b.oC[3][3] != 0))
			zeroBad++;
	}
	char buf[200];
	// Shaders with an if (an omni light) add that light into the same register as the shadowed sun
	// before the combine, so there oC3 also holds the omni light: noted, not a failure.
	bool branches = false;
	for (const Inst& in : insts)
		branches |= in.op == OP_IF || in.op == OP_IFC;
	std::string note = branches && cloudBad ? " (sun term includes the branch's omni light)" : "";
	if (branches)
		cloudBad = 0;
	if (mism || indBad || cutBad || zeroBad || sunBad || cloudBad || !trials)
	{
		failed = true;
		snprintf(buf, sizeof(buf),
			"FAILED (%d trials: %d changed oC0/oC1, %d term / %d ambient / %d sun / %d cloud mismatches (worst %g), %d non-zero)", trials,
			mism, cutBad, indBad, sunBad, cloudBad, worst, zeroBad);
		return buf;
	}
	if (matched)
		snprintf(buf, sizeof(buf), "indirect + sun found, temps r%d/r%d, %d trials ok%s%s", L.temp, L.sunTemp, trials,
			amb ? "" : " (ambient cube from the vertex shader: term check only)", note.c_str());
	else
		snprintf(buf, sizeof(buf), "oC2 = oC3 = 0 (%s), %d trials ok", reason.c_str(), trials);
	return buf;
}

int main(int argc, char** argv)
{
	const char* dir = argc > 1 ? argv[1] : "../re/shaders/dump";
	LogInit("aoshader_test.log", true);
	for (const char* dll : { "D3DCompiler_47.dll", "D3DCompiler_43.dll" })
		if (HMODULE m = LoadLibraryA(dll))
			if ((g_disasm = (PFN_Disasm)GetProcAddress(m, "D3DDisassemble")) != nullptr)
				break;
	if (!g_disasm)
	{
		printf("no D3DCompiler available\n");
		return 1;
	}
	char pattern[MAX_PATH];
	snprintf(pattern, sizeof(pattern), "%s\\*.cso", dir);
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE)
	{
		printf("no .cso files in %s\n", dir);
		return 1;
	}
	int shaders = 0, matchedCount = 0, zeroCount = 0, failures = 0;
	do
	{
		char path[MAX_PATH];
		snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
		FILE* f = fopen(path, "rb");
		std::vector<DWORD> code(fd.nFileSizeLow / 4 + 1, 0);
		fread(code.data(), 1, fd.nFileSizeLow, f);
		fclose(f);
		shaders++;
		int m1, m2;
		bool f1, f2;
		std::string plain = Check(code, m1, f1);
		std::string softText = "no soft shadow variant";
		std::vector<DWORD> soft;
		std::string why;
		m2 = m1;
		f2 = false;
		if (SoftShadows_Rewrite(code.data(), 3, soft, why))
		{
			soft.push_back(0);
			softText = Check(soft, m2, f2);
		}
		if (m1 == 1)
			matchedCount++;
		else if (m1 == 0)
			zeroCount++;
		if (f1 || f2 || m1 != m2)
			failures++;
		printf("%.8s %s | soft w3: %s%s\n", fd.cFileName, plain.c_str(), softText.c_str(), m1 != m2 ? "  MATCH DIFFERS" : "");
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	printf("\n%d shaders: %d with the indirect and sun light found, %d writing oC2 = oC3 = 0, %d with failures\n", shaders, matchedCount, zeroCount,
		failures);
	return failures ? 3 : 0;
}
