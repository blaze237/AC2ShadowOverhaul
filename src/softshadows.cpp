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
	bool usedFloat[256] = {};
};

static bool ReadCtab(const DWORD* code, const std::vector<Inst>& insts, CtabInfo& info)
{
	for (const Inst& in : insts)
	{
		if (in.op != OP_COMMENT || in.len < 8 || code[in.pos + 1] != 0x42415443) // 'CTAB'
			continue;
		const uint8_t* base = (const uint8_t*)&code[in.pos + 2];
		size_t size = (in.len - 1) * 4;
		auto rd32 = [&](size_t o) -> DWORD { return o + 4 <= size ? *(const DWORD*)(base + o) : 0; };
		auto rd16 = [&](size_t o) -> WORD { return o + 2 <= size ? *(const WORD*)(base + o) : 0; };
		DWORD count = rd32(12), infoOff = rd32(16);
		for (DWORD k = 0; k < count && infoOff + 20 * (k + 1) <= size; k++)
		{
			size_t e = infoOff + 20 * k;
			DWORD nameOff = rd32(e);
			WORD set = rd16(e + 4), index = rd16(e + 6), regCount = rd16(e + 8);
			char name[64] = {};
			for (size_t c = 0; c < sizeof(name) - 1 && nameOff + c < size && base[nameOff + c]; c++)
				name[c] = (char)base[nameOff + c];
			if (set == 3 && strcmp(name, "g_ShadowSampler") == 0)
				info.shadowSampler = index;
			if (set == 2)
			{
				if (strcmp(name, "g_ShadowMapSize") == 0)
					info.shadowMapSize = index;
				for (int r = index; r < index + regCount && r < 256; r++)
					info.usedFloat[r] = true;
			}
		}
		return true;
	}
	return false;
}

// ---- Rewrite -----------------------------------------------------------------------------------

bool SoftShadows_Rewrite(const DWORD* code, int width, std::vector<DWORD>& result, std::string& reason)
{
	std::vector<Inst> insts;
	size_t endPos = 0;
	if (!code || !Parse(code, insts, endPos))
	{
		reason = "not ps_3_0";
		return false;
	}
	CtabInfo ctab;
	if (!ReadCtab(code, insts, ctab) || ctab.shadowSampler < 0)
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
	bool constUsed[256] = {};
	memcpy(constUsed, ctab.usedFloat, sizeof(constUsed));
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
	const int T = maxTemp + 1;
	if (T + 10 > 32)
	{
		reason = "not enough free temp registers";
		return false;
	}
	int cregs[3], nc = 0;
	for (int r = 223; r >= 0 && nc < 3; r--)
		if (!constUsed[r])
			cregs[nc++] = r;
	if (nc < 3)
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

	SoftShadowsLayout& L = g_softShadowsLastLayout;
	L = SoftShadowsLayout();
	L.insertAt = insertAt;
	L.outputAt = lrp.pos;
	L.tempBase = T;
	L.outReg = RegNum(lrpDst);
	L.outComp = DstMask(lrpDst) == X ? 0 : DstMask(lrpDst) == Y ? 1 : DstMask(lrpDst) == Z ? 2 : 3;
	L.sampler = S;
	L.sizeConst = K;
	L.uvReg = uvReg;
	L.uvComp[0] = uvComp[0];
	L.uvComp[1] = uvComp[1];
	L.depthReg = depthReg;
	L.depthType = depthType;
	L.depthComp = depthComp;

	result.swap(out);
	reason.clear();
	return true;
}

// ---- Settings and toggle -----------------------------------------------------------------------

static bool g_allWidths = false;
static volatile bool g_active = true;
static volatile int g_liveWidth = 3;
static int g_width = 3, g_toggleKey = 0;

bool SoftShadows_Active() { return g_active; }
int SoftShadows_Width() { return g_width; }
int SoftShadows_LiveWidth() { return g_liveWidth; }
void SoftShadows_SetLiveWidth(int width) { g_liveWidth = width < 1 ? 1 : width > 4 ? 4 : width; }
void SoftShadows_SetBuildAllWidths(bool all) { g_allWidths = all; }
bool SoftShadows_BuildAllWidths() { return g_allWidths; }

static DWORD WINAPI SoftShadowsToggleThread(void*)
{
	bool wasDown = false;
	for (;;)
	{
		Sleep(30);
		bool down = (GetAsyncKeyState(g_toggleKey) & 0x8000) != 0;
		if (down && !wasDown && GameHasFocus())
		{
			g_active = !g_active;
			LogInfo("Soft shadows toggled %s.", g_active ? "ON" : "OFF (game filter)");
			Beep(g_active ? 988 : 494, 120);
		}
		wasDown = down;
	}
	return 0;
}

bool SoftShadows_Init()
{
	const char* S = "SoftShadows";
	g_width = IniInt(S, "FilterWidth", 3);
	if (g_width < 1)
		g_width = 1;
	if (g_width > 4)
		g_width = 4;
	g_liveWidth = g_width;
	g_active = true; // always on; the toggle key switches to the game's filter for comparisons
	g_toggleKey = IniInt(S, "ToggleKey", VK_F6);
	if (g_toggleKey > 0 && g_toggleKey < 256)
	{
		HANDLE h = CreateThread(NULL, 0, SoftShadowsToggleThread, NULL, 0, NULL);
		if (h)
			CloseHandle(h);
	}
	LogInfo("Soft shadows: filter width %d texels (%d taps), toggle key 0x%02X.", g_width, (g_width + 1) * (g_width + 1),
		g_toggleKey);
	return true;
}
