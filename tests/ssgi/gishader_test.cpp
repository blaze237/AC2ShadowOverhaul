// Offline test for the SSGI shader rewrite, run against the game's real pixel shaders (.cso files
// exported by AC2ShadowOverhaul's F8 frame dump; default ../../re/shaders/dump):
//  1. D3DCompiler's disassembler accepts the rewritten bytecode;
//  2. with the GI texture black, every output is bit-identical to the original's;
//  3. with the GI texture = g, every output equals the original's with albedo x g added to the lit
//     colour at the combine (the interpreter injects it there), so fog etc. apply to it unchanged;
//  4. the GI texture is read once, at (Q . c216, Q . c217) / (Q . c218) with Q = (oC1.x, vPos, 1);
//     with the z/w copy, with coord.z = (Q . c219) / (Q . c218) (1 / the view z the texel should
//     hold), and its last level is read too and used instead when the texel's alpha isn't that z.
// Built with WITH_OVERHAUL (tests/run_tests.sh does, with src/softshadows.cpp and src/litshader.cpp):
//  - the same checks on the soft shadow + AO rewritten shaders (the AO outputs and the bounce light
//    as two separate rewrites, one on top of the other);
//  - the reverse order: the AO output must still be found on our code (and then includes the bounce);
//  - merged, as the plugin does it (Lit_RewritePS with both parts, on the soft shadow rewrite):
//    oC0 / oC1 bit-identical to the original with the GI texture black; oC2 = exactly the game's
//    baked indirect light x (1 - fog) (removing the ambient term at the combine removes exactly oC2
//    from the colour) and bit-identical whatever the GI texture holds; oC0 with GI = g equals the
//    original with albedo x g injected at the combine (the bounce light reaches the colour, fogged).
#include "interp.h"
#include <windows.h>

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

#ifdef WITH_OVERHAUL
#include "ao.h"
#include "litshader.h"
bool SoftShadows_Rewrite(const DWORD* code, int width, std::vector<DWORD>& out, std::string& reason);
#endif

static uint32_t g_rng = 12345;
static float Rand() { return (g_rng = g_rng * 1664525u + 1013904223u) / 4294967296.0f; }
static bool Bad(float f) { return f != f || fabsf(f) > 1e30f; }

static Inputs RandomInputs()
{
	Inputs in;
	for (auto& v : in.v)
		for (float& f : v)
			f = Rand() * 2 - 1;
	for (auto& c : in.c)
		for (float& f : c)
			f = Rand() < 0.1f ? Rand() * 2 - 1 : Rand(); // some near zero, so branches go both ways
	for (auto& m : in.misc)
		for (float& f : m)
			f = floorf(Rand() * 100);
	for (bool& bb : in.b)
		bb = Rand() < 0.5f;
	in.texSeed = (uint32_t)(Rand() * 4e9);
	return in;
}

static bool Same(const Outputs& a, const Outputs& b, bool exact, float& worst)
{
	for (int t = 0; t < 4; t++)
	{
		if (a.written[t] != b.written[t])
			return false;
		if (!a.written[t])
			continue;
		for (int k = 0; k < 4; k++)
		{
			float x = a.oC[t][k], y = b.oC[t][k];
			if (exact ? memcmp(&x, &y, 4) != 0 : !(fabsf(x - y) <= 2e-5f * (1.0f + fabsf(x) + fabsf(y))))
			{
				worst = fmaxf(worst, fabsf(x - y));
				return false;
			}
		}
	}
	return true;
}

struct Result
{
	bool rewritten = false, failed = false;
	int form = -1;
	bool depthExact = false;
	std::string text;
};

