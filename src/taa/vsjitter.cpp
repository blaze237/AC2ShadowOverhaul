// See vsjitter.h. Token formats: d3d9types.h (D3DSHADER_PARAM_REGISTER_TYPE etc.).
#include "vsjitter.h"
#include <string.h>
#include <stdio.h>

namespace taa
{

namespace
{
enum
{
	OP_NOP = 0x00, OP_MOV = 0x01, OP_MAD = 0x04, OP_CALL = 0x19, OP_CALLNZ = 0x1A, OP_LOOP = 0x1B, OP_RET = 0x1C, OP_ENDLOOP = 0x1D,
	OP_LABEL = 0x1E, OP_DCL = 0x1F, OP_REP = 0x26, OP_ENDREP = 0x27, OP_IF = 0x28, OP_IFC = 0x29, OP_ELSE = 0x2A, OP_ENDIF = 0x2B,
	OP_BREAK = 0x2C, OP_BREAKC = 0x2D, OP_DEFB = 0x2F, OP_DEFI = 0x30, OP_DEF = 0x51, OP_BREAKP = 0x60, OP_COMMENT = 0xFFFE,
};
enum
{
	REG_TEMP = 0, REG_INPUT = 1, REG_CONST = 2, REG_ADDR = 3, REG_RASTOUT = 4, REG_OUTPUT = 6, REG_CONST2 = 11, REG_CONST3 = 12,
	REG_CONST4 = 13, REG_LOOP = 15,
};
const DWORD kEnd = 0x0000FFFF;
const size_t kMaxTokens = 1u << 16;

int RegType(DWORD t) { return (int)(((t >> 28) & 7) | ((t >> 8) & 0x18)); }
int RegNum(DWORD t) { return (int)(t & 0x7FF); }
bool Relative(DWORD t) { return (t & 0x2000) != 0; }
DWORD WithReg(DWORD t, int type, int num)
{
	t &= ~(0x7FFu | (7u << 28) | (3u << 11));
	return t | ((DWORD)(type & 7) << 28) | ((DWORD)(type & 0x18) << 8) | (DWORD)num;
}
DWORD Dst(int type, int num, int mask)
{
	return 0x80000000u | ((DWORD)(type & 7) << 28) | ((DWORD)(type & 0x18) << 8) | ((DWORD)mask << 16) | (DWORD)num;
}
DWORD Src(int type, int num, int swz)
{
	return 0x80000000u | ((DWORD)(type & 7) << 28) | ((DWORD)(type & 0x18) << 8) | ((DWORD)swz << 16) | (DWORD)num;
}
const int kIdentity = 0xE4, kWWWW = 0xFF;

struct Inst
{
	size_t pos; // index of the instruction token
	int op;
	size_t len; // parameter tokens
};

bool IsFlow(int op)
{
	return (op >= OP_CALL && op <= OP_LABEL) || (op >= OP_REP && op <= OP_BREAKC) || op == OP_BREAKP;
}

// Instructions whose first parameter is a destination register.
bool HasDst(const Inst& in)
{
	return in.len > 0 && in.op != OP_COMMENT && in.op != OP_DCL && in.op != OP_DEF && in.op != OP_DEFI && in.op != OP_DEFB &&
		in.op != OP_NOP && !IsFlow(in.op);
}

bool IsConstType(int t) { return t == REG_CONST || t == REG_CONST2 || t == REG_CONST3 || t == REG_CONST4; }

struct CtabFloat
{
	std::string name;
	int index, count;
};

void ReadCtab(const DWORD* code, const std::vector<Inst>& insts, std::vector<CtabFloat>& vars, bool& found)
{
	found = false;
	for (const Inst& in : insts)
	{
		if (in.op != OP_COMMENT || in.len < 8 || code[in.pos + 1] != 0x42415443) // 'CTAB'
			continue;
		found = true;
		const unsigned char* base = (const unsigned char*)&code[in.pos + 2];
		size_t size = (in.len - 1) * 4;
		auto rd32 = [&](size_t o) -> DWORD { return o + 4 <= size ? *(const DWORD*)(base + o) : 0; };
		auto rd16 = [&](size_t o) -> WORD { return o + 2 <= size ? *(const WORD*)(base + o) : 0; };
		DWORD count = rd32(12), infoOff = rd32(16);
		for (DWORD k = 0; k < count && infoOff + 20 * (k + 1) <= size; k++)
		{
			size_t e = infoOff + 20 * k;
			DWORD nameOff = rd32(e);
			if (rd16(e + 4) != 2) // float registers only
				continue;
			CtabFloat v;
			v.index = rd16(e + 6);
			v.count = rd16(e + 8);
			for (size_t c = 0; c < 64 && nameOff + c < size && base[nameOff + c]; c++)
				v.name += (char)base[nameOff + c];
			vars.push_back(v);
		}
		return;
	}
}
} // namespace

bool VsJitter_Rewrite(const DWORD* code, std::vector<DWORD>& out, VsJitterInfo& info, std::string& reason)
{
	out.clear();
	if (!code || (code[0] & 0xFFFF0000) != 0xFFFE0000)
		return reason = "not a vertex shader", false;
	const int major = (code[0] >> 8) & 0xFF;
	if (major < 2)
		return reason = "vs_1_x", false;
	const int maxTemps = major >= 3 ? 32 : 12;

	// Split into instructions.
	std::vector<Inst> insts;
	size_t endPos = 0;
	for (size_t i = 1;;)
	{
		if (i >= kMaxTokens)
			return reason = "no end token", false;
		DWORD tok = code[i];
		if (tok == kEnd)
		{
			endPos = i;
			break;
		}
		Inst in;
		in.pos = i;
		in.op = (int)(tok & 0xFFFF);
		in.len = in.op == OP_COMMENT ? ((tok >> 16) & 0x7FFF) : ((tok >> 24) & 0xF);
		insts.push_back(in);
		i += 1 + in.len;
	}

	// Constant table: the camera matrices, and the float registers it names.
	std::vector<CtabFloat> vars;
	bool haveCtab = false;
	ReadCtab(code, insts, vars, haveCtab);
	bool constNamed[256] = {};
	for (const CtabFloat& v : vars)
	{
		if (v.count == 4 && v.name == "g_WorldViewProj")
			info.wvpReg = v.index;
		if (v.count == 4 && v.name == "g_World")
			info.worldReg = v.index;
		for (int r = v.index; r < v.index + v.count && r < 256; r++)
			constNamed[r] = true;
	}

	// Position output, constants defined in the shader, temps used, our constant used?
	if (major < 3)
	{
		info.posType = REG_RASTOUT;
		info.posReg = 0;
	}
	bool defined[256] = {};
	bool relativeConst = false, relativeOut = false, usesJitterReg = constNamed[kJitterReg];
	int maxTemp = -1;
	for (const Inst& in : insts)
	{
		if (in.op == OP_COMMENT)
			continue;
		if (in.op == OP_DCL && in.len >= 2 && major >= 3)
		{
			DWORD usage = code[in.pos + 1], reg = code[in.pos + 2];
			if (RegType(reg) == REG_OUTPUT && (usage & 0x1F) == 0 /* D3DDECLUSAGE_POSITION */ && ((usage >> 16) & 0xF) == 0)
			{
				info.posType = REG_OUTPUT;
				info.posReg = RegNum(reg);
			}
			continue;
		}
		if ((in.op == OP_DEF || in.op == OP_DEFI || in.op == OP_DEFB) && in.len >= 1)
		{
			DWORD reg = code[in.pos + 1];
			if (RegType(reg) == REG_CONST && RegNum(reg) < 256)
			{
				defined[RegNum(reg)] = true;
				if (RegNum(reg) == kJitterReg)
					usesJitterReg = true;
			}
			continue;
		}
		for (size_t p = 1; p <= in.len; p++)
		{
			DWORD t = code[in.pos + p];
			if (!(t & 0x80000000u))
				continue;
			int type = RegType(t);
			if (type == REG_TEMP)
				maxTemp = maxTemp > RegNum(t) ? maxTemp : RegNum(t);
			if (IsConstType(type) && Relative(t))
				relativeConst = true;
			if (type == REG_CONST && RegNum(t) == kJitterReg)
				usesJitterReg = true;
			if (p == 1 && HasDst(in) && type == info.posType && RegNum(t) == info.posReg && Relative(t))
				relativeOut = true;
		}
	}
	if (info.posReg < 0)
		return reason = "no position output", false;
	if (usesJitterReg)
		return reason = "uses c255", false;
	if (relativeConst && !haveCtab)
		return reason = "relative constant addressing without a constant table", false;
	if (relativeOut)
		return reason = "position written through relative addressing", false;
	const int temp = maxTemp + 1;
	if (temp >= maxTemps)
		return reason = "no free temp register", false;
	info.temp = temp;

	// Does the position depend on a shader constant (anything the game sets, as opposed to def)?
	// Flow-insensitive: a temp is tainted if any write to it reads a tainted value; two passes
	// settle values carried around loops.
	bool taint[32] = {}, posTainted = false, posWritten = false;
	for (int pass = 0; pass < 2; pass++)
		for (const Inst& in : insts)
		{
			if (!HasDst(in))
				continue;
			bool t = false;
			for (size_t p = 2; p <= in.len; p++)
			{
				DWORD s = code[in.pos + p];
				if (!(s & 0x80000000u))
					continue;
				int type = RegType(s);
				if (IsConstType(type) && (Relative(s) || RegNum(s) >= 256 || !defined[RegNum(s)]))
					t = true;
				if (type == REG_TEMP && RegNum(s) < 32 && taint[RegNum(s)])
					t = true;
			}
			DWORD d = code[in.pos + 1];
			if (RegType(d) == REG_TEMP && RegNum(d) < 32 && t)
				taint[RegNum(d)] = true;
			if (RegType(d) == info.posType && RegNum(d) == info.posReg)
			{
				posWritten = true;
				posTainted = posTainted || t;
			}
		}
	if (!posWritten)
		return reason = "position never written", false;
	if (!posTainted)
		return reason = "position does not depend on the game's constants (full-screen pass)", false;

	// End of main: the first label starts the subroutines; main then ends with a ret.
	size_t mainEnd = endPos; // token index to insert before
	size_t mainLast = insts.size(); // instruction index just past main
	for (size_t k = 0; k < insts.size(); k++)
		if (insts[k].op == OP_LABEL)
		{
			mainEnd = insts[k].pos;
			mainLast = k;
			break;
		}
	int depth = 0;
	size_t lastReal = (size_t)-1;
	for (size_t k = 0; k < mainLast; k++)
	{
		const Inst& in = insts[k];
		if (in.op == OP_COMMENT)
			continue;
		if (in.op == OP_IF || in.op == OP_IFC || in.op == OP_LOOP || in.op == OP_REP)
			depth++;
		else if (in.op == OP_ENDIF || in.op == OP_ENDLOOP || in.op == OP_ENDREP)
			depth--;
		else if (in.op == OP_RET && (depth != 0 || k + 1 < mainLast))
		{
			// Only a final ret is fine (comments may follow it).
			bool onlyCommentsAfter = depth == 0;
			for (size_t j = k + 1; j < mainLast && onlyCommentsAfter; j++)
				onlyCommentsAfter = insts[j].op == OP_COMMENT;
			if (!onlyCommentsAfter)
				return reason = "returns early from main", false;
		}
		lastReal = k;
	}
	if (lastReal != (size_t)-1 && insts[lastReal].op == OP_RET)
		mainEnd = insts[lastReal].pos;
	else if (mainLast < insts.size())
		return reason = "subroutines without a final ret in main", false;
	info.insertAt = mainEnd;

	// Emit: copy with position writes redirected to the temp; the epilogue before mainEnd.
	out.reserve(endPos + 16);
	out.push_back(code[0]);
	size_t k = 0;
	for (size_t i = 1; i < endPos;)
	{
		if (i == mainEnd)
			break;
		const Inst& in = insts[k++];
		out.push_back(code[i]);
		for (size_t p = 1; p <= in.len; p++)
		{
			DWORD t = code[in.pos + p];
			if (p == 1 && HasDst(in) && RegType(t) == info.posType && RegNum(t) == info.posReg)
				t = WithReg(t, REG_TEMP, temp);
			out.push_back(t);
		}
		i += 1 + in.len;
	}
	const int X = 1, Y = 2;
	out.push_back((DWORD)OP_MAD | (4u << 24));
	out.push_back(Dst(REG_TEMP, temp, X | Y));
	out.push_back(Src(REG_CONST, kJitterReg, kIdentity));
	out.push_back(Src(REG_TEMP, temp, kWWWW));
	out.push_back(Src(REG_TEMP, temp, kIdentity));
	out.push_back((DWORD)OP_MOV | (2u << 24));
	out.push_back(Dst(info.posType, info.posReg, 0xF));
	out.push_back(Src(REG_TEMP, temp, kIdentity));
	// The rest (final ret, subroutines), with position writes redirected too.
	for (; k < insts.size(); k++)
	{
		const Inst& in = insts[k];
		out.push_back(code[in.pos]);
		for (size_t p = 1; p <= in.len; p++)
		{
			DWORD t = code[in.pos + p];
			if (p == 1 && HasDst(in) && RegType(t) == info.posType && RegNum(t) == info.posReg)
				t = WithReg(t, REG_TEMP, temp);
			out.push_back(t);
		}
	}
	out.push_back(kEnd);
	reason.clear();
	return true;
}

} // namespace taa
