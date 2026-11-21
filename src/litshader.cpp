// The lit opaque shaders: one dataflow analysis, one rewrite for ambient occlusion and SSGI (see
// litshader.h). Ambient occlusion's and SSGI's shader rewrites used to be two plugins with an
// analysis each (the same one, copied); here both use this one, in one pass over the bytecode.
//
// AC2's lit shaders compute (with many compiled variations, see the F8 frame dump exports)
//     indirect = lerp(g_AmbientCube(normal) * albedo, lightmap.rgb * albedo, w)     (+ more terms)
//     lit      = sunDirect * (shadow * cloud) * albedo + indirect                   <- "combine"
//                (or the other way round: lit = ambient * albedo + shadowedSun, or an add)
//     lit      = lit + other unshadowed terms (rim / fill light)                    (optional)
//     oC0.xyz  = lit + fogFactor * (fogColour - lit)                                <- "final"
//     oC1      = z / w                                                              (opaque pass only)
//
// Analysis. Two taints are followed through the straight-line code (if / else merged at the endif):
// AMB (read from g_AmbientCube or the world lightmap) and SH (read from the shadow or cloud shadow
// map). The combine is the first mad or add that adds a term carrying only AMB to a term carrying
// only SH: the AMB term is the complete indirect light, the SH term the shadowed sun light. From
// there the lit colour may only have terms added to it (mad with the lit colour as addend, add,
// mov) until the final mad into oC0, which gives the fog factor. The albedo (for the bounce light)
// is the factor the sun or ambient light is multiplied by that carries neither taint.
//
// Shading normal. g_AmbientCube is evaluated with the per-pixel normal the shader lights with
// (normal map and two-sided flip applied; world space, like the sun direction it is dotted with):
//     cmp faceSel.xyz, N, 1, 0 / cmp ..., -N, ...     (which cube face per axis: the signs)
//     mul sq.xyz, N, N                                  (the weights: the squares)
//     mul t, sq.x, g_AmbientCube[0] ... sq.z, g_AmbientCube[5]
// N is taken to be the normal only when exactly one top-level "mul sq.xyz, N, N" before the
// combine has each of sq.x, sq.y, sq.z multiplying a g_AmbientCube register and N's own value is
// also sign-tested by a cmp; otherwise there is no normal (oC2.w = oC3.w = 0, as before).
//
// Inserted code. AO (fresh temps T, S, and N for the normal; constant Ka = (0, 1, 2, 2^-20)):
//     before the combine:   mov rT.xyz, <indirect term> ; mov rS.xyz, <sun term>
//                           (mul instead of mov for a term that is a mad's product)
//     before the square:    mov rN.xyz, N
//     before the final:     mad rT.xyz, rT, -fog, rT ; mad rS.xyz, rS, -fog, rS ;
//                           rT.w, rS.w = octahedral encoding of rN + 2 (Ka.x = 0 without a normal) ;
//                           mov oC2, rT ; mov oC3, rS
//   Shaders that write oC1 but don't match get "mov oC2, Ka.x ; mov oC3, Ka.x" at the top, so the
//   targets never keep a value from a surface hidden behind them.
//   The encoding: p = N / (|N.x| + |N.y| + |N.z|); where N.z < 0, p.xy = (1 - |p.yx|) * sign(p.xy);
//   + 2 so that 0 (a target cleared, an unmatched shader) is never a normal (core/geoprep.hlsl).
// SSGI (fresh temps G, Q; constant Kg = (1, 0, 0, 0)), right before the combine:
//     Q.x  = z / w            (a copy of the instructions that compute oC1.x; 1 if they can't be copied)
//     Q.yz = vPos.xy, Q.w = 1
//     G.xyw = Q . c216, Q . c217, Q . c218     (reprojection into the GI texture of the last frame)
//     G     = tex2D(s13, G.xy / G.w)
//     indirect' = albedo * G + indirect
//   and the combine reads indirect' instead. Everything after it (other light, fog) is unchanged, so
//   the bounce light is fogged exactly like the game's ambient light. A shader with no GI texture
//   bound samples black and renders exactly as before.
// Both: the SSGI code first, then the AO capture of the game's own indirect term (still in its
//   register: SSGI's sum goes to G), then the combine reading G. So oC2 never holds the bounce
//   light. T reuses Q and S the albedo copy (both dead by then), so both parts need 3 fresh temps
//   (4 with the normal).
// Nothing else in the shader changes.

#include "litshader.h"
#include "ao.h"
#include "ssgi/gi.h"
#include "sm3.h"
#include <stdio.h>

AoRewriteLayout g_aoLastLayout;
namespace ssgi
{
GiRewriteLayout g_giLastLayout;
}
using ssgi::GI_CONST;
using ssgi::GI_SAMPLER;

enum { T_AMB = 1, T_SH = 2, T_LIT = 4 };
static const int REG_MISC = 17; // vPos (0), vFace (1)

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
	uint8_t Xyz(DWORD s) const { return Of(s, 0) | Of(s, 1) | Of(s, 2); }
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