// Checks the GI rewrite of `code` (the game's shader, possibly rewritten by AC2ShadowOverhaul first).
static Result Check(const std::vector<DWORD>& code, std::vector<DWORD>* outCode = nullptr)
{
	Result R;
	std::vector<DWORD> out;
	std::string reason;
	if (!GI_RewritePS(code.data(), out, reason))
	{
		R.text = "unchanged (" + reason + ")";
		return R;
	}
	R.rewritten = true;
	GiRewriteLayout L = g_giLastLayout;
	R.form = L.form;
	R.depthExact = L.depthExact;
	if (outCode)
		*outCode = out;
	std::string text;
	if (!Disassemble(out, text) || text.find("error") != std::string::npos)
	{
		R.failed = true;
		R.text = "DISASSEMBLY FAILED";
		return R;
	}
	int trials = 0, blackBad = 0, giBad = 0, uvBad = 0, readsBad = 0, fallbackBad = 0;
	float worst = 0;
	std::string err;
	for (int n = 0; n < 400 && trials < 150; n++)
	{
		Inputs in = RandomInputs();
		Outputs a, b;
		if (!Run(code, in, a, err))
		{
			R.failed = true;
			R.text = "interpreter: " + err;
			return R;
		}
		bool bad = a.killed;
		for (int t = 0; t < 4; t++)
			for (int k = 0; k < 4; k++)
				bad |= a.written[t] && Bad(a.oC[t][k]);
		if (bad)
			continue;
		trials++;
		// 2. black GI: nothing changes.
		if (!Run(out, in, b, err))
		{
			R.failed = true;
			R.text = "interpreter (rewritten): " + err;
			return R;
		}
		if (!Same(a, b, true, worst))
			blackBad++;
		if (b.giReads != 1 || b.giCoarseReads != (L.depthExact ? 1 : 0))
			readsBad++;
		// 4. where the GI texture was read
		float q[4] = { L.depthExact ? a.oC[1][0] : 1.0f, in.misc[0][0], in.misc[0][1], 1.0f };
		float h[3];
		for (int r = 0; r < 3; r++)
			h[r] = q[0] * in.c[GI_CONST + r][0] + q[1] * in.c[GI_CONST + r][1] + q[2] * in.c[GI_CONST + r][2] + q[3] * in.c[GI_CONST + r][3];
		if (fabsf(h[2]) > 1e-2f)
			for (int k = 0; k < 2; k++)
			{
				float expect = h[k] / h[2], got = b.giUv[k];
				if (!(fabsf(expect - got) <= 1e-4f * (1.0f + fabsf(expect))))
					uvBad++;
			}
		if (L.depthExact && fabsf(h[2]) > 1e-2f)
		{
			const float* zr = in.c[GI_CONST + 3];
			float expect = (q[0] * zr[0] + q[1] * zr[1] + q[2] * zr[2] + q[3] * zr[3]) / h[2];
			if (!(fabsf(expect - b.giZ) <= 1e-4f * (1.0f + fabsf(expect))))
				uvBad++;
		}
		// 3. GI = g: the original with albedo x g added at the combine.
		Inputs g = in;
		for (int k = 0; k < 3; k++)
		{
			g.gi[k] = Rand() * 2;
			g.giCoarse[k] = Rand() * 2;
		}
		g.gi[3] = 1;
		g.giDepth = 1;
		Outputs gotG, expG;
		Inputs inj = g;
		inj.injectAt = L.combineAt;
		inj.injectAlb = L.albedo;
		inj.captureAt = L.albedoAt;
		if (!Run(out, g, gotG, err) || !Run(code, inj, expG, err))
		{
			R.failed = true;
			R.text = "interpreter (GI): " + err;
			return R;
		}
		if (!Same(expG, gotG, false, worst))
			giBad++;
		// 5. the texel shows another surface: the last level's light instead (with the z/w copy).
		g.giDepth = 2;
		if (L.depthExact)
			memcpy(inj.gi, g.giCoarse, sizeof(inj.gi));
		if (!Run(out, g, gotG, err) || !Run(code, inj, expG, err))
		{
			R.failed = true;
			R.text = "interpreter (GI fallback): " + err;
			return R;
		}
		if (!Same(expG, gotG, false, worst))
			fallbackBad++;
	}
	char buf[256];
	static const char* forms[] = { "sun*albedo + indirect", "ambient*albedo + sun", "sun + indirect" };
	if (blackBad || giBad || uvBad || readsBad || fallbackBad || !trials)
	{
		R.failed = true;
		snprintf(buf, sizeof(buf), "FAILED (%d trials: %d changed with black GI, %d wrong with GI (worst %g), %d wrong with the fallback, %d wrong uv, %d bad read counts)",
			trials, blackBad, giBad, worst, fallbackBad, uvBad, readsBad);
	}
	else
		snprintf(buf, sizeof(buf), "bounce light added (%s, albedo r%d%s, z/w %s), %d trials ok", forms[L.form], RegNum(L.albedo),
			L.albedoCopied ? " copied early" : "", L.depthExact ? "copied" : "NOT FOUND (rotation-only reprojection)", trials);
	R.text = buf;
	return R;
}

