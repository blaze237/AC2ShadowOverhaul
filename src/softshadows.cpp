// Soft sun shadows
//
// AC2's shadow receivers (ps_3_0) do a 4-tap manual bilinear PCF on the R32F shadow atlas (s15,
// point sampled): taps at uv, uv + (1/size, 0), (0, 1/size), (1/size, 1/size), each compared
// against the receiver depth (lit when stored >= depth), blended with weights frc(uv * size)
// where size = g_ShadowMapSize. The edge fades over exactly one texel, so stretched cascades look
// blocky. The 4 taps are compiled into a few different instruction orders, so the rewrite works
// from dataflow rather than fixed patterns:
//   - the filter's base uv is the operand of "mul X, uv, g_ShadowMapSize" that feeds the frc,
//   - the receiver depth is the negated operand of the compares that read the taps,
//   - the filter result is the lrp whose weight is the frc's second (v) component.
// We insert a wider filter in fresh temp registers just before the first tap and overwrite the
// game's result with it right after that lrp. The game's own taps are left in place (unused).
//
// The wider filter is a box of `width` texels centred where the game's 1-texel box is, evaluated
// exactly over the point-sampled map: (width+1)^2 taps, interior weight 1, edge rows/columns
// weighted by the fractional position. width = 1 reproduces the game's filter. Taps are clamped
// to the cascade's quarter of the atlas so they never read a neighbouring cascade.
//
// Contact hardening (PCSS, its own variant; the plain one above is unchanged by it). The box's
// width follows how far the shadow's caster is from the receiver, so a shadow is sharp where an
// object meets the ground and widens with distance from it, as a real sun shadow does:
//   1. the 5 x 5 texels around the sample (the footprint of the widest box, 4 texels) are read
//      once; texels closer to the light than the receiver are the blockers, and their mean depth
//      difference to the receiver, / |g_ShadowNearFar.x|, is the blocker distance in metres along
//      the light. Blockers outside that footprint can't change the filter result anyway, and with
//      none inside it the pixel is fully lit at any width (no branch needed).
//   2. penumbra (metres) = blocker distance * PenumbraScale. It becomes texels with the scale of
//      this pixel's cascade (the g_ShadowScalesX entry whose g_ShadowOffsetsX lies in the same
//      quarter of the atlas), so it is the same width in metres on either side of a cascade
//      boundary. It is then clamped to [MinWidth, MaxWidth] texels (at most 4, the grid).
//   3. the box of that (continuous) width, centred as above, over the same 25 texels: each
//      column's weight is how much of it the box covers, sat(hi - i) - sat(lo - i), likewise each
//      row, and the result is 1 - (blocked weight) / width^2. At a whole width N this is the plain
//      box of width N. Taps are clamped to the cascade's tile as in the plain filter.
// Its settings are compiled into the shader (constants defined in it, so the game's own constant
// uploads can never change them); changing one in the menu rebuilds these variants.

#include "softshadows.h"
#include "common.h"
#include "sm3.h"
#include <string.h>
#include <stdio.h>

SoftShadowsLayout g_softShadowsLastLayout;

// ---- Constant table ----------------------------------------------------------------------------

struct CtabInfo
{
	int shadowSampler = -1, shadowMapSize = -1;
	int scalesX = -1, offsetsX = -1, nearFar = -1; // for contact hardening
	bool usedFloat[256] = {};
};

static bool ReadCtab(const DWORD* code, CtabInfo& info)
{
	core::ConstantTable t;
	if (!core::ReadConstantTable(code, t))
		return false;
	info.shadowSampler = t.Register("g_ShadowSampler", core::RS_SAMPLER);
	info.shadowMapSize = t.Register("g_ShadowMapSize", core::RS_FLOAT4);
	info.scalesX = t.Register("g_ShadowScalesX", core::RS_FLOAT4);
	info.offsetsX = t.Register("g_ShadowOffsetsX", core::RS_FLOAT4);
	info.nearFar = t.Register("g_ShadowNearFar", core::RS_FLOAT4);
	for (const core::ShaderConstant& c : t.constants)
		if (c.set == core::RS_FLOAT4)
			for (int r = c.reg; r < c.reg + c.count && r < 256; r++)
				info.usedFloat[r] = true;
	return true;
}

// ---- Analysis ----------------------------------------------------------------------------------

// What both filters need to know about a shader: where the game's filter is and what it reads.
struct Analysis
{
	std::vector<Inst> insts;
	size_t endPos = 0;
	CtabInfo ctab;
	int S = 0, K = 0;              // shadow sampler, g_ShadowMapSize
	size_t first = 0, lrpI = 0;    // first shadow tap, the filter's final lrp (indices into insts)
	int uvReg = -1, uvComp[2] = {};
	int depthType = -1, depthReg = -1, depthComp = -1;
	int T = 0;                     // first free temp
	bool constUsed[256] = {};
};

