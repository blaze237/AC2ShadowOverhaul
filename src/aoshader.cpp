// AO shader rewrite: adds oC2 = indirect light x (1 - fog) to the main opaque pass's pixel shaders.
//
// AC2's lit shaders compute (with many compiled variations)
//     indirect = lerp(g_AmbientCube(normal) * albedo, lightmap.rgb * albedo, w)     (+ more terms)
//     lit      = sunDirect * (shadow * cloud) + indirect                            <- "combine"
//                (or the other way round: lit = indirect * vertexAO + shadowedSun, or an add)
//     lit      = lit + other unshadowed terms (rim / fill light)                    (optional)
//     oC0.xyz  = lit + fogFactor * (fogColour - lit)                                <- "final"
// We follow two taints through the straight-line code: AMB (read from g_AmbientCube or the world
// lightmap) and SH (read from the shadow or cloud shadow map). The combine is the first mad or add
// that adds a term carrying only AMB to a term carrying only SH: the AMB term is the complete
// indirect light. From there the lit colour may only have terms added to it (mad with the lit
// colour as addend, add, mov) until the final mad into oC0, which gives the fog factor.
//
// Inserted code (fresh temp T, constant K = (0, 1, 0, 0)):
//     before the combine:   mov rT.xyz, <indirect term>   (mul rT.xyz, a, b if it is a mad's product)
//     before the final:     mad rT.xyz, rT, -fog, rT ; mov rT.w, K.x ; mov oC2, rT
// Shaders that write oC1 but don't match get "mov oC2, K.x" at the top, so the target never keeps
// a value from a surface hidden behind them. Nothing else in the shader changes.

#include "ao.h"
#include "sm3.h"
#include <stdio.h>

AoRewriteLayout g_aoLastLayout;

enum { T_AMB = 1, T_SH = 2, T_LIT = 4 };

namespace
{
struct Taint
{
	uint8_t r[32][4] = {};
	int ambLo = -1, ambHi = -1; // g_AmbientCube registers [lo, hi)
	int lmSampler = -1, shadowSampler = -1, cloudSampler = -1;

