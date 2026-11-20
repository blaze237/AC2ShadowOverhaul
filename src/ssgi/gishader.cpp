// SSGI shader rewrite: adds one-bounce indirect light to the game's own lighting.
//
// AC2's lit pixel shaders compute (with many compiled variations, see the F8 frame dump exports)
//     indirect = lerp(g_AmbientCube(normal) * albedo, lightmap.rgb * albedo, w)
//     lit      = sunDirect * shadow * albedo + indirect                         <- "combine"
//     oC0.xyz  = lit + fogFactor * (fogColour - lit)                            <- "final"
//     oC1      = z / w                                                          (opaque pass only)
// The combine is found with the same dataflow taints as AC2ShadowOverhaul's AO output: AMB (from
// g_AmbientCube or the world lightmap) and SH (from the shadow or cloud shadow map). It is the first
// mad / add that adds a term carrying only AMB to one carrying only SH. The albedo is the factor
// the sun light is multiplied by: the operand of the sun product that carries neither taint.
//
// Inserted right before the combine (fresh temps G, Q; constant K = (1, 0, 0, 0)):
//     Q.x  = z / w            (a copy of the instructions that compute oC1.x; 1 if they can't be copied)
//     Q.yz = vPos.xy, Q.w = 1
//     G.xyw = Q . c216, Q . c217, Q . c218     (reprojection into the GI texture of the last frame)
//     G     = tex2D(s13, G.xy / G.w)
//     indirect' = albedo * G + indirect
// and the combine reads indirect' instead. Everything after it (other light, fog) is unchanged, so the
// bounce light is fogged and (with AC2ShadowOverhaul) occluded exactly like the game's ambient light.
// A shader with no GI texture bound samples black and renders exactly as before.

#include "gi.h"
#include "sm3.h"
#include <stdio.h>

namespace ssgi
{

GiRewriteLayout g_giLastLayout;

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

bool IdentityXyz(DWORD s) { return SwzComp(s, 0) == 0 && SwzComp(s, 1) == 1 && SwzComp(s, 2) == 2; }

// What the analysis found.
struct Found
{
	size_t combI = 0; // index into insts
	int form = -1;    // GI_FORM_*
	DWORD albedo = 0;   // source token, as read by instruction albedoI
	size_t albedoI = 0; // the combine, or an earlier product the albedo is read from
	bool copy = false;  // its register changes before the combine: copy it at albedoI
	int addSrc = 0;     // form SUN_ALBEDO / ADD: which combine source (1..3) holds the indirect light
};

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

// Finds the combine and the albedo. Mirrors AC2ShadowOverhaul's FindIndirect, so both mods agree on
// which shaders and which term.
bool FindCombine(const DWORD* code, const std::vector<Inst>& insts, const core::ConstantTable& vars, Found& f, std::string& why)
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
	// Taints of every instruction's sources at the point it runs (top level only), for tracing the
	// sun term of an add back to its product.
	struct Branch
	{
		Taint before, thenEnd;
		bool inElse = false;
	};
	std::vector<Branch> branches;
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
			const int nIn = in.op == OP_MAD ? 3 : 2;
			uint8_t st[3] = {};
			for (int s = 0; s < nIn; s++)
				for (int k = 0; k < 3; k++)
					st[s] |= t.Of(p[1 + s], k);
			auto sunOnly = [](uint8_t v) { return (v & T_SH) && !(v & (T_AMB | T_LIT)); };
			auto ambOnly = [](uint8_t v) { return (v & T_AMB) && !(v & (T_SH | T_LIT)); };
			int form = -1, addSrc = 0;
			if (in.op == OP_MAD && sunOnly(st[0] | st[1]) && ambOnly(st[2]) && SrcMod(p[3]) == 0)
				form = GI_FORM_SUN_ALBEDO, addSrc = 3;
			else if (in.op == OP_MAD && ambOnly(st[0] | st[1]) && sunOnly(st[2]) && !SrcMod(p[1]) && !SrcMod(p[2]))
				form = GI_FORM_AMB_PRODUCT;
			else if (in.op == OP_ADD && sunOnly(st[0]) && ambOnly(st[1]) && SrcMod(p[2]) == 0)
				form = GI_FORM_ADD, addSrc = 2;
			else if (in.op == OP_ADD && sunOnly(st[1]) && ambOnly(st[0]) && SrcMod(p[1]) == 0)
				form = GI_FORM_ADD, addSrc = 1;
			if (form >= 0)
			{
				haveComb = true;
				f.combI = i;
				f.form = form;
				f.addSrc = addSrc;
				for (int k = 0; k < 4; k++)
					res[k] |= T_LIT;
			}
		}
		else if (haveComb)
		{
			// Between the combine and the final colour the lit colour may only have things added to it
			// (so the bounce light we add to the indirect term isn't scaled by anything else).
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
				if (!branches.empty() || haveFinal || !litSrc || (DstMask(d) & (X | Y | Z)) != (X | Y | Z))
				{
					why = "final colour not recognised";
					return false;
				}
				bool fog = in.op == OP_MAD && litMask == ((1 << 2) | (1 << 3)) && IdentityXyz(p[3]); // lit + fog * (fogColour - lit)
				bool plain = (in.op == OP_MOV && litMask == 2 && IdentityXyz(p[1])) || (in.op == OP_MAD && litMask == 8 && IdentityXyz(p[3])) ||
					(in.op == OP_ADD && litSrc == 1 && IdentityXyz(p[litMask == 2 ? 1 : 2]) && SrcMod(p[litMask == 2 ? 1 : 2]) == 0);
				if (!fog && !plain)
				{
					why = "final colour not recognised";
					return false;
				}
				haveFinal = true;
			}
			else if (litSrc)
			{
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

	// The albedo.
	const Inst& C = insts[f.combI];
	const DWORD* p = &code[C.pos + 1];
	const Taint& tc = history[f.combI];
	auto plainFactor = [&](const Taint& tt, DWORD s) { return tt.Xyz(s) == 0; };
	if (f.form == GI_FORM_SUN_ALBEDO)
	{
		// mad lit, a, b, indirect with a * b = sun light: the albedo is the factor without the shadow.
		bool a = plainFactor(tc, p[1]), b = plainFactor(tc, p[2]);
		if (a == b)
		{
			why = a ? "sun term has no shadow factor" : "albedo not separable from the sun term";
			return false;
		}
		f.albedo = a ? p[1] : p[2];
		f.albedoI = f.combI;
		return true;
	}
	if (f.form == GI_FORM_AMB_PRODUCT)
	{
		// mad lit, a, b, sun with a * b = indirect light: the albedo is the factor without the ambient light.
		bool a = plainFactor(tc, p[1]), b = plainFactor(tc, p[2]);
		if (a == b)
		{
			why = "albedo not separable from the indirect term";
			return false;
		}
		f.albedo = a ? p[1] : p[2];
		f.albedoI = f.combI;
		return true;
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
			f.copy = !StillHolds(code, insts, f.albedo, j, f.combI);
			return true;
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
		f.copy = !StillHolds(code, insts, f.albedo, j, f.combI);
		return true;
	}
	why = "albedo not traceable";
	return false;
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
} // namespace