static bool Analyse(const DWORD* code, Analysis& a, std::string& reason)
{
	std::vector<Inst>& insts = a.insts;
	size_t& endPos = a.endPos;
	if (!code || !Parse(code, insts, endPos))
	{
		reason = "not ps_3_0";
		return false;
	}
	CtabInfo& ctab = a.ctab;
	if (!ReadCtab(code, ctab) || ctab.shadowSampler < 0)
	{
		reason = "no shadow sampler";
		return false;
	}
	if (ctab.shadowMapSize < 0)
	{
		reason = "no g_ShadowMapSize (single-tap shader)";
		return false;
	}
	const int S = ctab.shadowSampler, K = ctab.shadowMapSize;

	// Shadow taps.
	std::vector<size_t> taps; // indices into insts
	for (size_t i = 0; i < insts.size(); i++)
	{
		const Inst& in = insts[i];
		if ((in.op == OP_TEXLD || in.op == OP_TEXLDL) && in.len >= 3)
		{
			DWORD s = code[in.pos + 3];
			if (RegType(s) == REG_SAMPLER && RegNum(s) == S)
				taps.push_back(i);
		}
	}
	if (taps.size() != 4)
	{
		reason = std::to_string(taps.size()) + " shadow taps (expected 4)";
		return false;
	}
	const size_t first = taps[0];

	// frc fed by "mul X, uv, g_ShadowMapSize".
	size_t frcI = 0, mulI = 0;
	int uvReg = -1, uvComp[2] = {}, wComp[2] = {}, wReg = -1;
	for (size_t i = first + 1; i < insts.size() && uvReg < 0; i++)
	{
		const Inst& f = insts[i];
		if (f.op != OP_FRC || f.len != 2)
			continue;
		DWORD fd = code[f.pos + 1], fs = code[f.pos + 2];
		int mask = DstMask(fd);
		if (RegType(fd) != REG_TEMP || RegType(fs) != REG_TEMP || SrcMod(fs) || Relative(fs))
			continue;
		int comps[4], n = 0;
		for (int c = 0; c < 4; c++)
			if (mask & (1 << c))
				comps[n++] = c;
		if (n != 2 || SwzComp(fs, comps[0]) != comps[0] || SwzComp(fs, comps[1]) != comps[1])
			continue;
		// Most recent write to the frc's source components.
		for (size_t j = i; j-- > 0;)
		{
			if (!WritesTemp(code, insts[j], RegNum(fs), mask))
				continue;
			const Inst& m = insts[j];
			if (m.op == OP_MUL && m.len == 3 && DstMask(code[m.pos + 1]) == mask)
			{
				DWORD a = code[m.pos + 2], b = code[m.pos + 3];
				if (RegType(a) == REG_CONST)
					std::swap(a, b);
				if (RegType(b) == REG_CONST && RegNum(b) == K && RegType(a) == REG_TEMP && !SrcMod(a) && !SrcMod(b) && !Relative(a) &&
					!Relative(b) && SwzComp(b, comps[0]) == 0 && SwzComp(b, comps[1]) == 1)
				{
					uvReg = RegNum(a);
					uvComp[0] = SwzComp(a, comps[0]);
					uvComp[1] = SwzComp(a, comps[1]);
					wReg = RegNum(fd);
					wComp[0] = comps[0];
					wComp[1] = comps[1];
					frcI = i;
					mulI = j;
				}
			}
			break;
		}
	}
	if (uvReg < 0)
	{
		reason = "filter weights (frc of uv * g_ShadowMapSize) not found";
		return false;
	}

	// The filter's final lrp: weight = the frc's v component.
	size_t lrpI = 0;
	for (size_t i = frcI + 1; i < insts.size(); i++)
	{
		const Inst& l = insts[i];
		if (l.op != OP_LRP || l.len != 4)
			continue;
		DWORD d = code[l.pos + 1], w = code[l.pos + 2];
		int mask = DstMask(d);
		if (RegType(w) == REG_TEMP && RegNum(w) == wReg && !SrcMod(w) && (mask == X || mask == Y || mask == Z || mask == W))
		{
			int c = mask == X ? 0 : mask == Y ? 1 : mask == Z ? 2 : 3;
			if (SwzComp(w, c) == wComp[1])
			{
				lrpI = i;
				break;
			}
		}
	}
	if (!lrpI)
	{
		reason = "filter result (lrp) not found";
		return false;
	}

	// Receiver depth: the negated operand of the compares that read the taps.
	int depthType = -1, depthReg = -1, depthComp = -1, compares = 0;
	size_t firstCompare = 0;
	for (size_t i = first + 1; i < lrpI; i++)
	{
		const Inst& a = insts[i];
		if (a.op != OP_ADD || a.len != 3)
			continue;
		DWORD s0 = code[a.pos + 2], s1 = code[a.pos + 3];
		if (SrcMod(s1) == 1)
			std::swap(s0, s1);
		if (SrcMod(s0) != 1 || SrcMod(s1) != 0 || RegType(s1) != REG_TEMP || Relative(s0) || Relative(s1))
			continue;
		bool readsTap = false; // s1 is the destination of a tap issued before this add
		for (size_t t : taps)
			if (t < i && RegNum(code[insts[t].pos + 1]) == RegNum(s1) && RegType(code[insts[t].pos + 1]) == REG_TEMP)
				readsTap = true;
		if (!readsTap)
			continue;
		int dc = 0;
		for (int c = 0; c < 4; c++)
			if (DstMask(code[a.pos + 1]) & (1 << c))
			{
				dc = c;
				break;
			}
		int type = RegType(s0), reg = RegNum(s0), comp = SwzComp(s0, dc);
		if (depthReg < 0)
		{
			depthType = type;
			depthReg = reg;
			depthComp = comp;
			firstCompare = i;
		}
		else if (type != depthType || reg != depthReg || comp != depthComp)
		{
			reason = "compares use different depth operands";
			return false;
		}
		compares++;
	}
	if (compares != 4 || (depthType != REG_TEMP && depthType != REG_INPUT))
	{
		reason = "depth compares not recognised (" + std::to_string(compares) + ")";
		return false;
	}

	// Inputs must already hold their values at the insertion point, and nothing between the
	// insertion point and the lrp may be flow control or predicated.
	for (size_t i = first; i <= lrpI; i++)
	{
		const Inst& in = insts[i];
		if (IsFlowControl(in.op) || in.predicated)
		{
			reason = "flow control inside the filter";
			return false;
		}
		if (i < mulI && WritesTemp(code, in, uvReg, (1 << uvComp[0]) | (1 << uvComp[1])))
		{
			reason = "uv overwritten before use";
			return false;
		}
		if (i < firstCompare && depthType == REG_TEMP && WritesTemp(code, in, depthReg, 1 << depthComp))
		{
			reason = "depth overwritten before use";
			return false;
		}
	}

	// Free registers: temps above the highest used, float constants not used or defined.
	int maxTemp = -1;
	bool* constUsed = a.constUsed;
	memcpy(constUsed, ctab.usedFloat, sizeof(a.constUsed));
	for (const Inst& in : insts)
	{
		if (in.op == OP_COMMENT)
			continue;
		for (size_t p = 1; p <= in.len; p++)
		{
			DWORD t = code[in.pos + p];
			if (!(t & 0x80000000u))
				continue; // usage token / literal
			if (Relative(t) && RegType(t) == REG_CONST)
			{
				reason = "relative constant addressing";
				return false;
			}
			if (RegType(t) == REG_TEMP)
				maxTemp = maxTemp > RegNum(t) ? maxTemp : RegNum(t);
			if (RegType(t) == REG_CONST && RegNum(t) < 256)
				constUsed[RegNum(t)] = true;
			if (in.op == OP_DEF || in.op == OP_DEFI || in.op == OP_DEFB || in.op == OP_DCL)
				break; // only the destination token is a register
		}
	}
	a.S = S;
	a.K = K;
	a.first = first;
	a.lrpI = lrpI;
	a.uvReg = uvReg;
	a.uvComp[0] = uvComp[0];
	a.uvComp[1] = uvComp[1];
	a.depthType = depthType;
	a.depthReg = depthReg;
	a.depthComp = depthComp;
	a.T = maxTemp + 1;
	return true;
}