#ifdef WITH_OVERHAUL
// Their AO output on top of our code: must still be found, and oC2 (the indirect light AO darkens)
// must carry the bounce light: its change with GI = g must equal the colour's change.
static std::string CheckReverse(const std::vector<DWORD>& giCode, bool expectAo, bool& failed)
{
	failed = false;
	std::vector<DWORD> soft, ao;
	std::string why;
	const std::vector<DWORD>* base = &giCode;
	if (SoftShadows_Rewrite(giCode.data(), 3, soft, why))
	{
		soft.push_back(0);
		base = &soft;
	}
	bool matched = false;
	if (!AO_RewritePS(base->data(), ao, matched, why))
	{
		failed = expectAo;
		return "their AO rewrite refused: " + why;
	}
	if (matched != expectAo)
	{
		failed = true;
		return std::string("their AO match changed (") + (matched ? "now matched" : why) + ")";
	}
	if (!matched)
		return "their AO not matched (as without SSGI)";
	int trials = 0, bad = 0;
	std::string err;
	for (int n = 0; n < 300 && trials < 100; n++)
	{
		Inputs in = RandomInputs();
		Outputs z, g;
		if (!Run(ao, in, z, err))
		{
			failed = true;
			return "interpreter: " + err;
		}
		if (z.killed || Bad(z.oC[0][0]) || Bad(z.oC[2][0]))
			continue;
		Inputs ig = in;
		for (int k = 0; k < 3; k++)
			ig.gi[k] = Rand() * 2;
		ig.giDepth = 1;
		if (!Run(ao, ig, g, err))
		{
			failed = true;
			return "interpreter: " + err;
		}
		trials++;
		for (int k = 0; k < 3; k++)
		{
			float d0 = g.oC[0][k] - z.oC[0][k], d2 = g.oC[2][k] - z.oC[2][k];
			if (!(fabsf(d0 - d2) <= 2e-4f * (1.0f + fabsf(g.oC[0][k]) + fabsf(g.oC[2][k]))))
				bad++;
		}
	}
	if (bad || !trials)
	{
		failed = true;
		char buf[96];
		snprintf(buf, sizeof(buf), "their oC2 misses the bounce light in %d of %d trials", bad, trials);
		return buf;
	}
	return "their AO includes the bounce light";
}