	uint8_t Of(DWORD s, int k) const
	{
		int type = RegType(s), n = RegNum(s);
		if (type == REG_TEMP && n < 32)
			return r[n][SwzComp(s, k)];
		if (type == REG_CONST && n >= ambLo && n < ambHi)
			return T_AMB;
		return 0;
	}
	uint8_t All(DWORD s, int comps = 4) const
	{
		uint8_t t = 0;
		for (int k = 0; k < comps; k++)
			t |= Of(s, k);
		return t;
	}
};

// Taint of each destination component of an instruction.
void InstTaint(const Taint& t, const DWORD* code, const Inst& in, uint8_t res[4])
{
	const DWORD* p = &code[in.pos + 1];
	int nsrc = (int)in.len - 1;
	for (int k = 0; k < 4; k++)
		res[k] = 0;
	switch (in.op)
	{
	case OP_MOV: case OP_ADD: case OP_SUB: case OP_MAD: case OP_MUL: case OP_MIN: case OP_MAX: case OP_SLT: case OP_SGE:
	case OP_LRP: case OP_FRC: case OP_CMP: case OP_ABS: case OP_DSX: case OP_DSY:
		for (int k = 0; k < 4; k++)
			for (int s = 1; s <= nsrc; s++)
				res[k] |= t.Of(p[s], k);
		return;
	case OP_TEXLD: case OP_TEXLDL: case OP_TEXLDD:
	{
		// The fetched value doesn't scale with the coordinates, so they don't pass their taint on.
		uint8_t v = 0;
		int smp = nsrc >= 2 ? RegNum(p[2]) : -1;
		if (smp == t.lmSampler)
			v |= T_AMB;
		if (smp == t.shadowSampler || smp == t.cloudSampler)
			v |= T_SH;
		for (int k = 0; k < 4; k++)
			res[k] = v;
		return;
	}
	case OP_DP3: case OP_NRM: case OP_CRS:
	{
		uint8_t v = 0;
		for (int s = 1; s <= nsrc; s++)
			v |= t.All(p[s], 3);
		for (int k = 0; k < 4; k++)
			res[k] = v;
		return;
	}
	case OP_DP2ADD:
	{
		uint8_t v = t.All(p[1], 2) | t.All(p[2], 2) | t.Of(p[3], 0);
		for (int k = 0; k < 4; k++)
			res[k] = v;
		return;
	}
	default: // dp4, rcp, rsq, pow, exp, log, sincos, ...: every input feeds every output
	{
		uint8_t v = 0;
		for (int s = 1; s <= nsrc; s++)
			v |= t.All(p[s]);
		for (int k = 0; k < 4; k++)
			res[k] = v;
		return;
	}
	}
}

bool NegateSrc(DWORD s, DWORD& out)
{
	int mod = SrcMod(s), neg;
	switch (mod)
	{
	case 0x0: neg = 0x1; break;
	case 0x1: neg = 0x0; break;
	case 0xB: neg = 0xC; break; // abs -> -abs
	case 0xC: neg = 0xB; break;
	default: return false;
	}
	out = (s & ~0x0F000000u) | ((DWORD)neg << 24);
	return true;
}

bool IdentityXyz(DWORD s) { return SwzComp(s, 0) == 0 && SwzComp(s, 1) == 1 && SwzComp(s, 2) == 2; }

// Finds the combine and final instructions. fogSrc = 0 when the final instruction has no fog.
bool FindIndirect(const DWORD* code, const std::vector<Inst>& insts, const std::vector<CtabVar>& vars, size_t& combI,
	size_t& finalI, DWORD indirect[2], DWORD& ambSrc, DWORD& fogSrc, std::string& why)
{
	Taint t;
	if (const CtabVar* v = FindVar(vars, "g_AmbientCube", 2))
	{
		t.ambLo = v->index;
		t.ambHi = v->index + v->count;
	}
	if (const CtabVar* v = FindVar(vars, "g_WorldLightMapSampler", 3))
		t.lmSampler = v->index;
	if (const CtabVar* v = FindVar(vars, "g_ShadowSampler", 3))
		t.shadowSampler = v->index;
	if (const CtabVar* v = FindVar(vars, "g_CloudShadowSampler", 3))
		t.cloudSampler = v->index;
	if (t.ambLo < 0 && t.lmSampler < 0)
	{
		why = "no ambient light inputs";
		return false;
	}
	if (t.shadowSampler < 0)
	{
		why = "no sun shadow";
		return false;
	}
	combI = finalI = 0;
	fogSrc = 0;
	bool haveComb = false, haveFinal = false;
	// if / else / endif blocks: either branch may run, so taints are merged at the endif. Our
	// insertion points must be outside them. Loops, subroutines and predication aren't handled.
	struct Branch
	{
		Taint before, thenEnd;
		bool inElse = false;
	};
	std::vector<Branch> branches;
	for (size_t i = 0; i < insts.size(); i++)
	{
		const Inst& in = insts[i];
		if (in.op == OP_IF || in.op == OP_IFC)
		{
			branches.push_back(Branch());
			branches.back().before = t;
			continue;
		}
		if (in.op == OP_ELSE || in.op == OP_ENDIF)
		{
			if (branches.empty())
			{
				why = "unbalanced if";
				return false;
			}
			Branch& b = branches.back();
			if (in.op == OP_ELSE)
			{
				b.thenEnd = t;
				b.inElse = true;
				t = b.before;
				continue;
			}
			const Taint& other = b.inElse ? b.thenEnd : b.before;
			for (int r = 0; r < 32; r++)
				for (int k = 0; k < 4; k++)
					t.r[r][k] |= other.r[r][k];
			branches.pop_back();
			continue;
		}
		if (IsFlowControl(in.op) || in.predicated)
		{
			why = "loop, subroutine or predication";
			return false;
		}
		if (!WritesDst(in))
			continue;
		const DWORD* p = &code[in.pos + 1];
		DWORD d = p[0];
		int nsrc = (int)in.len - 1;
		uint8_t res[4];
		InstTaint(t, code, in, res);

		if (!haveComb && branches.empty() && (in.op == OP_MAD || in.op == OP_ADD) && RegType(d) == REG_TEMP &&
			DstMask(d) == (X | Y | Z))
		{
			// Taint of the components actually read for x, y, z (sources may be swizzled).
			// mad lit, sun, shadow, indirect / mad lit, indirect, factor, shadowedSun /
			// add lit, shadowedSun, indirect (either order)
			const int nIn = in.op == OP_MAD ? 3 : 2;
			uint8_t st[3] = {};
			for (int s = 0; s < nIn; s++)
				for (int k = 0; k < 3; k++)
					st[s] |= t.Of(p[1 + s], k);
			auto sunOnly = [](uint8_t v) { return (v & T_SH) && !(v & (T_AMB | T_LIT)); };
			auto ambOnly = [](uint8_t v) { return (v & T_AMB) && !(v & (T_SH | T_LIT)); };
			int found = -1;
			if (in.op == OP_MAD && sunOnly(st[0] | st[1]) && ambOnly(st[2]))
				found = 2;
			else if (in.op == OP_MAD && ambOnly(st[0] | st[1]) && sunOnly(st[2]) && !SrcMod(p[1]) && !SrcMod(p[2]))
				found = 3; // the product
			else if (in.op == OP_ADD && sunOnly(st[0]) && ambOnly(st[1]))
				found = 1;
			else if (in.op == OP_ADD && sunOnly(st[1]) && ambOnly(st[0]))
				found = 0;
			if (found == 3)
			{
				indirect[0] = p[1];
				indirect[1] = p[2];
				ambSrc = (st[0] & T_AMB) ? p[1] : p[2];
			}
			else if (found >= 0 && SrcMod(p[1 + found]) == 0)
			{
				indirect[0] = ambSrc = p[1 + found];
				indirect[1] = 0;
			}
			else
				found = -1;
			if (found >= 0)
			{
				haveComb = true;
				combI = i;
				for (int k = 0; k < 4; k++)
					res[k] |= T_LIT;
			}
		}
		else if (haveComb)
		{
			// Which sources carry the lit colour?
			int litSrc = 0, litMask = 0;
			for (int s = 1; s <= nsrc; s++)
			{
				uint8_t v = 0;
				for (int k = 0; k < 4; k++)
					if (DstMask(d) & (1 << k))
						v |= t.Of(p[s], k);
				if (v & T_LIT)
				{
					litSrc++;
					litMask |= 1 << s;
				}
			}
			bool toColour0 = RegType(d) == REG_COLOROUT && RegNum(d) == 0 && (DstMask(d) & (X | Y | Z));
			if (toColour0)
			{
				if (!branches.empty())
				{
					why = "final colour written inside a branch";
					return false;
				}
				if (haveFinal)
				{
					why = "oC0 written twice";
					return false;
				}
				if (!litSrc)
				{
					why = "final colour doesn't use the lit colour";
					return false;
				}
				if ((DstMask(d) & (X | Y | Z)) != (X | Y | Z))
				{
					why = "final colour written in parts";
					return false;
				}
				if (in.op == OP_MAD && litMask == ((1 << 2) | (1 << 3)) && IdentityXyz(p[3]))
				{
					DWORD neg;
					if (!NegateSrc(p[1], neg))
					{
						why = "unusual fog factor";
						return false;
					}
					fogSrc = p[1]; // oC0 = lit + fog * (fogColour - lit)
				}
				else if ((in.op == OP_MOV && litMask == 2 && IdentityXyz(p[1])) ||
						 (in.op == OP_MAD && litMask == 8 && IdentityXyz(p[3])) ||
						 (in.op == OP_ADD && litSrc == 1 && IdentityXyz(p[litMask == 2 ? 1 : 2]) && SrcMod(p[litMask == 2 ? 1 : 2]) == 0))
					fogSrc = 0; // no fog: added terms only
				else
				{
					why = "final colour not recognised";
					return false;
				}
				haveFinal = true;
				finalI = i;
			}
			else if (litSrc)
			{
				// Between combine and final the lit colour may only have things added to it.
				bool ok = (in.op == OP_MOV && litSrc == 1) || (in.op == OP_ADD && litSrc == 1) ||
					(in.op == OP_MAD && litMask == 8);
				if (!ok)
				{
					why = "lit colour is scaled after the indirect light is added";
					return false;
				}
			}
		}
		if (RegType(d) == REG_TEMP && RegNum(d) < 32)
			for (int k = 0; k < 4; k++)
				if (DstMask(d) & (1 << k))
					t.r[RegNum(d)][k] = res[k];
#ifdef AO_REWRITE_DEBUG
		AO_REWRITE_DEBUG(code, in, res);
#endif
	}
	if (!branches.empty())
	{
		why = "unbalanced if";
		return false;
	}
	if (!haveComb)
	{
		why = "sun + indirect light not found";
		return false;
	}
	if (!haveFinal)
	{
		why = "final colour not found";
		return false;
	}
	return true;
}
} // namespace