// `count` free float constants, highest first (from c223 down).
static bool FreeConsts(const Analysis& a, int* cregs, int count)
{
	int nc = 0;
	for (int r = 223; r >= 0 && nc < count; r--)
		if (!a.constUsed[r])
			cregs[nc++] = r;
	return nc == count;
}

static void FillLayout(const DWORD* code, const Analysis& a, size_t insertAt, bool pcss)
{
	const Inst& lrp = a.insts[a.lrpI];
	DWORD lrpDst = code[lrp.pos + 1];
	SoftShadowsLayout& L = g_softShadowsLastLayout;
	L = SoftShadowsLayout();
	L.insertAt = insertAt;
	L.outputAt = lrp.pos;
	L.tempBase = a.T;
	L.outReg = RegNum(lrpDst);
	L.outComp = DstMask(lrpDst) == X ? 0 : DstMask(lrpDst) == Y ? 1 : DstMask(lrpDst) == Z ? 2 : 3;
	L.sampler = a.S;
	L.sizeConst = a.K;
	L.uvReg = a.uvReg;
	L.uvComp[0] = a.uvComp[0];
	L.uvComp[1] = a.uvComp[1];
	L.depthReg = a.depthReg;
	L.depthType = a.depthType;
	L.depthComp = a.depthComp;
	L.pcss = pcss;
	L.scalesX = a.ctab.scalesX;
	L.offsetsX = a.ctab.offsetsX;
	L.nearFar = a.ctab.nearFar;
}

// ---- Rewrite -----------------------------------------------------------------------------------