bool GI_RewritePS(const DWORD* code, std::vector<DWORD>& result, std::string& reason)
{
	reason.clear();
	g_giLastLayout = GiRewriteLayout();
	std::vector<Inst> insts;
	size_t endPos = 0;
	if (!code || !Parse(code, insts, endPos))
	{
		reason = "not ps_3_0";
		return false;
	}
	bool writesOC1 = false, hasVPos = false;
	for (const Inst& in : insts)
	{
		if (WritesDst(in) && RegType(code[in.pos + 1]) == REG_COLOROUT && RegNum(code[in.pos + 1]) == 1)
			writesOC1 = true;
		if (in.op == OP_DCL && in.len == 2)
		{
			DWORD r = code[in.pos + 2];
			if (RegType(r) == REG_SAMPLER && RegNum(r) == GI_SAMPLER)
			{
				reason = "sampler s13 already used";
				return false;
			}
			if (RegType(r) == REG_MISC && RegNum(r) == 0)
			{
				if ((DstMask(r) & (X | Y)) != (X | Y))
				{
					reason = "vPos declared without .xy";
					return false;
				}
				hasVPos = true;
			}
		}
	}
	if (!writesOC1)
	{
		reason = "not an opaque pass shader";
		return false;
	}
	core::ConstantTable vars;
	core::ReadConstantTable(code, vars);
	int maxTemp;
	bool constUsed[256];
	if (!RegisterUsage(code, insts, vars, maxTemp, constUsed))
	{
		reason = "relative constant addressing";
		return false;
	}
	for (int r = GI_CONST; r < GI_CONST + 3; r++)
		if (constUsed[r])
		{
			reason = "constants c216-c218 already used";
			return false;
		}
	const int G = maxTemp + 1, Q = maxTemp + 2, A = maxTemp + 3;
	int K = -1;
	for (int r = GI_CONST - 1; r >= 0 && K < 0; r--)
		if (!constUsed[r])
			K = r;
	if (K < 0)
	{
		reason = "no free constant register";
		return false;
	}

	Found f;
	if (!FindCombine(code, insts, vars, f, reason))
		return false;
	if ((f.copy ? A : Q) > 31)
	{
		reason = "no free temp register";
		return false;
	}
	size_t writeI = 0, defI = 0;
	bool haveDef = false;
	// Its sources are inputs and constants, so the copy can run before the combine.
	bool depthExact = FindDepthSlice(code, insts, writeI, defI, haveDef);

	const Inst& C = insts[f.combI];
	const DWORD* cp = &code[C.pos + 1];
	std::vector<DWORD> out;
	out.reserve(endPos + 64);
	Emitter e{ out };
	const size_t firstReal = FirstRealInstruction(insts, endPos);
	out.insert(out.end(), code, code + firstReal);
	// Declarations: vPos, the GI sampler, K.
	if (!hasVPos)
		e.I(OP_DCL, { 0x80000000u, Dst(REG_MISC, 0, X | Y) });
	e.I(OP_DCL, { 0x90000000u, Dst(REG_SAMPLER, GI_SAMPLER, 0xF) });
	e.I(OP_DEF, { Dst(REG_CONST, K, 0xF), FloatBits(1.0f), FloatBits(0.0f), FloatBits(0.0f), FloatBits(0.0f) });
	DWORD albedo = f.albedo;
	if (f.copy)
	{
		// The albedo register changes before the combine: keep a copy from where it is read.
		const size_t at = insts[f.albedoI].pos;
		out.insert(out.end(), code + firstReal, code + at);
		e.I(OP_MOV, { Dst(REG_TEMP, A, X | Y | Z), f.albedo });
		out.insert(out.end(), code + at, code + C.pos);
		albedo = Src(REG_TEMP, A, kIdentity);
	}
	else
		out.insert(out.end(), code + firstReal, code + C.pos);

	// Q.x = z / w
	if (depthExact)
	{
		const Inst& W = insts[writeI];
		int fromReg = -1;
		if (haveDef)
		{
			const Inst& D = insts[defI];
			fromReg = RegNum(code[D.pos + 1]);
			std::vector<DWORD> inst(code + D.pos, code + D.pos + 1 + D.len);
			inst[1] = WithDst(inst[1], REG_TEMP, G, DstMask(inst[1]));
			out.insert(out.end(), inst.begin(), inst.end());
		}
		std::vector<DWORD> inst(code + W.pos, code + W.pos + 1 + W.len);
		inst[1] = WithDst(inst[1], REG_TEMP, Q, X);
		for (size_t s = 2; s < inst.size(); s++)
			inst[s] = Renamed(inst[s], fromReg, G);
		out.insert(out.end(), inst.begin(), inst.end());
	}
	else
		e.I(OP_MOV, { Dst(REG_TEMP, Q, X), Src(REG_CONST, K, Rep(0)) }); // far away: rotation-only reprojection
	e.I(OP_MOV, { Dst(REG_TEMP, Q, Y | Z), Src(REG_MISC, 0, Swz(0, 0, 1, 1)) });
	e.I(OP_MOV, { Dst(REG_TEMP, Q, W), Src(REG_CONST, K, Rep(0)) });
	e.I(OP_DP4, { Dst(REG_TEMP, G, X), Src(REG_TEMP, Q, kIdentity), Src(REG_CONST, GI_CONST + 0, kIdentity) });
	e.I(OP_DP4, { Dst(REG_TEMP, G, Y), Src(REG_TEMP, Q, kIdentity), Src(REG_CONST, GI_CONST + 1, kIdentity) });
	e.I(OP_DP4, { Dst(REG_TEMP, G, W), Src(REG_TEMP, Q, kIdentity), Src(REG_CONST, GI_CONST + 2, kIdentity) });
	e.I(OP_RCP, { Dst(REG_TEMP, G, W), Src(REG_TEMP, G, Rep(3)) });
	e.I(OP_MUL, { Dst(REG_TEMP, G, X | Y), Src(REG_TEMP, G, kIdentity), Src(REG_TEMP, G, Rep(3)) });
	e.I(OP_TEXLD, { Dst(REG_TEMP, G, 0xF), Src(REG_TEMP, G, kIdentity), Src(REG_SAMPLER, GI_SAMPLER, kIdentity) });

	const size_t combLen = 1 + C.len;
	if (f.form == GI_FORM_AMB_PRODUCT)
	{
		// lit = a * b + sun  ->  Q = a * b ; Q = albedo * G + Q ; lit = Q + sun
		e.I(OP_MUL, { Dst(REG_TEMP, Q, X | Y | Z), cp[1], cp[2] });
		e.I(OP_MAD, { Dst(REG_TEMP, Q, X | Y | Z), albedo, Src(REG_TEMP, G, kIdentity), Src(REG_TEMP, Q, kIdentity) });
		e.I(OP_ADD, { cp[0], Src(REG_TEMP, Q, kIdentity), cp[3] });
	}
	else
	{
		// indirect' = albedo * G + indirect, read by the combine in place of indirect
		e.I(OP_MAD, { Dst(REG_TEMP, G, X | Y | Z), albedo, Src(REG_TEMP, G, kIdentity), cp[f.addSrc] });
		std::vector<DWORD> inst(code + C.pos, code + C.pos + combLen);
		inst[1 + f.addSrc] = Src(REG_TEMP, G, kIdentity);
		out.insert(out.end(), inst.begin(), inst.end());
	}
	out.insert(out.end(), code + C.pos + combLen, code + endPos + 1);

	g_giLastLayout.form = f.form;
	g_giLastLayout.combineAt = C.pos;
	g_giLastLayout.albedo = f.albedo;
	g_giLastLayout.albedoAt = insts[f.albedoI].pos;
	g_giLastLayout.albedoCopied = f.copy;
	g_giLastLayout.depthExact = depthExact;
	g_giLastLayout.temp = G;
	g_giLastLayout.temp2 = Q;
	g_giLastLayout.constReg = K;
	result.swap(out);
	return true;
}

} // namespace ssgi