bool AO_RewritePS(const DWORD* code, std::vector<DWORD>& result, bool& matched, std::string& reason)
{
	matched = false;
	reason.clear();
	std::vector<Inst> insts;
	size_t endPos = 0;
	if (!code || !Parse(code, insts, endPos))
	{
		reason = "not ps_3_0";
		return false;
	}
	bool writesOC1 = false;
	for (const Inst& in : insts)
		if (WritesDst(in) && RegType(code[in.pos + 1]) == REG_COLOROUT)
		{
			int n = RegNum(code[in.pos + 1]);
			if (n >= 2)
			{
				reason = "already writes oC2";
				return false;
			}
			if (n == 1)
				writesOC1 = true;
		}
	if (!writesOC1)
	{
		reason = "single render target";
		return false;
	}
	std::vector<CtabVar> vars;
	ReadCtabVars(code, insts, vars);
	int maxTemp;
	bool constUsed[256];
	if (!RegisterUsage(code, insts, vars, maxTemp, constUsed))
	{
		reason = "relative constant addressing";
		return false;
	}
	const int T = maxTemp + 1;
	int K = -1;
	for (int r = 223; r >= 0 && K < 0; r--)
		if (!constUsed[r])
			K = r;
	if (T > 31 || K < 0)
	{
		reason = T > 31 ? "no free temp register" : "no free constant register";
		return false;
	}

	size_t combI = 0, finalI = 0;
	DWORD indirect[2] = {}, ambSrc = 0, fogSrc = 0;
	std::string why;
	matched = FindIndirect(code, insts, vars, combI, finalI, indirect, ambSrc, fogSrc, why);
	if (!matched)
		reason = why;

	std::vector<DWORD> out;
	out.reserve(endPos + 40);
	Emitter e{ out };
	const size_t firstReal = FirstRealInstruction(insts, endPos);
	out.insert(out.end(), code, code + firstReal);
	e.I(OP_DEF, { Dst(REG_CONST, K, 0xF), FloatBits(0.0f), FloatBits(1.0f), FloatBits(0.0f), FloatBits(0.0f) });
	if (!matched)
	{
		// After the declarations (they must come first).
		size_t at = firstReal;
		for (const Inst& in : insts)
		{
			if (in.pos < firstReal || in.op == OP_COMMENT || in.op == OP_DCL || in.op == OP_DEF || in.op == OP_DEFI || in.op == OP_DEFB)
				continue;
			at = in.pos;
			break;
		}
		out.insert(out.end(), code + firstReal, code + at);
		e.I(OP_MOV, { Dst(REG_COLOROUT, 2, 0xF), Src(REG_CONST, K, Rep(0)) });
		out.insert(out.end(), code + at, code + endPos + 1);
	}
	else
	{
		const size_t combPos = insts[combI].pos, finalPos = insts[finalI].pos;
		out.insert(out.end(), code + firstReal, code + combPos);
		if (indirect[1])
			e.I(OP_MUL, { Dst(REG_TEMP, T, X | Y | Z), indirect[0], indirect[1] });
		else
			e.I(OP_MOV, { Dst(REG_TEMP, T, X | Y | Z), indirect[0] });
		out.insert(out.end(), code + combPos, code + finalPos);
		if (fogSrc)
		{
			DWORD negFog = 0;
			NegateSrc(fogSrc, negFog);
			e.I(OP_MAD, { Dst(REG_TEMP, T, X | Y | Z), Src(REG_TEMP, T, kIdentity), negFog, Src(REG_TEMP, T, kIdentity) });
		}
		e.I(OP_MOV, { Dst(REG_TEMP, T, W), Src(REG_CONST, K, Rep(0)) });
		e.I(OP_MOV, { Dst(REG_COLOROUT, 2, 0xF), Src(REG_TEMP, T, kIdentity) });
		out.insert(out.end(), code + finalPos, code + endPos + 1);
		g_aoLastLayout.combineAt = combPos;
		g_aoLastLayout.addend = ambSrc;
		g_aoLastLayout.finalAt = finalPos;
	}
	g_aoLastLayout.matched = matched;
	g_aoLastLayout.temp = T;
	g_aoLastLayout.constReg = K;
	result.swap(out);
	return true;
}