bool SoftShadows_Rewrite(const DWORD* code, int width, std::vector<DWORD>& result, std::string& reason)
{
	Analysis an;
	if (!Analyse(code, an, reason))
		return false;
	const std::vector<Inst>& insts = an.insts;
	const size_t endPos = an.endPos;
	const int S = an.S, K = an.K, uvReg = an.uvReg;
	const int* uvComp = an.uvComp;
	const int depthType = an.depthType, depthReg = an.depthReg, depthComp = an.depthComp;
	const size_t first = an.first, lrpI = an.lrpI;
	const int T = an.T;
	if (T + 10 > 32)
	{
		reason = "not enough free temp registers";
		return false;
	}
	int cregs[3];
	if (!FreeConsts(an, cregs, 3))
	{
		reason = "no free constant registers";
		return false;
	}
	if (width < 1)
		width = 1;
	if (width > 4)
		width = 4;
	const int N = width;
	const int CA = cregs[0], CB = cregs[1], CC = cregs[2];
	const float off = 0.5f - N * 0.5f, invN2 = 1.0f / (float)(N * N);
	auto fbits = [](float f) {
		DWORD d;
		memcpy(&d, &f, 4);
		return d;
	};

	// ---- Emit ----
	std::vector<DWORD> out;
	out.reserve(endPos + 600);
	Emitter e{ out };
	size_t firstReal = endPos; // first non-comment instruction: the defs go before it
	for (const Inst& in : insts)
		if (in.op != OP_COMMENT)
		{
			firstReal = in.pos;
			break;
		}
	out.insert(out.end(), code, code + firstReal);
	// CA = (off, off, 0.5, 4), CB = (0.25, 1, 0, 1/N^2), CC = (0, 1, 2, 3)
	e.I(OP_DEF, { Dst(REG_CONST, CA, 0xF), fbits(off), fbits(off), fbits(0.5f), fbits(4.0f) });
	e.I(OP_DEF, { Dst(REG_CONST, CB, 0xF), fbits(0.25f), fbits(1.0f), fbits(0.0f), fbits(invN2) });
	e.I(OP_DEF, { Dst(REG_CONST, CC, 0xF), fbits(0.0f), fbits(1.0f), fbits(2.0f), fbits(3.0f) });
	const size_t insertAt = insts[first].pos;
	out.insert(out.end(), code + firstReal, code + insertAt);

	const int T0 = T, T1 = T + 1, T2 = T + 2, T3 = T + 3, T4 = T + 4, T5 = T + 5, T6 = T + 6, T7 = T + 7, T8 = T + 8, T9 = T + 9;
	auto t = [](int r, int swz = kIdentity, bool neg = false) { return Src(REG_TEMP, r, swz, neg); };
	auto c = [](int r, int swz) { return Src(REG_CONST, r, swz); };
	auto idx = [&](int i) { return i < 4 ? c(CC, Rep(i)) : c(CA, Rep(3)); }; // the constant i (0..4)
	auto col = [&](int i, int& reg, int& comp) {
		reg = i < 4 ? T4 : T5;
		comp = i < 4 ? i : 0;
	};

	// T0.xy = x = uv * size; a = x + off; T1.xy = f = frc(a); T0.xy = (a - f + 0.5) / size
	e.I(OP_MUL, { Dst(REG_TEMP, T0, X | Y), t(uvReg, Swz(uvComp[0], uvComp[1], uvComp[1], uvComp[1])), c(K, kIdentity) });
	e.I(OP_ADD, { Dst(REG_TEMP, T0, X | Y), t(T0), c(CA, Rep(0)) });
	e.I(OP_FRC, { Dst(REG_TEMP, T1, X | Y), t(T0) });
	e.I(OP_ADD, { Dst(REG_TEMP, T0, X | Y), t(T0), t(T1, kIdentity, true) });
	e.I(OP_ADD, { Dst(REG_TEMP, T0, X | Y), t(T0), c(CA, Rep(2)) });
	e.I(OP_RCP, { Dst(REG_TEMP, T2, X), c(K, Rep(0)) });
	e.I(OP_RCP, { Dst(REG_TEMP, T2, Y), c(K, Rep(1)) });
	e.I(OP_MUL, { Dst(REG_TEMP, T0, X | Y), t(T0), t(T2) });
	// Cascade tile in u: [floor(u*4)/4 + 0.5/w, floor(u*4)/4 + 0.25 - 0.5/w] -> T3.y, T3.z
	e.I(OP_MUL, { Dst(REG_TEMP, T3, X), t(uvReg, Rep(uvComp[0])), c(CA, Rep(3)) });
	e.I(OP_FRC, { Dst(REG_TEMP, T3, Y), t(T3, Rep(0)) });
	e.I(OP_ADD, { Dst(REG_TEMP, T3, X), t(T3, Rep(0)), t(T3, Rep(1), true) });
	e.I(OP_MUL, { Dst(REG_TEMP, T3, X), t(T3, Rep(0)), c(CB, Rep(0)) });
	e.I(OP_MAD, { Dst(REG_TEMP, T3, Y), t(T2, Rep(0)), c(CA, Rep(2)), t(T3, Rep(0)) });
	e.I(OP_ADD, { Dst(REG_TEMP, T3, Z), t(T3, Rep(0)), c(CB, Rep(0)) });
	e.I(OP_MAD, { Dst(REG_TEMP, T3, Z), t(T2, Rep(0)), Src(REG_CONST, CA, Rep(2), true), t(T3, Rep(2)) });
	// Column u coordinates, clamped to the tile.
	for (int i = 0; i <= N; i++)
	{
		int r, k;
		col(i, r, k);
		e.I(OP_MAD, { Dst(REG_TEMP, r, 1 << k), t(T2, Rep(0)), idx(i), t(T0, Rep(0)) });
		e.I(OP_MAX, { Dst(REG_TEMP, r, 1 << k), t(r, Rep(k)), t(T3, Rep(1)) });
		e.I(OP_MIN, { Dst(REG_TEMP, r, 1 << k), t(r, Rep(k)), t(T3, Rep(2)) });
	}
	e.I(OP_MOV, { Dst(REG_TEMP, T6, Z | W), c(CB, Rep(2)) });
	const DWORD depthSrc = Src(depthType, depthReg, Rep(depthComp), true);
	for (int j = 0; j <= N; j++)
	{
		e.I(OP_MAD, { Dst(REG_TEMP, T6, Y), t(T2, Rep(1)), idx(j), t(T0, Rep(1)) });
		for (int i = 0; i <= N; i++)
		{
			int r, k;
			col(i, r, k);
			e.I(OP_MOV, { Dst(REG_TEMP, T6, X), t(r, Rep(k)) });
			e.I(OP_TEXLDL, { Dst(REG_TEMP, T8, 0xF), t(T6), Src(REG_SAMPLER, S, kIdentity) });
			e.I(OP_ADD, { Dst(REG_TEMP, T8, X), t(T8, Rep(0)), depthSrc });
			e.I(OP_CMP, { Dst(REG_TEMP, T8, X), t(T8, Rep(0)), c(CB, Rep(1)), c(CB, Rep(2)) });
			if (i == 0)
				e.I(OP_MOV, { Dst(REG_TEMP, T7, Z), t(T8, Rep(0)) });
			else if (i < N)
			{
				if (i == 1)
					e.I(OP_MOV, { Dst(REG_TEMP, T7, Y), t(T8, Rep(0)) });
				else
					e.I(OP_ADD, { Dst(REG_TEMP, T7, Y), t(T7, Rep(1)), t(T8, Rep(0)) });
			}
			else
			{
				// row = interior + first + fx * (last - first)
				e.I(OP_ADD, { Dst(REG_TEMP, T8, X), t(T8, Rep(0)), t(T7, Rep(2), true) });
				e.I(OP_MAD, { Dst(REG_TEMP, T8, X), t(T8, Rep(0)), t(T1, Rep(0)), t(T7, Rep(2)) });
				if (N > 1)
					e.I(OP_ADD, { Dst(REG_TEMP, T8, X), t(T8, Rep(0)), t(T7, Rep(1)) });
			}
		}
		if (j == 0)
			e.I(OP_MOV, { Dst(REG_TEMP, T9, X), t(T8, Rep(0)) });
		else if (j < N)
		{
			if (j == 1)
				e.I(OP_MOV, { Dst(REG_TEMP, T9, Y), t(T8, Rep(0)) });
			else
				e.I(OP_ADD, { Dst(REG_TEMP, T9, Y), t(T9, Rep(1)), t(T8, Rep(0)) });
		}
		else
		{
			e.I(OP_ADD, { Dst(REG_TEMP, T8, X), t(T8, Rep(0)), t(T9, Rep(0), true) });
			e.I(OP_MAD, { Dst(REG_TEMP, T8, X), t(T8, Rep(0)), t(T1, Rep(1)), t(T9, Rep(0)) });
			if (N > 1)
				e.I(OP_ADD, { Dst(REG_TEMP, T8, X), t(T8, Rep(0)), t(T9, Rep(1)) });
		}
	}
	e.I(OP_MUL, { Dst(REG_TEMP, T9, X), t(T8, Rep(0)), c(CB, Rep(3)) });

	// Original code up to and including the game's lrp, then overwrite its result.
	const Inst& lrp = insts[lrpI];
	size_t afterLrp = lrp.pos + 1 + lrp.len;
	out.insert(out.end(), code + insertAt, code + afterLrp);
	DWORD lrpDst = code[lrp.pos + 1];
	e.I(OP_MOV, { lrpDst, t(T9, Rep(0)) });
	out.insert(out.end(), code + afterLrp, code + endPos + 1); // includes the end token

	FillLayout(code, an, insertAt, false);
	result.swap(out);
	reason.clear();
	return true;
}