// Merged, as the plugin creates the shaders: see the top.
static std::string CheckMerged(const std::vector<DWORD>& code, bool expectAo, bool& failed)
{
	failed = false;
	std::vector<DWORD> both;
	LitParts parts;
	parts.ao = parts.gi = true;
	LitRewriteResult res;
	if (!Lit_RewritePS(code.data(), parts, both, res) || !res.gi || res.ao != true || res.aoMatched != expectAo)
	{
		failed = true;
		return "merged: PARTS DIFFER (ao " + std::to_string(res.ao) + ", matched " + std::to_string(res.aoMatched) + ", gi " +
			std::to_string(res.gi) + ": " + res.aoWhy + " / " + res.giWhy + ")";
	}
	if (!expectAo)
		return "merged: bounce light, oC2 = 0 (as AO alone)";
	AoRewriteLayout LA = g_aoLastLayout;
	GiRewriteLayout LG = g_giLastLayout;
	std::string text;
	if (!Disassemble(both, text) || text.find("error") != std::string::npos)
	{
		failed = true;
		return "merged: DISASSEMBLY FAILED";
	}
	int trials = 0, baseBad = 0, termBad = 0, giMoves = 0, bounceBad = 0;
	float worst = 0;
	std::string err;
	for (int n = 0; n < 400 && trials < 120; n++)
	{
		Inputs in = RandomInputs();
		Outputs a, b;
		if (!Run(code, in, a, err) || !Run(both, in, b, err))
		{
			failed = true;
			return "merged: interpreter: " + err;
		}
		bool bad = a.killed;
		for (int t = 0; t < 2; t++)
			for (int k = 0; k < 4; k++)
				bad |= Bad(a.oC[t][k]);
		if (bad)
			continue;
		trials++;
		baseBad += memcmp(a.oC[0], b.oC[0], 16) != 0 || memcmp(a.oC[1], b.oC[1], 16) != 0 || !b.written[2] || !b.written[3];
		// oC2 = the baked indirect term's share of the colour
		Inputs cut = in;
		cut.zeroAt = LA.combineAt;
		cut.zeroSrc = LA.addend;
		Outputs zc;
		if (!Run(code, cut, zc, err))
		{
			failed = true;
			return "merged: interpreter: " + err;
		}
		for (int k = 0; k < 3; k++)
		{
			float expect = a.oC[0][k] - zc.oC[0][k], got = b.oC[2][k];
			if (!(fabsf(expect - got) <= 2e-4f * (1.0f + fabsf(a.oC[0][k]) + fabsf(got))))
			{
				termBad++;
				worst = fmaxf(worst, fabsf(expect - got));
			}
		}
		// with GI = g: oC2 unchanged, oC0 = the original with albedo x g injected at the combine
		Inputs g = in;
		for (int k = 0; k < 3; k++)
			g.gi[k] = Rand() * 2;
		g.gi[3] = 1;
		g.giDepth = 1;
		Inputs inj = g;
		inj.injectAt = LG.combineAt;
		inj.injectAlb = LG.albedo;
		inj.captureAt = LG.albedoAt;
		Outputs bg, eg;
		if (!Run(both, g, bg, err) || !Run(code, inj, eg, err))
		{
			failed = true;
			return "merged: interpreter: " + err;
		}
		giMoves += memcmp(b.oC[2], bg.oC[2], 16) != 0 || memcmp(b.oC[3], bg.oC[3], 16) != 0;
		for (int k = 0; k < 3; k++)
			if (!(fabsf(eg.oC[0][k] - bg.oC[0][k]) <= 2e-5f * (1.0f + fabsf(eg.oC[0][k]) + fabsf(bg.oC[0][k]))))
			{
				bounceBad++;
				worst = fmaxf(worst, fabsf(eg.oC[0][k] - bg.oC[0][k]));
				break;
			}
	}
	char buf[200];
	if (baseBad || termBad || giMoves || bounceBad || !trials)
	{
		failed = true;
		snprintf(buf, sizeof(buf),
			"merged: FAILED (%d trials: %d changed oC0/oC1, %d oC2 not the baked indirect, %d oC2/oC3 moved with GI, %d bounce wrong; worst %g)",
			trials, baseBad, termBad, giMoves, bounceBad, worst);
		return buf;
	}
	snprintf(buf, sizeof(buf), "merged: oC2 = baked indirect only, bounce in oC0, temps r%d-r%d, %d trials ok", LG.temp,
		LA.sunTemp > LG.temp2 ? LA.sunTemp : LG.temp2, trials);
	return buf;
}
#endif

int main(int argc, char** argv)
{
	const char* dir = argc > 1 ? argv[1] : "../../re/shaders/dump";
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
	int shaders = 0, rewritten = 0, exact = 0, failures = 0;
	do
	{
		char path[MAX_PATH];
		snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
		FILE* f = fopen(path, "rb");
		std::vector<DWORD> code(fd.nFileSizeLow / 4 + 1, 0);
		fread(code.data(), 1, fd.nFileSizeLow, f);
		fclose(f);
		shaders++;
		std::vector<DWORD> giCode;
		Result r = Check(code, &giCode);
		rewritten += r.rewritten;
		exact += r.rewritten && r.depthExact;
		bool fail = r.failed;
		std::string line = r.text;
#ifdef WITH_OVERHAUL
		// Their rewrites first (soft shadows width 3, then the AO output), then ours.
		std::vector<DWORD> soft, ao;
		std::string why;
		const std::vector<DWORD>* base = &code;
		if (SoftShadows_Rewrite(code.data(), 3, soft, why))
		{
			soft.push_back(0);
			base = &soft;
		}
		bool aoMatched = false;
		if (AO_RewritePS(base->data(), ao, aoMatched, why))
		{
			ao.push_back(0);
			base = &ao;
		}
		Result r2 = Check(*base);
		if (r2.rewritten != r.rewritten || r2.failed)
			fail = true;
		line += " | after theirs: " + (r2.rewritten == r.rewritten ? r2.text : "MATCH DIFFERS: " + r2.text);
		if (r.rewritten)
		{
			bool f3;
			std::string rev = CheckReverse(giCode, aoMatched, f3);
			fail |= f3;
			line += " | theirs after ours: " + rev;
			bool fm;
			line += " | " + CheckMerged(soft.empty() ? code : soft, aoMatched, fm);
			fail |= fm;
		}
#endif
		failures += fail;
		printf("%.8s %s%s\n", fd.cFileName, fail ? "** " : "", line.c_str());
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	printf("\n%d shaders: %d given bounce light (%d with exact depth reprojection), %d with failures\n", shaders, rewritten, exact, failures);
	return failures ? 3 : 0;
}
