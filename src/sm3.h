// Shader model 3 bytecode helpers shared by the shader rewrites (soft shadows, AO output).
#pragma once

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include <initializer_list>
#include "core/shaderinfo.h"

enum
{
	OP_NOP = 0x00, OP_MOV = 0x01, OP_ADD = 0x02, OP_SUB = 0x03, OP_MAD = 0x04, OP_MUL = 0x05, OP_RCP = 0x06, OP_RSQ = 0x07,
	OP_DP3 = 0x08, OP_DP4 = 0x09, OP_MIN = 0x0A, OP_MAX = 0x0B, OP_SLT = 0x0C, OP_SGE = 0x0D, OP_EXP = 0x0E, OP_LOG = 0x0F,
	OP_LRP = 0x12, OP_FRC = 0x13, OP_CALL = 0x19, OP_CALLNZ = 0x1A, OP_LOOP = 0x1B, OP_RET = 0x1C, OP_ENDLOOP = 0x1D,
	OP_LABEL = 0x1E, OP_DCL = 0x1F, OP_POW = 0x20, OP_CRS = 0x21, OP_ABS = 0x23, OP_NRM = 0x24, OP_SINCOS = 0x25,
	OP_REP = 0x26, OP_ENDREP = 0x27, OP_IF = 0x28, OP_IFC = 0x29, OP_ELSE = 0x2A, OP_ENDIF = 0x2B, OP_BREAK = 0x2C,
	OP_BREAKC = 0x2D, OP_DEFB = 0x2F, OP_DEFI = 0x30, OP_TEXKILL = 0x41, OP_TEXLD = 0x42, OP_DEF = 0x51, OP_DP2ADD = 0x5A,
	OP_DSX = 0x5B, OP_DSY = 0x5C, OP_TEXLDD = 0x5D, OP_SETP = 0x5E, OP_TEXLDL = 0x5F, OP_BREAKP = 0x60, OP_CMP = 0x58,
	OP_COMMENT = 0xFFFE, OP_END = 0xFFFF,
};
enum
{
	REG_TEMP = 0, REG_INPUT = 1, REG_CONST = 2, REG_COLOROUT = 8, REG_SAMPLER = 10,
};

static inline int RegType(DWORD t) { return (int)(((t >> 28) & 7) | ((t >> 8) & 0x18)); }
static inline int RegNum(DWORD t) { return (int)(t & 0x7FF); }
static inline int DstMask(DWORD t) { return (int)((t >> 16) & 0xF); }
static inline int SwzComp(DWORD t, int k) { return (int)((t >> (16 + 2 * k)) & 3); }
static inline int SrcMod(DWORD t) { return (int)((t >> 24) & 0xF); }
static inline bool Relative(DWORD t) { return (t & 0x2000) != 0; }

static inline DWORD Dst(int type, int num, int mask)
{
	return 0x80000000u | ((DWORD)(type & 7) << 28) | ((DWORD)(type & 0x18) << 8) | ((DWORD)mask << 16) | (DWORD)num;
}
static inline DWORD Src(int type, int num, int swz, bool neg = false)
{
	return 0x80000000u | ((DWORD)(type & 7) << 28) | ((DWORD)(type & 0x18) << 8) | ((DWORD)swz << 16) | ((neg ? 1u : 0u) << 24) |
		(DWORD)num;
}
static inline int Swz(int a, int b, int c, int d) { return a | (b << 2) | (c << 4) | (d << 6); }
static inline int Rep(int c) { return Swz(c, c, c, c); }
static const int kIdentity = 0xE4;
enum { X = 1, Y = 2, Z = 4, W = 8 };

struct Inst
{
	size_t pos; // index of the instruction token
	int op;
	size_t len; // parameter tokens
	bool predicated;
};

// Splits ps_3_0 bytecode into instructions. endPos receives the index of the end token.
static inline bool Parse(const DWORD* code, std::vector<Inst>& out, size_t& endPos)
{
	if ((code[0] & 0xFFFF0000) != 0xFFFF0000 || (code[0] & 0xFFFF) != 0x0300)
		return false; // ps_3_0 only
	for (size_t i = 1; i < (1u << 18);)
	{
		DWORD tok = code[i];
		if (tok == 0x0000FFFF)
		{
			endPos = i;
			return true;
		}
		Inst in;
		in.pos = i;
		in.op = (int)(tok & 0xFFFF);
		in.predicated = (tok & 0x10000000) != 0;
		in.len = in.op == OP_COMMENT ? ((tok >> 16) & 0x7FFF) : ((tok >> 24) & 0xF);
		out.push_back(in);
		i += 1 + in.len;
	}
	return false;
}

static inline bool IsFlowControl(int op)
{
	return (op >= OP_CALL && op <= OP_LABEL) || (op >= OP_REP && op <= OP_BREAKC) || op == OP_BREAKP;
}

// Instructions whose first parameter is a destination register that gets written.
static inline bool WritesDst(const Inst& in)
{
	if (in.len == 0 || in.op == OP_COMMENT || in.op == OP_DCL || in.op == OP_DEF || in.op == OP_DEFI || in.op == OP_DEFB ||
		in.op == OP_TEXKILL || IsFlowControl(in.op))
		return false;
	return true;
}

static inline bool WritesTemp(const DWORD* code, const Inst& in, int reg, int mask)
{
	if (!WritesDst(in))
		return false;
	DWORD d = code[in.pos + 1];
	return RegType(d) == REG_TEMP && RegNum(d) == reg && (DstMask(d) & mask);
}

// ---- Constant table ----------------------------------------------------------------------------
// Read with core/shaderinfo.h: core::ConstantTable vars; core::ReadConstantTable(code, vars);
// vars.Find("g_ShadowSampler", core::RS_SAMPLER) ...

// Highest temp register referenced (-1 if none) and the float constants that are named in the
// constant table, defined or read. Returns false if constants are addressed relatively.
static inline bool RegisterUsage(const DWORD* code, const std::vector<Inst>& insts, const core::ConstantTable& vars, int& maxTemp,
	bool constUsed[256])
{
	maxTemp = -1;
	memset(constUsed, 0, 256);
	for (const core::ShaderConstant& v : vars.constants)
		if (v.set == core::RS_FLOAT4)
			for (int r = v.reg; r < v.reg + v.count && r < 256; r++)
				constUsed[r] = true;
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
				return false;
			if (RegType(t) == REG_TEMP)
				maxTemp = maxTemp > RegNum(t) ? maxTemp : RegNum(t);
			if (RegType(t) == REG_CONST && RegNum(t) < 256)
				constUsed[RegNum(t)] = true;
			if (in.op == OP_DEF || in.op == OP_DEFI || in.op == OP_DEFB || in.op == OP_DCL)
				break; // only the destination token is a register
		}
	}
	return true;
}

static inline size_t FirstRealInstruction(const std::vector<Inst>& insts, size_t endPos)
{
	for (const Inst& in : insts)
		if (in.op != OP_COMMENT)
			return in.pos;
	return endPos;
}

static inline DWORD FloatBits(float f)
{
	DWORD d;
	memcpy(&d, &f, 4);
	return d;
}

struct Emitter
{
	std::vector<DWORD>& out;
	void I(int op, std::initializer_list<DWORD> params)
	{
		out.push_back((DWORD)op | ((DWORD)params.size() << 24));
		out.insert(out.end(), params.begin(), params.end());
	}
};