// Instruction slots a ps_3_0 program uses, roughly as the compiler counts them (the macros that
// expand to several instructions counted as such), for keeping the variants within 512.
int SoftShadows_InstructionSlots(const std::vector<DWORD>& code)
{
	std::vector<Inst> insts;
	size_t endPos = 0;
	if (code.empty() || !Parse(code.data(), insts, endPos))
		return 0;
	int n = 0;
	for (const Inst& in : insts)
	{
		switch (in.op)
		{
		case OP_COMMENT:
		case OP_DCL:
		case OP_DEF:
		case OP_DEFI:
		case OP_DEFB:
			break;
		case OP_LRP:
		case OP_CRS:
		case OP_TEXLDL:
			n += 2;
			break;
		case OP_TEXLDD:
			n += 3;
			break;
		case OP_NRM:
		case OP_POW:
			n += 3;
			break;
		case OP_SINCOS:
			n += 8;
			break;
		default:
			n++;
			break;
		}
	}
	return n;
}

// Contact hardening temps: 10, like the plain filter, and the 3 the lighting rewrite (AO, SSGI,
// litshader.cpp) adds on top of it are left free, so those effects never lose a shader to it.
static const int kPcssTemps = 10, kLitTemps = 3;
// The lighting rewrite adds a few dozen instructions on top; keep room for them.
static const int kPcssMaxSlots = 512 - 48;