// A source register still holds the value it had at instruction `from` when `to` runs.
bool StillHolds(const DWORD* code, const std::vector<Inst>& insts, DWORD src, size_t from, size_t to)
{
	if (RegType(src) != REG_TEMP)
		return RegType(src) == REG_INPUT || RegType(src) == REG_CONST;
	int mask = 0;
	for (int k = 0; k < 3; k++)
		mask |= 1 << SwzComp(src, k);
	for (size_t k = from + 1; k < to; k++)
		if (WritesTemp(code, insts[k], RegNum(src), mask))
			return false;
	return true;
}

// What the analysis found.
struct Lighting
{
	size_t combI = 0, finalI = 0; // indices into insts
	int form = -1;                // ssgi::GI_FORM_*
	int addSrc = 0;               // forms SUN_ALBEDO / ADD: the combine source (1..3) holding the indirect light
	DWORD indirect[2] = {};       // the indirect term: one source, or the two factors of a product
	DWORD ambSrc = 0;             // its source carrying the ambient light (zeroing it removes the indirect term)
	DWORD sun[2] = {};            // the shadowed sun term, likewise
	DWORD sunSrc = 0;             // its source carrying the shadow (zeroing it removes the sun term)
	DWORD fogSrc = 0;             // the fog factor at the final instruction (0: no fog)
	bool fogNegatable = true;     // it can be negated with a source modifier (AO needs -fog)
	bool albedoFound = false;
	std::string albedoWhy;
	DWORD albedo = 0;   // source token, as read by instruction albedoI
	size_t albedoI = 0; // the combine, or an earlier product the albedo is read from
	bool albedoCopy = false; // its register changes before the combine: copy it at albedoI
};

void FindAlbedo(const DWORD* code, const std::vector<Inst>& insts, const std::vector<Taint>& history, const std::vector<int>& depthAt,
	Lighting& f)
{
	const Inst& C = insts[f.combI];
	const DWORD* p = &code[C.pos + 1];
	const Taint& tc = history[f.combI];
	auto plainFactor = [&](const Taint& tt, DWORD s) { return tt.Xyz(s) == 0; };
	f.albedoFound = false;
	if (f.form == ssgi::GI_FORM_SUN_ALBEDO || f.form == ssgi::GI_FORM_AMB_PRODUCT)
	{
		// mad lit, a, b, indirect with a * b = sun light (or mad lit, a, b, sun with a * b = indirect
		// light): the albedo is the factor without the shadow (or the ambient light).
		bool a = plainFactor(tc, p[1]), b = plainFactor(tc, p[2]);
		if (a == b)
		{
			f.albedoWhy = f.form == ssgi::GI_FORM_AMB_PRODUCT ? "albedo not separable from the indirect term"
				: a ? "sun term has no shadow factor" : "albedo not separable from the sun term";
			return;
		}
		f.albedo = a ? p[1] : p[2];
		f.albedoI = f.combI;
		f.albedoFound = true;
		return;
	}
	// add lit, sun, indirect. First try the sun side: the sun term straight from a top-level mul
	// with a plain factor.
	f.albedoI = f.combI;
	DWORD sun = p[f.addSrc == 1 ? 2 : 1];
	if (RegType(sun) == REG_TEMP && IdentityXyz(sun))
	{
		int reg = RegNum(sun);
		for (size_t j = f.combI; j-- > 0;)
		{
			const Inst& in = insts[j];
			if (depthAt[j] != 0 || in.op == OP_ENDIF || in.op == OP_ELSE)
				break;
			if (!WritesTemp(code, in, reg, X | Y | Z))
				continue;
			const DWORD* q = &code[in.pos + 1];
			if (in.op != OP_MUL || (DstMask(q[0]) & (X | Y | Z)) != (X | Y | Z))
				break;
			bool a = plainFactor(history[j], q[1]), b = plainFactor(history[j], q[2]);
			if (a == b)
				break;
			f.albedo = a ? q[1] : q[2];
			f.albedoI = j;
			f.albedoCopy = !StillHolds(code, insts, f.albedo, j, f.combI);
			f.albedoFound = true;
			return;
		}
	}
	// Else the ambient side (the sun term may be built inside a branch, e.g. with an omni light):
	// the last top-level "ambient x plain colour" product before the combine.
	for (size_t j = f.combI; j-- > 0;)
	{
		const Inst& in = insts[j];
		if (depthAt[j] != 0 || in.op != OP_MUL)
			continue;
		const DWORD* q = &code[in.pos + 1];
		if (RegType(q[0]) != REG_TEMP || (DstMask(q[0]) & (X | Y | Z)) != (X | Y | Z))
			continue;
		uint8_t t1 = history[j].Xyz(q[1]), t2 = history[j].Xyz(q[2]);
		DWORD alb;
		if (t1 == T_AMB && t2 == 0)
			alb = q[2];
		else if (t2 == T_AMB && t1 == 0)
			alb = q[1];
		else
			continue;
		if (SwzComp(alb, 0) == SwzComp(alb, 1) && SwzComp(alb, 1) == SwzComp(alb, 2))
			continue; // a scalar (vertex AO, a blend weight): not the colour
		f.albedo = alb;
		f.albedoI = j;
		f.albedoCopy = !StillHolds(code, insts, f.albedo, j, f.combI);
		f.albedoFound = true;
		return;
	}
	f.albedoWhy = "albedo not traceable";
}

