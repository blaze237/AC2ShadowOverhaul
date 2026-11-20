// ps_3_0 interpreter for the shader rewrite tests (adapted from AC2ShadowOverhaul tests/aoshader_test.cpp).
// Runs straight-line code and if / else; textures return a hash of (sampler, coordinates), except the
// GI sampler, which returns Inputs::gi and records where it was read.
#pragma once
#include "sm3.h"
#include "ssgi/gi.h"
using namespace ssgi;
#include <stdio.h>
#include <math.h>
#include <string>
#include <vector>

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
	float gi[4] = {};    // what the GI sampler returns
	size_t injectAt = 0; // after the instruction at this token index, add the value of source token
	DWORD injectAlb = 0; // injectAlb (as read by the instruction at captureAt) x gi to its result
	size_t captureAt = 0; // (before _sat)
};

struct Outputs
{
	float oC[4][4];
	bool written[4];
	bool killed;
	int giReads;
	float giUv[2]; // coordinates of the last GI read
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
	float captured[4] = {};
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
		const size_t instPos = i - 1 - len;
		if (in.captureAt && instPos == in.captureAt)
			read(in.injectAlb, captured);
		if (in.zeroAt && instPos == in.zeroAt && RegType(in.zeroSrc) == REG_TEMP)
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
				res[k] = smp == GI_SAMPLER ? in.gi[k] : TexValue(in.texSeed, smp, a[0], a[1], k);
			if (smp == GI_SAMPLER)
			{
				o.giReads++;
				o.giUv[0] = a[0];
				o.giUv[1] = a[1];
			}
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
		if (in.injectAt && instPos == in.injectAt)
			for (int k = 0; k < 3; k++)
				res[k] += captured[k] * in.gi[k];
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