bool SoftShadows_RewritePcss(const DWORD* code, const SoftShadowsPcss& params, std::vector<DWORD>& result, std::string& reason)
{
	Analysis an;
	if (!Analyse(code, an, reason))
		return false;
	if (an.ctab.scalesX < 0 || an.ctab.offsetsX < 0 || an.ctab.nearFar < 0)
	{
		reason = "no g_ShadowScalesX / g_ShadowOffsetsX / g_ShadowNearFar";
		return false;
	}
	const int T = an.T;
	if (T + kPcssTemps + kLitTemps > 32)
	{
		reason = "not enough free temp registers";
		return false;
	}
	int cregs[4];
	if (!FreeConsts(an, cregs, 4))
	{
		reason = "no free constant registers";
		return false;
	}
	SoftShadowsPcss p = SoftShadows_ClampPcss(params);
	const float minW = p.minWidth < p.maxWidth ? p.minWidth : p.maxWidth;

	const std::vector<Inst>& insts = an.insts;
	const int S = an.S, K = an.K, uvReg = an.uvReg;
	const int CA = cregs[0], CB = cregs[1], CC = cregs[2], CD = cregs[3];
	const int SX = an.ctab.scalesX, OX = an.ctab.offsetsX, NF = an.ctab.nearFar;

	std::vector<DWORD> out;
	out.reserve(an.endPos + 900);
	Emitter e{ out };
	const size_t firstReal = FirstRealInstruction(insts, an.endPos);
	out.insert(out.end(), code, code + firstReal);
	// CA = (-1.5, 1/8, 0.5, 4), CB = (0.25, 1, 0, -), CC = (0, 1, 2, 3), CD = (scale, min, max, -).
	// (One constant register per instruction at most, as the shader models allow.)
	e.I(OP_DEF, { Dst(REG_CONST, CA, 0xF), FloatBits(-1.5f), FloatBits(0.125f), FloatBits(0.5f), FloatBits(4.0f) });
	e.I(OP_DEF, { Dst(REG_CONST, CB, 0xF), FloatBits(0.25f), FloatBits(1.0f), FloatBits(0.0f), FloatBits(0.0f) });
	e.I(OP_DEF, { Dst(REG_CONST, CC, 0xF), FloatBits(0.0f), FloatBits(1.0f), FloatBits(2.0f), FloatBits(3.0f) });
	e.I(OP_DEF, { Dst(REG_CONST, CD, 0xF), FloatBits(p.penumbraScale), FloatBits(minW), FloatBits(p.maxWidth), FloatBits(0.0f) });
	const size_t insertAt = insts[an.first].pos;
	out.insert(out.end(), code + firstReal, code + insertAt);

	// A, U, V, F scratch; P[j] = row j of the grid, columns 0-3; Q = column 4, rows 0-3; F.x the
	// last texel (4, 4). The result ends in Q.x (= tempBase + 9, as in the plain filter).
	const int A = T, U = T + 1, V = T + 2, F = T + 3, P0 = T + 4, Q = T + 9;
	auto t = [](int r, int swz = kIdentity, bool neg = false) { return Src(REG_TEMP, r, swz, neg); };
	auto c = [](int r, int swz = kIdentity, bool neg = false) { return Src(REG_CONST, r, swz, neg); };
	auto d = [](int r, int mask, bool sat = false) { return Dst(REG_TEMP, r, mask) | (sat ? (1u << 20) : 0u); };
	const DWORD uv = t(uvReg, Swz(an.uvComp[0], an.uvComp[1], an.uvComp[1], an.uvComp[1]));
	const DWORD uvX = t(uvReg, Rep(an.uvComp[0]));
	const DWORD depthSrc = Src(an.depthType, an.depthReg, Rep(an.depthComp), true);
	const DWORD zero = c(CB, Rep(2)), one = c(CB, Rep(1));

	// Grid origin: V.xy = (floor(uv * size - 1.5) + 0.5) / size; F.zw = 1 / size.
	e.I(OP_MUL, { d(V, X | Y), uv, c(K) });
	e.I(OP_ADD, { d(V, X | Y), t(V), c(CA, Rep(0)) });
	e.I(OP_FRC, { d(F, X | Y), t(V) });
	e.I(OP_ADD, { d(V, X | Y), t(V), t(F, kIdentity, true) });
	e.I(OP_ADD, { d(V, X | Y), t(V), c(CA, Rep(2)) });
	e.I(OP_RCP, { d(F, Z), c(K, Rep(0)) });
	e.I(OP_RCP, { d(F, W), c(K, Rep(1)) });
	e.I(OP_MUL, { d(V, X | Y), t(V), t(F, Swz(2, 3, 2, 3)) });
	// The cascade's tile in u, as in the plain filter: A.z = lo, A.w = hi.
	e.I(OP_MUL, { d(A, X), uvX, c(CA, Rep(3)) });
	e.I(OP_FRC, { d(A, Y), t(A, Rep(0)) });
	e.I(OP_ADD, { d(A, X), t(A, Rep(0)), t(A, Rep(1), true) });
	e.I(OP_MUL, { d(A, X), t(A, Rep(0)), c(CB, Rep(0)) });
	e.I(OP_MAD, { d(A, Z), t(F, Rep(2)), c(CA, Rep(2)), t(A, Rep(0)) });
	e.I(OP_ADD, { d(A, W), t(A, Rep(0)), c(CB, Rep(0)) });
	e.I(OP_MAD, { d(A, W), t(F, Rep(2)), c(CA, Rep(2), true), t(A, Rep(3)) });
	// Column u coordinates, clamped to the tile: U = columns 0-3, A.x = column 4. Then A.y = row 0's
	// v, A.z = 1 / height.
	e.I(OP_MAD, { d(U, 0xF), t(F, Rep(2)), c(CC), t(V, Rep(0)) });
	e.I(OP_MAX, { d(U, 0xF), t(U), t(A, Rep(2)) });
	e.I(OP_MIN, { d(U, 0xF), t(U), t(A, Rep(3)) });
	e.I(OP_MAD, { d(A, X), t(F, Rep(2)), c(CA, Rep(3)), t(V, Rep(0)) });
	e.I(OP_MAX, { d(A, X), t(A, Rep(0)), t(A, Rep(2)) });
	e.I(OP_MIN, { d(A, X), t(A, Rep(0)), t(A, Rep(3)) });
	e.I(OP_MOV, { d(A, Y), t(V, Rep(1)) });
	e.I(OP_MOV, { d(A, Z), t(F, Rep(3)) });
	e.I(OP_MOV, { d(V, Z | W), zero });
	// The 25 texels: stored depth - receiver depth (>= 0 lit, < 0 a blocker). Plain texld, as the
	// game's own taps (the atlas has one mip level and is point sampled; texldl takes 2 slots).
	for (int j = 0; j <= 4; j++)
	{
		e.I(OP_MAD, { d(V, Y), t(A, Rep(2)), j < 4 ? c(CC, Rep(j)) : c(CA, Rep(3)), t(A, Rep(1)) });
		for (int i = 0; i <= 4; i++)
		{
			e.I(OP_MOV, { d(V, X), i < 4 ? t(U, Rep(i)) : t(A, Rep(0)) });
			e.I(OP_TEXLD, { d(F, 0xF), t(V), Src(REG_SAMPLER, S, kIdentity) });
			DWORD dst = i < 4 ? d(P0 + j, 1 << i) : j < 4 ? d(Q, 1 << j) : d(F, X);
			e.I(OP_ADD, { dst, t(F, Rep(0)), depthSrc });
		}
	}
	// Blocker search: A = sum of the (negative) differences of the blockers, V = their count; each
	// texel's difference becomes 1 if it is a blocker, else 0.
	for (int k = 0; k < 6; k++)
	{
		const int Xr = k < 5 ? P0 + k : Q;
		if (k == 0)
			e.I(OP_MIN, { d(A, 0xF), t(Xr), zero });
		else
		{
			e.I(OP_MIN, { d(U, 0xF), t(Xr), zero });
			e.I(OP_ADD, { d(A, 0xF), t(A), t(U) });
		}
		e.I(OP_CMP, { d(Xr, 0xF), t(Xr), zero, one });
		if (k == 0)
			e.I(OP_MOV, { d(V, 0xF), t(Xr) });
		else
			e.I(OP_ADD, { d(V, 0xF), t(V), t(Xr) });
	}
	e.I(OP_MIN, { d(U, X), t(F, Rep(0)), zero });
	e.I(OP_ADD, { d(A, X), t(A, Rep(0)), t(U, Rep(0)) });
	e.I(OP_CMP, { d(F, X), t(F, Rep(0)), zero, one });
	e.I(OP_ADD, { d(V, X), t(V, Rep(0)), t(F, Rep(0)) });
	e.I(OP_DP4, { d(A, X), t(A), one });
	e.I(OP_DP4, { d(A, Y), t(V), one });
	// Blocker distance in metres along the light: mean difference / |g_ShadowNearFar.x| -> A.x.
	// (No blocker: 0, and then every texel is lit, whatever the width.)
	e.I(OP_MAX, { d(A, Y), t(A, Rep(1)), one });
	e.I(OP_RCP, { d(A, Y), t(A, Rep(1)) });
	e.I(OP_MUL, { d(A, X), t(A, Rep(0), true), t(A, Rep(1)) });
	e.I(OP_MUL, { d(A, Z), c(NF, Rep(0)), c(NF, Rep(0)) });
	e.I(OP_RSQ, { d(A, Z), t(A, Rep(2)) });
	e.I(OP_MUL, { d(A, X), t(A, Rep(0)), t(A, Rep(2)) });
	// Texels per metre in this pixel's cascade: |g_ShadowScalesX[k]| * width for the cascade k whose
	// g_ShadowOffsetsX lies in the same quarter of the atlas as the sample (floor(offset * 4 + 1/8)
	// is the tile index whether the offset is the tile's centre or its start) -> A.y.
	e.I(OP_MUL, { d(A, Y), uvX, c(CA, Rep(3)) });
	e.I(OP_FRC, { d(A, Z), t(A, Rep(1)) });
	e.I(OP_ADD, { d(A, Y), t(A, Rep(1)), t(A, Rep(2), true) });
	e.I(OP_MOV, { d(V, 0xF), c(OX) });
	e.I(OP_MAD, { d(V, 0xF), t(V), c(CA, Rep(3)), c(CA, Rep(1)) });
	e.I(OP_FRC, { d(U, 0xF), t(V) });
	e.I(OP_ADD, { d(V, 0xF), t(V), t(U, kIdentity, true) });
	e.I(OP_ADD, { d(V, 0xF), t(V), t(A, Rep(1), true) });
	e.I(OP_MUL, { d(V, 0xF), t(V), t(V) });
	e.I(OP_CMP, { d(V, 0xF), t(V, kIdentity, true), one, zero });
	e.I(OP_DP4, { d(A, Y), t(V), c(SX) });
	e.I(OP_MAX, { d(A, Y), t(A, Rep(1)), t(A, Rep(1), true) });
	e.I(OP_MUL, { d(A, Y), t(A, Rep(1)), c(K, Rep(0)) });
	// Width in texels: metres * scale * texels per metre, clamped -> A.x; F.y = width^2.
	e.I(OP_MUL, { d(A, X), t(A, Rep(0)), t(A, Rep(1)) });
	e.I(OP_MUL, { d(A, X), t(A, Rep(0)), c(CD, Rep(0)) });
	e.I(OP_MAX, { d(A, X), t(A, Rep(0)), c(CD, Rep(1)) });
	e.I(OP_MIN, { d(A, X), t(A, Rep(0)), c(CD, Rep(2)) });
	e.I(OP_MUL, { d(F, Y), t(A, Rep(0)), t(A, Rep(0)) });
	// The box's edges in grid cells: lo = frc(uv * size - 1.5) + 2 - width / 2, hi = lo + width.
	// U = (lo.x, lo.y, hi.x, hi.y).
	e.I(OP_MUL, { d(U, X | Y), uv, c(K) });
	e.I(OP_ADD, { d(U, X | Y), t(U), c(CA, Rep(0)) });
	e.I(OP_FRC, { d(U, X | Y), t(U) });
	e.I(OP_MAD, { d(U, X | Y), t(A, Rep(0)), c(CA, Rep(2), true), t(U) });
	e.I(OP_ADD, { d(U, X | Y), t(U), c(CC, Rep(2)) });
	e.I(OP_ADD, { d(U, Z | W), t(U, Swz(0, 1, 0, 1)), t(A, Rep(0)) });
	// Cell weights, sat(hi - i) - sat(lo - i): F.z, F.w = column 4 and row 4, V = columns 0-3,
	// A = rows 0-3.
	e.I(OP_ADD, { d(F, Z | W, true), t(U), c(CA, Rep(3), true) });
	e.I(OP_ADD, { d(A, X | Y, true), t(U), c(CA, Rep(3), true) });
	e.I(OP_ADD, { d(F, Z | W), t(F), t(A, Swz(0, 1, 0, 1), true) });
	e.I(OP_ADD, { d(V, 0xF, true), t(U, Rep(2)), c(CC, kIdentity, true) });
	e.I(OP_ADD, { d(A, 0xF, true), t(U, Rep(0)), c(CC, kIdentity, true) });
	e.I(OP_ADD, { d(V, 0xF), t(V), t(A, kIdentity, true) });
	e.I(OP_ADD, { d(A, 0xF, true), t(U, Rep(3)), c(CC, kIdentity, true) });
	e.I(OP_ADD, { d(U, 0xF, true), t(U, Rep(1)), c(CC, kIdentity, true) });
	e.I(OP_ADD, { d(A, 0xF), t(A), t(U, kIdentity, true) });
	// Blocked weight -> U.x; lit = 1 - blocked / width^2 -> Q.x.
	for (int j = 0; j < 4; j++)
		e.I(OP_DP4, { d(U, 1 << j), t(P0 + j), t(V) });
	e.I(OP_MAD, { d(U, 0xF), t(Q), t(F, Rep(2)), t(U) });
	e.I(OP_DP4, { d(U, X), t(U), t(A) });
	e.I(OP_DP4, { d(U, Y), t(P0 + 4), t(V) });
	e.I(OP_MAD, { d(U, Y), t(F, Rep(0)), t(F, Rep(2)), t(U, Rep(1)) });
	e.I(OP_MAD, { d(U, X), t(U, Rep(1)), t(F, Rep(3)), t(U, Rep(0)) });
	e.I(OP_RCP, { d(U, Y), t(F, Rep(1)) });
	e.I(OP_MAD, { d(Q, X, true), t(U, Rep(0), true), t(U, Rep(1)), one });

	// Original code up to and including the game's lrp, then overwrite its result.
	const Inst& lrp = insts[an.lrpI];
	size_t afterLrp = lrp.pos + 1 + lrp.len;
	out.insert(out.end(), code + insertAt, code + afterLrp);
	e.I(OP_MOV, { code[lrp.pos + 1], t(Q, Rep(0)) });
	out.insert(out.end(), code + afterLrp, code + an.endPos + 1); // includes the end token

	int slots = SoftShadows_InstructionSlots(out);
	if (slots > kPcssMaxSlots)
	{
		reason = "too long with contact hardening (" + std::to_string(slots) + " instruction slots)";
		return false;
	}
	FillLayout(code, an, insertAt, true);
	result.swap(out);
	reason.clear();
	return true;
}