// Finds the combine, the final instruction and the albedo. Returns false (with `why`) if the
// combine and final aren't there; the fog's and albedo's own problems are reported in `f`.
bool AnalyseLighting(const DWORD* code, const std::vector<Inst>& insts, const core::ConstantTable& vars, Lighting& f, std::string& why)
{
	Taint t;
	if (const core::ShaderConstant* v = vars.Find("g_AmbientCube", core::RS_FLOAT4))
	{
		t.ambLo = v->reg;
		t.ambHi = v->reg + v->count;
	}
	if (const core::ShaderConstant* v = vars.Find("g_WorldLightMapSampler", core::RS_SAMPLER))
		t.lmSampler = v->reg;
	if (const core::ShaderConstant* v = vars.Find("g_ShadowSampler", core::RS_SAMPLER))
		t.shadowSampler = v->reg;
	if (const core::ShaderConstant* v = vars.Find("g_CloudShadowSampler", core::RS_SAMPLER))
		t.cloudSampler = v->reg;
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
	bool haveComb = false, haveFinal = false;
	// if / else / endif blocks: either branch may run, so taints are merged at the endif. Our
	// insertion points must be outside them. Loops, subroutines and predication aren't handled.
	struct Branch
	{
		Taint before, thenEnd;
		bool inElse = false;
	};
	std::vector<Branch> branches;
	// Taints of every instruction's sources at the point it runs, and its branch depth, for tracing
	// the albedo back.
	std::vector<Taint> history(insts.size());
	std::vector<int> depthAt(insts.size());
	for (size_t i = 0; i < insts.size(); i++)
	{
		const Inst& in = insts[i];
		history[i] = t;
		depthAt[i] = (int)branches.size();
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
			int form = -1;
			if (in.op == OP_MAD && sunOnly(st[0] | st[1]) && ambOnly(st[2]) && SrcMod(p[3]) == 0)
			{
				form = ssgi::GI_FORM_SUN_ALBEDO;
				f.addSrc = 3;
				f.indirect[0] = f.ambSrc = p[3];
				f.indirect[1] = 0;
				f.sun[0] = p[1];
				f.sun[1] = p[2];
				f.sunSrc = (st[0] & T_SH) ? p[1] : p[2];
			}
			else if (in.op == OP_MAD && ambOnly(st[0] | st[1]) && sunOnly(st[2]) && !SrcMod(p[1]) && !SrcMod(p[2]))
			{
				form = ssgi::GI_FORM_AMB_PRODUCT;
				f.addSrc = 0;
				f.indirect[0] = p[1];
				f.indirect[1] = p[2];
				f.ambSrc = (st[0] & T_AMB) ? p[1] : p[2];
				f.sun[0] = f.sunSrc = p[3];
				f.sun[1] = 0;
			}
			else if (in.op == OP_ADD && ((sunOnly(st[0]) && ambOnly(st[1]) && SrcMod(p[2]) == 0) || (sunOnly(st[1]) && ambOnly(st[0]) && SrcMod(p[1]) == 0)))
			{
				form = ssgi::GI_FORM_ADD;
				f.addSrc = sunOnly(st[0]) ? 2 : 1;
				f.indirect[0] = f.ambSrc = p[f.addSrc];
				f.indirect[1] = 0;
				f.sun[0] = f.sunSrc = p[3 - f.addSrc];
				f.sun[1] = 0;
			}
			if (form >= 0)
			{
				haveComb = true;
				f.combI = i;
				f.form = form;
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
					f.fogSrc = p[1]; // oC0 = lit + fog * (fogColour - lit)
					f.fogNegatable = NegateSrc(p[1], neg);
				}
				else if ((in.op == OP_MOV && litMask == 2 && IdentityXyz(p[1])) || (in.op == OP_MAD && litMask == 8 && IdentityXyz(p[3])) ||
						 (in.op == OP_ADD && litSrc == 1 && IdentityXyz(p[litMask == 2 ? 1 : 2]) && SrcMod(p[litMask == 2 ? 1 : 2]) == 0))
					f.fogSrc = 0; // no fog: added terms only
				else
				{
					why = "final colour not recognised";
					return false;
				}
				haveFinal = true;
				f.finalI = i;
			}
			else if (litSrc)
			{
				// Between combine and final the lit colour may only have things added to it (so what
				// is captured or added at the combine reaches the final colour unscaled).
				bool ok = (in.op == OP_MOV && litSrc == 1) || (in.op == OP_ADD && litSrc == 1) || (in.op == OP_MAD && litMask == 8);
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
	FindAlbedo(code, insts, history, depthAt, f);
	return true;
}

// The shading normal (see the top): the token index of "mul sq.xyz, N, N" and its source N.
bool FindNormal(const DWORD* code, const std::vector<Inst>& insts, const core::ConstantTable& vars, size_t combI, size_t& normalI,
	DWORD& normalSrc)
{
	const core::ShaderConstant* amb = vars.Find("g_AmbientCube", core::RS_FLOAT4);
	if (!amb)
		return false;
	const int lo = amb->reg, hi = amb->reg + amb->count;
	auto isCube = [&](DWORD t) { return RegType(t) == REG_CONST && !Relative(t) && RegNum(t) >= lo && RegNum(t) < hi; };
	// Straight-line neighbourhood of an instruction: no flow control or branch boundary in between.
	auto plain = [&](const Inst& in) {
		return !(IsFlowControl(in.op) || in.op == OP_IF || in.op == OP_IFC || in.op == OP_ELSE || in.op == OP_ENDIF || in.predicated);
	};
	int found = 0, depth = 0;
	for (size_t j = 0; j < combI; j++)
	{
		const Inst& in = insts[j];
		if (in.op == OP_IF || in.op == OP_IFC || in.op == OP_REP || in.op == OP_LOOP)
			depth++;
		else if (in.op == OP_ENDIF || in.op == OP_ENDREP || in.op == OP_ENDLOOP)
			depth--;
		if (depth != 0 || in.op != OP_MUL || in.predicated || in.len != 3)
			continue;
		const DWORD* p = &code[in.pos + 1];
		const DWORD n = p[1];
		if (RegType(p[0]) != REG_TEMP || (DstMask(p[0]) & (X | Y | Z)) != (X | Y | Z) || p[2] != n || RegType(n) != REG_TEMP ||
			Relative(n) || SrcMod(n) != 0 || !IdentityXyz(n) || (p[0] & 0x00F00000u))
			continue;
		const int sq = RegNum(p[0]), nReg = RegNum(n);
		// Every square weighs a g_AmbientCube face, read before the squares are overwritten.
		int weighed = 0;
		for (size_t k = j + 1; k < insts.size() && plain(insts[k]); k++)
		{
			const Inst& u = insts[k];
			const DWORD* q = &code[u.pos + 1];
			if ((u.op == OP_MUL || u.op == OP_MAD) && u.len >= 3)
				for (int s = 1; s <= 2; s++)
				{
					DWORD a = q[s], b = q[3 - s];
					if (RegType(a) == REG_TEMP && RegNum(a) == sq && !Relative(a) && SrcMod(a) == 0 && isCube(b) &&
						SwzComp(a, 0) == SwzComp(a, 1) && SwzComp(a, 1) == SwzComp(a, 2) && SwzComp(a, 2) == SwzComp(a, 3) && SwzComp(a, 0) < 3)
						weighed |= 1 << SwzComp(a, 0);
				}
			if (WritesTemp(code, u, sq, X | Y | Z))
				break;
		}
		if (weighed != 7)
			continue;
		// The same value of N sign-tested by a cmp: back to the instruction that wrote it, and on to
		// the next that writes it (none after j if the square is done in place).
		bool signs = false;
		auto signTest = [&](const Inst& u) {
			const DWORD* q = &code[u.pos + 1];
			return u.op == OP_CMP && RegType(q[1]) == REG_TEMP && RegNum(q[1]) == nReg && !Relative(q[1]) && IdentityXyz(q[1]) &&
				(SrcMod(q[1]) == 0 || SrcMod(q[1]) == 1);
		};
		for (size_t k = j; k-- > 0 && !signs && plain(insts[k]);)
		{
			if (WritesTemp(code, insts[k], nReg, X | Y | Z))
				break;
			signs = signTest(insts[k]);
		}
		if (!WritesTemp(code, in, nReg, X | Y | Z))
			for (size_t k = j + 1; k < insts.size() && !signs && plain(insts[k]); k++)
			{
				signs = signTest(insts[k]);
				if (WritesTemp(code, insts[k], nReg, X | Y | Z))
					break;
			}
		if (!signs)
			continue;
		found++;
		normalI = j;
		normalSrc = n;
	}
	return found == 1;
}

// The instructions computing oC1.x (the z/w target): the write itself and at most one temp it reads,
// computed from inputs and constants only. Returns false if they're more involved.
bool FindDepthSlice(const DWORD* code, const std::vector<Inst>& insts, size_t& writeI, size_t& defI, bool& haveDef)
{
	haveDef = false;
	int depth = 0;
	bool found = false;
	for (size_t i = 0; i < insts.size(); i++)
	{
		const Inst& in = insts[i];
		if (in.op == OP_IF || in.op == OP_IFC || in.op == OP_REP || in.op == OP_LOOP)
			depth++;
		else if (in.op == OP_ENDIF || in.op == OP_ENDREP || in.op == OP_ENDLOOP)
			depth--;
		if (!WritesDst(in))
			continue;
		DWORD d = code[in.pos + 1];
		if (RegType(d) == REG_COLOROUT && RegNum(d) == 1 && (DstMask(d) & X))
		{
			if (depth != 0 || in.predicated)
				return false;
			writeI = i;
			found = true; // the last one counts
		}
	}
	if (!found)
		return false;
	const Inst& W = insts[writeI];
	if (W.op == OP_TEXLD || W.op == OP_TEXLDL || W.op == OP_TEXLDD)
		return false;
	int temps = 0, tempReg = -1, tempComp = -1;
	for (size_t s = 2; s <= W.len; s++)
	{
		DWORD t = code[W.pos + s];
		int type = RegType(t);
		if (Relative(t))
			return false;
		if (type == REG_TEMP)
		{
			if (temps++ && (RegNum(t) != tempReg || SwzComp(t, 0) != tempComp))
				return false;
			tempReg = RegNum(t);
			tempComp = SwzComp(t, 0);
		}
		else if (type != REG_INPUT && type != REG_CONST)
			return false;
	}
	if (!temps)
		return true;
	// The last writer of that temp component before the write, with nothing but straight-line code between.
	for (size_t j = writeI; j-- > 0;)
	{
		const Inst& in = insts[j];
		if (IsFlowControl(in.op) || in.op == OP_IF || in.op == OP_IFC || in.op == OP_ELSE || in.op == OP_ENDIF || in.predicated)
			return false;
		if (!WritesTemp(code, in, tempReg, 1 << tempComp))
			continue;
		if (in.op == OP_TEXLD || in.op == OP_TEXLDL || in.op == OP_TEXLDD)
			return false;
		for (size_t s = 2; s <= in.len; s++)
		{
			DWORD t = code[in.pos + s];
			if (Relative(t) || (RegType(t) != REG_INPUT && RegType(t) != REG_CONST))
				return false;
		}
		defI = j;
		haveDef = true;
		return true;
	}
	return false;
}

DWORD WithDst(DWORD orig, int type, int num, int mask)
{
	// Keeps the result modifier (_sat) and shift bits of the original destination.
	return (orig & 0x0FF00000u) | Dst(type, num, mask);
}

DWORD Renamed(DWORD src, int fromReg, int toReg)
{
	if (RegType(src) != REG_TEMP || RegNum(src) != fromReg)
		return src;
	return (src & ~0x7FFu) | (DWORD)toReg;
}

// The highest constant register at or below `top` that is free (not used, not reserved), or -1.
int FreeConst(const bool used[256], int top, int reserveLo = -1, int reserveHi = -1, int reserve2 = -1)
{
	for (int r = top; r >= 0; r--)
		if (!used[r] && !(r >= reserveLo && r <= reserveHi) && r != reserve2)
			return r;
	return -1;
}
} // namespace