// ---- Settings ----------------------------------------------------------------------------------

static bool g_allWidths = false;
static volatile bool g_active = true;
static volatile int g_liveWidth = 3;
static int g_width = 3;

bool SoftShadows_Active() { return g_active; }
void SoftShadows_SetActive(bool on) { g_active = on; }
int SoftShadows_Width() { return g_width; }
int SoftShadows_LiveWidth() { return g_liveWidth; }
void SoftShadows_SetLiveWidth(int width) { g_liveWidth = width < 1 ? 1 : width > 4 ? 4 : width; }
void SoftShadows_SetBuildAllWidths(bool all) { g_allWidths = all; }
bool SoftShadows_BuildAllWidths() { return g_allWidths; }

// Contact hardening: on at start / now; the settings as the menu shows them, and as the variants
// were last built with (the menu's changes are taken once they have settled, so holding a key
// doesn't rebuild the shaders on every repeat).
static bool g_chStart = false;
static volatile bool g_ch = false;
static SoftShadowsPcss g_pcss, g_pcssBuilt;
static int g_pcssVersion = 0;
static bool g_pcssPending = false;
static DWORD g_pcssChangedAt = 0;
static const DWORD kPcssSettleMs = 300;

SoftShadowsPcss SoftShadows_ClampPcss(const SoftShadowsPcss& in)
{
	auto clamp = [](float v, float lo, float hi) { return v != v ? lo : v < lo ? lo : v > hi ? hi : v; }; // NaN -> lo
	SoftShadowsPcss p;
	p.penumbraScale = clamp(in.penumbraScale, 0.005f, 0.5f);
	p.minWidth = clamp(in.minWidth, 1.0f, 4.0f);
	p.maxWidth = clamp(in.maxWidth, 1.0f, 4.0f);
	return p;
}

bool SoftShadows_ContactHardeningAtStart() { return g_chStart; }
bool SoftShadows_ContactHardening() { return g_ch; }
void SoftShadows_SetContactHardening(bool on) { g_ch = on; }
SoftShadowsPcss SoftShadows_Pcss() { return g_pcss; }

void SoftShadows_SetPcss(const SoftShadowsPcss& p)
{
	g_pcss = SoftShadows_ClampPcss(p);
	g_pcssPending = true;
	g_pcssChangedAt = GetTickCount();
}

int SoftShadows_PcssBuilt(SoftShadowsPcss* p)
{
	if (g_pcssPending && GetTickCount() - g_pcssChangedAt >= kPcssSettleMs)
	{
		g_pcssPending = false;
		if (memcmp(&g_pcss, &g_pcssBuilt, sizeof(g_pcss)) != 0)
		{
			g_pcssBuilt = g_pcss;
			g_pcssVersion++;
			LogInfo("Contact hardening: shaders rebuilt for penumbra %.3f m per m, width %.2f to %.2f texels.", g_pcssBuilt.penumbraScale,
				g_pcssBuilt.minWidth, g_pcssBuilt.maxWidth);
		}
	}
	if (p)
		*p = g_pcssBuilt;
	return g_pcssVersion;
}

bool SoftShadows_Init()
{
	const char* S = "SoftShadows";
	g_width = IniInt(S, "FilterWidth", 3);
	// 0 = off: the game keeps its own filter (the shaders are still rewritten, at width 1, so the
	// menu can switch soft shadows on without a restart).
	g_active = g_width != 0;
	if (g_width < 1)
		g_width = 1;
	if (g_width > 4)
		g_width = 4;
	g_liveWidth = g_width;
	if (g_active)
		LogInfo("Soft shadows: filter width %d texels (%d taps).", g_width, (g_width + 1) * (g_width + 1));
	else
		LogInfo("Soft shadows: off (FilterWidth=0); the game's own filter is used.");

	g_chStart = g_ch = IniInt(S, "ContactHardening", 0) != 0;
	SoftShadowsPcss p;
	p.penumbraScale = IniFloat(S, "PenumbraScale", p.penumbraScale);
	p.minWidth = IniFloat(S, "MinWidth", p.minWidth);
	p.maxWidth = IniFloat(S, "MaxWidth", p.maxWidth);
	g_pcss = g_pcssBuilt = SoftShadows_ClampPcss(p);
	if (g_chStart)
		LogInfo("Contact hardening: on (penumbra %.3f m per metre from the caster, %.2f to %.2f texels wide).", g_pcss.penumbraScale,
			g_pcss.minWidth, g_pcss.maxWidth);
	return true;
}