bool Lit_RewritePS(const DWORD* code, LitParts parts, std::vector<DWORD>& result, LitRewriteResult& r)
{
	r = LitRewriteResult();
	if (parts.gi)
		ssgi::g_giLastLayout = ssgi::GiRewriteLayout();
	bool ao = parts.ao, gi = parts.gi;
	auto failBoth = [&](const char* aoWhy, const char* giWhy) {
		r.aoWhy = aoWhy;
		r.giWhy = giWhy;
		return false;
	};
	std::vector<Inst> insts;
	size_t endPos = 0;
	if (!code || !Parse(code, insts, endPos))
		return failBoth("not ps_3_0", "not ps_3_0");
	bool writesOC1 = false, writesOC2 = false, hasVPos = false;
	for (const Inst& in : insts)
	{
		if (WritesDst(in) && RegType(code[in.pos + 1]) == REG_COLOROUT)
		{
			int n = RegNum(code[in.pos + 1]);
			writesOC1 |= n == 1;
			writesOC2 |= n >= 2;
		}
		if (in.op == OP_DCL && in.len == 2)
		{
			DWORD reg = code[in.pos + 2];
			if (RegType(reg) == REG_SAMPLER && RegNum(reg) == GI_SAMPLER && gi)
			{
				gi = false;
				r.giWhy = "sampler s13 already used";
			}
			if (RegType(reg) == REG_MISC && RegNum(reg) == 0)
			{
				if ((DstMask(reg) & (X | Y)) != (X | Y) && gi)
				{
					gi = false;
					r.giWhy = "vPos declared without .xy";
				}
				hasVPos = true;
			}
		}
	}
	if (!writesOC1)
		return failBoth("single render target", "not an opaque pass shader");
	if (writesOC2 && ao)
	{
		ao = false;
		r.aoWhy = "already writes oC2";
	}
	if (!ao && !gi)
		return false;
	core::ConstantTable vars;
	core::ReadConstantTable(code, vars);
	int maxTemp;
	bool constUsed[256];
	if (!RegisterUsage(code, insts, vars, maxTemp, constUsed))
		return failBoth("relative constant addressing", "relative constant addressing");
	for (int c = GI_CONST; c < GI_CONST + 3 && gi; c++)
		if (constUsed[c])
		{
			gi = false;
			r.giWhy = "constants c216-c218 already used";
		}

	Lighting L;
	std::string why;
	const bool found = AnalyseLighting(code, insts, vars, L, why);
	bool aoMatched = ao && found && L.fogNegatable;
	if (ao && !aoMatched)
		r.aoWhy = !found ? why : "unusual fog factor";
	if (gi && !(found && L.albedoFound))
	{
		gi = false;
		r.giWhy = !found ? why : L.albedoWhy;
	}

	// Constants: AO's highest free one at or below c223 (clear of SSGI's c216-c218), SSGI's below c216.
	const int Ka = ao ? FreeConst(constUsed, 223, gi ? GI_CONST : -1, gi ? GI_CONST + 2 : -1) : -1;
	if (ao && Ka < 0)
	{
		ao = aoMatched = false;
		r.aoWhy = "no free constant register";
	}
	const int Kg = gi ? FreeConst(constUsed, GI_CONST - 1, -1, -1, Ka) : -1;
	if (gi && Kg < 0)
	{
		gi = false;
		r.giWhy = "no free constant register";
	}

	// Temps. SSGI: G, Q, A (the albedo copy, if needed). AO alone: T, S. Both: T reuses Q (dead after
	// the reprojection) and S takes A's place (dead after the bounce light is added), so 3.
	const int base = maxTemp + 1;
	int G = -1, Q = -1, A = -1, T = -1, S = -1;
	if (ao && gi && base + 2 > 31)
	{
		gi = false; // keep AO, the shader's first job in this plugin
		r.giWhy = "no free temp register";
	}
	if (gi)
	{
		G = base;
		Q = base + 1;
		A = L.albedoCopy ? base + 2 : -1;
		if ((L.albedoCopy ? A : Q) > 31)
		{
			gi = false;
			r.giWhy = "no free temp register";
		}
	}
	if (ao)
	{
		T = gi ? Q : base;
		S = gi ? base + 2 : base + 1;
		if (S > 31)
		{
			ao = aoMatched = false;
			r.aoWhy = "no free temp register";
		}
	}
	if (!ao && !gi)
		return false;
	// The shading normal, with the AO outputs that carry it: N after S.
	size_t normalI = 0;
	DWORD normalSrc = 0;
	const int N = S + 1;
	const bool normal = aoMatched && N <= 31 && FindNormal(code, insts, vars, L.combI, normalI, normalSrc);
	r.normal = normal;

	size_t writeI = 0, defI = 0;
	bool haveDef = false;
	// Its sources are inputs and constants, so the copy can run before the combine.
	const bool depthExact = gi && FindDepthSlice(code, insts, writeI, defI, haveDef);

	std::vector<DWORD> out;
	out.reserve(endPos + 96);
	Emitter e{ out };
	const size_t firstReal = FirstRealInstruction(insts, endPos);
	out.insert(out.end(), code, code + firstReal);
	size_t cursor = firstReal;
	bool normalDue = normal; // "mov rN.xyz, N" still to insert before the square
	auto copyTo = [&](size_t pos) {
		const size_t at = insts[normalI].pos;
		if (normalDue && at >= cursor && at <= pos)
		{
			out.insert(out.end(), code + cursor, code + at);
			cursor = at;
			e.I(OP_MOV, { Dst(REG_TEMP, N, X | Y | Z), normalSrc });
			normalDue = false;
		}
		out.insert(out.end(), code + cursor, code + pos);
		cursor = pos;
	};

	// Declarations: SSGI's vPos, GI sampler and Kg; AO's Ka.
	if (gi)
	{
		if (!hasVPos)
			e.I(OP_DCL, { 0x80000000u, Dst(REG_MISC, 0, X | Y) });
		e.I(OP_DCL, { 0x90000000u, Dst(REG_SAMPLER, GI_SAMPLER, 0xF) });
		e.I(OP_DEF, { Dst(REG_CONST, Kg, 0xF), FloatBits(1.0f), FloatBits(0.0f), FloatBits(0.0f), FloatBits(0.0f) });
	}
	if (ao)
		e.I(OP_DEF, { Dst(REG_CONST, Ka, 0xF), FloatBits(0.0f), FloatBits(1.0f), FloatBits(2.0f), FloatBits(1.0f / 1048576.0f) });

	if (ao && !aoMatched)
	{
		// oC2 = oC3 = 0, after the declarations (they must come first).
		size_t at = firstReal;
		for (const Inst& in : insts)
		{
			if (in.pos < firstReal || in.op == OP_COMMENT || in.op == OP_DCL || in.op == OP_DEF || in.op == OP_DEFI || in.op == OP_DEFB)
				continue;
			at = in.pos;
			break;
		}
		copyTo(at);
		e.I(OP_MOV, { Dst(REG_COLOROUT, 2, 0xF), Src(REG_CONST, Ka, Rep(0)) });
		e.I(OP_MOV, { Dst(REG_COLOROUT, 3, 0xF), Src(REG_CONST, Ka, Rep(0)) });
	}

	// AO: the indirect and sun terms into T and S, read from the combine's sources.
	auto captureAo = [&](bool indirect, bool sun) {
		const DWORD* terms[2] = { L.indirect, L.sun };
		const int regs[2] = { T, S };
		for (int k = 0; k < 2; k++)
		{
			if (!(k == 0 ? indirect : sun))
				continue;
			if (terms[k][1])
				e.I(OP_MUL, { Dst(REG_TEMP, regs[k], X | Y | Z), terms[k][0], terms[k][1] });
			else
				e.I(OP_MOV, { Dst(REG_TEMP, regs[k], X | Y | Z), terms[k][0] });
		}
	};

	if (aoMatched || gi)
	{
		const Inst& C = insts[L.combI];
		const DWORD* cp = &code[C.pos + 1];
		const size_t combLen = 1 + C.len;
		DWORD albedo = L.albedo;
		if (gi && L.albedoCopy)
		{
			// The albedo register changes before the combine: keep a copy from where it is read.
			copyTo(insts[L.albedoI].pos);
			e.I(OP_MOV, { Dst(REG_TEMP, A, X | Y | Z), L.albedo });
			albedo = Src(REG_TEMP, A, kIdentity);
		}
		copyTo(C.pos);
		if (!gi)
			captureAo(true, true); // the combine follows unchanged
		else
		{
			// Q.x = z / w
			if (depthExact)
			{
				const Inst& Wr = insts[writeI];
				int fromReg = -1;
				if (haveDef)
				{
					const Inst& D = insts[defI];
					fromReg = RegNum(code[D.pos + 1]);
					std::vector<DWORD> inst(code + D.pos, code + D.pos + 1 + D.len);
					inst[1] = WithDst(inst[1], REG_TEMP, G, DstMask(inst[1]));
					out.insert(out.end(), inst.begin(), inst.end());
				}
				std::vector<DWORD> inst(code + Wr.pos, code + Wr.pos + 1 + Wr.len);
				inst[1] = WithDst(inst[1], REG_TEMP, Q, X);
				for (size_t s = 2; s < inst.size(); s++)
					inst[s] = Renamed(inst[s], fromReg, G);
				out.insert(out.end(), inst.begin(), inst.end());
			}
			else
				e.I(OP_MOV, { Dst(REG_TEMP, Q, X), Src(REG_CONST, Kg, Rep(0)) }); // far away: rotation-only reprojection
			e.I(OP_MOV, { Dst(REG_TEMP, Q, Y | Z), Src(REG_MISC, 0, Swz(0, 0, 1, 1)) });
			e.I(OP_MOV, { Dst(REG_TEMP, Q, W), Src(REG_CONST, Kg, Rep(0)) });
			e.I(OP_DP4, { Dst(REG_TEMP, G, X), Src(REG_TEMP, Q, kIdentity), Src(REG_CONST, GI_CONST + 0, kIdentity) });
			e.I(OP_DP4, { Dst(REG_TEMP, G, Y), Src(REG_TEMP, Q, kIdentity), Src(REG_CONST, GI_CONST + 1, kIdentity) });
			e.I(OP_DP4, { Dst(REG_TEMP, G, W), Src(REG_TEMP, Q, kIdentity), Src(REG_CONST, GI_CONST + 2, kIdentity) });
			e.I(OP_RCP, { Dst(REG_TEMP, G, W), Src(REG_TEMP, G, Rep(3)) });
			e.I(OP_MUL, { Dst(REG_TEMP, G, X | Y), Src(REG_TEMP, G, kIdentity), Src(REG_TEMP, G, Rep(3)) });
			e.I(OP_TEXLD, { Dst(REG_TEMP, G, 0xF), Src(REG_TEMP, G, kIdentity), Src(REG_SAMPLER, GI_SAMPLER, kIdentity) });
			if (L.form == ssgi::GI_FORM_AMB_PRODUCT)
			{
				// lit = a * b + sun  ->  P = a * b ; G = albedo * G + P ; lit = G + sun
				// (with AO, P is T, the baked indirect light AO keeps; else Q)
				const int P = aoMatched ? T : Q;
				e.I(OP_MUL, { Dst(REG_TEMP, P, X | Y | Z), cp[1], cp[2] });
				e.I(OP_MAD, { Dst(REG_TEMP, aoMatched ? G : Q, X | Y | Z), albedo, Src(REG_TEMP, G, kIdentity), Src(REG_TEMP, P, kIdentity) });
				if (aoMatched)
					captureAo(false, true);
				e.I(OP_ADD, { cp[0], Src(REG_TEMP, aoMatched ? G : Q, kIdentity), cp[3] });
			}
			else
			{
				// indirect' = albedo * G + indirect, read by the combine in place of indirect. AO's
				// capture comes after it: the game's indirect term is still in its own register.
				e.I(OP_MAD, { Dst(REG_TEMP, G, X | Y | Z), albedo, Src(REG_TEMP, G, kIdentity), cp[L.addSrc] });
				if (aoMatched)
					captureAo(true, true);
				std::vector<DWORD> inst(code + C.pos, code + C.pos + combLen);
				inst[1 + L.addSrc] = Src(REG_TEMP, G, kIdentity);
				out.insert(out.end(), inst.begin(), inst.end());
			}
			cursor = C.pos + combLen;
		}
	}

	if (aoMatched)
	{
		copyTo(insts[L.finalI].pos);
		const int regs[2] = { T, S };
		for (int k = 0; k < 2; k++)
		{
			const int R = regs[k];
			if (L.fogSrc)
			{
				DWORD negFog = 0;
				NegateSrc(L.fogSrc, negFog);
				e.I(OP_MAD, { Dst(REG_TEMP, R, X | Y | Z), Src(REG_TEMP, R, kIdentity), negFog, Src(REG_TEMP, R, kIdentity) });
			}
			if (!normal)
				e.I(OP_MOV, { Dst(REG_TEMP, R, W), Src(REG_CONST, Ka, Rep(0)) });
		}
		if (normal)
		{
			// rT.w, rS.w = octahedral N + 2 (see the top); rN.w is scratch.
			auto mod = [](DWORD src, int m) { return (src & ~0x0F000000u) | ((DWORD)m << 24); };
			const DWORD nXyz = Src(REG_TEMP, N, kIdentity), one = Src(REG_CONST, Ka, Rep(1)), minusOne = Src(REG_CONST, Ka, Rep(1), true);
			auto n = [&](int c) { return Src(REG_TEMP, N, Rep(c)); };
			const DWORD tw = Dst(REG_TEMP, T, W), sw = Dst(REG_TEMP, S, W), nw = Dst(REG_TEMP, N, W);
			const DWORD tW = Src(REG_TEMP, T, Rep(3)), sW = Src(REG_TEMP, S, Rep(3)), nW = n(3);
			e.I(OP_DP3, { nw, mod(nXyz, 0xB), one });                             // |x| + |y| + |z|
			e.I(OP_MAX, { nw, nW, Src(REG_CONST, Ka, Rep(3)) });                  // (a zero normal stays finite)
			e.I(OP_RCP, { nw, nW });
			e.I(OP_MUL, { Dst(REG_TEMP, N, X | Y | Z), nXyz, nW });               // p
			e.I(OP_CMP, { tw, n(0), one, minusOne });                             // sign(p.x)
			e.I(OP_ADD, { sw, mod(n(1), 0xC), one });                             // 1 - |p.y|
			e.I(OP_MUL, { tw, tW, sW });
			e.I(OP_CMP, { sw, n(1), one, minusOne });                             // sign(p.y)
			e.I(OP_ADD, { nw, mod(n(0), 0xC), one });                             // 1 - |p.x|
			e.I(OP_MUL, { sw, sW, nW });
			e.I(OP_CMP, { tw, n(2), n(0), tW });                                  // upper half: p, lower: folded
			e.I(OP_CMP, { sw, n(2), n(1), sW });
			e.I(OP_ADD, { tw, tW, Src(REG_CONST, Ka, Rep(2)) });
			e.I(OP_ADD, { sw, sW, Src(REG_CONST, Ka, Rep(2)) });
		}
		e.I(OP_MOV, { Dst(REG_COLOROUT, 2, 0xF), Src(REG_TEMP, T, kIdentity) });
		e.I(OP_MOV, { Dst(REG_COLOROUT, 3, 0xF), Src(REG_TEMP, S, kIdentity) });
	}
	copyTo(endPos + 1);

	if (ao)
	{
		if (aoMatched)
		{
			g_aoLastLayout.combineAt = insts[L.combI].pos;
			g_aoLastLayout.addend = L.ambSrc;
			g_aoLastLayout.sunSrc = L.sunSrc;
			g_aoLastLayout.finalAt = insts[L.finalI].pos;
		}
		g_aoLastLayout.matched = aoMatched;
		g_aoLastLayout.normal = normal;
		g_aoLastLayout.normalTemp = normal ? N : -1;
		g_aoLastLayout.normalAt = normal ? insts[normalI].pos : 0;
		g_aoLastLayout.normalSrc = normal ? normalSrc : 0;
		g_aoLastLayout.temp = T;
		g_aoLastLayout.sunTemp = S;
		g_aoLastLayout.constReg = Ka;
	}
	if (gi)
	{
		ssgi::GiRewriteLayout& g = ssgi::g_giLastLayout;
		g.form = L.form;
		g.combineAt = insts[L.combI].pos;
		g.albedo = L.albedo;
		g.albedoAt = insts[L.albedoI].pos;
		g.albedoCopied = L.albedoCopy;
		g.depthExact = depthExact;
		g.temp = G;
		g.temp2 = Q;
		g.constReg = Kg;
	}
	r.ao = ao;
	r.aoMatched = aoMatched;
	r.gi = gi;
	result.swap(out);
	return true;
}

// ---- The single-part entry points ---------------------------------------------------------------

bool AO_RewritePS(const DWORD* code, std::vector<DWORD>& out, bool& matched, std::string& reason)
{
	LitParts p;
	p.ao = true;
	LitRewriteResult r;
	bool ok = Lit_RewritePS(code, p, out, r) && r.ao;
	matched = r.aoMatched;
	reason = r.aoWhy;
	return ok;
}

namespace ssgi
{
bool GI_RewritePS(const DWORD* code, std::vector<DWORD>& out, std::string& reason)
{
	LitParts p;
	p.gi = true;
	LitRewriteResult r;
	bool ok = Lit_RewritePS(code, p, out, r) && r.gi;
	reason = r.giWhy;
	return ok;
}
} // namespace ssgi
