// See shaderinfo.h. Layout (d3dx9shader.h): the comment token, 'CTAB', then a D3DXSHADER_CONSTANTTABLE
// header whose offsets count from the 'CTAB' word's end: +12 constant count, +16 offset of the
// D3DXSHADER_CONSTANTINFO array (20 bytes each: name offset, register set, register index, register
// count, reserved, type info offset, default value offset). Type info: class, type, rows, columns.
#include "core/shaderinfo.h"
#include <string.h>

namespace core
{

bool ReadConstantTable(const DWORD* code, ConstantTable& out, size_t maxTokens)
{
	out.found = false;
	out.constants.clear();
	if (!code || (code[0] & 0xFFFE0000) != 0xFFFE0000) // vs (FFFE) or ps (FFFF) version token
		return false;
	const int major = (int)((code[0] >> 8) & 0xFF);
	for (size_t i = 1; i < maxTokens;)
	{
		const DWORD tok = code[i];
		if (tok == 0x0000FFFF) // end
			break;
		if ((tok & 0xFFFF) != 0xFFFE)
		{
			// An instruction. The table comes before any, but shaders rewritten by the effects may have
			// instructions inserted ahead of later comments, so keep walking (shader model 2+ tokens say
			// how many parameters follow; 1.x ones don't, and have no table worth reading).
			if (major < 2)
				break;
			i += 1 + ((tok >> 24) & 0xF);
			continue;
		}
		const size_t len = (tok >> 16) & 0x7FFF;
		if (len < 8 || i + 1 + len > maxTokens || code[i + 1] != 0x42415443) // 'CTAB'
		{
			i += 1 + len;
			continue;
		}
		const unsigned char* base = (const unsigned char*)&code[i + 2];
		const size_t size = (len - 1) * 4;
		auto rd32 = [&](size_t o) -> DWORD {
			DWORD v = 0;
			if (o + 4 <= size)
				memcpy(&v, base + o, 4);
			return v;
		};
		auto rd16 = [&](size_t o) -> int {
			WORD v = 0;
			if (o + 2 <= size)
				memcpy(&v, base + o, 2);
			return v;
		};
		const DWORD count = rd32(12), infoOff = rd32(16);
		for (DWORD k = 0; k < count && k < 4096 && infoOff + 20 * (size_t)(k + 1) <= size; k++)
		{
			const size_t e = infoOff + 20 * (size_t)k;
			ShaderConstant c;
			const DWORD nameOff = rd32(e), type = rd32(e + 12);
			c.set = rd16(e + 4);
			c.reg = rd16(e + 6);
			c.count = rd16(e + 8);
			for (size_t p = nameOff; p < size && base[p]; p++)
				c.name += (char)base[p];
			if (type + 8 <= size)
			{
				c.cls = rd16(type);
				c.rows = rd16(type + 4);
				c.cols = rd16(type + 6);
			}
			out.constants.push_back(c);
		}
		out.found = true;
		return true;
	}
	return false;
}

const ShaderConstant* ConstantTable::Find(const char* name, int set) const
{
	for (const ShaderConstant& c : constants)
		if (c.set == set && c.name == name)
			return &c;
	return nullptr;
}

int ConstantTable::Register(const char* name, int set, int minCount) const
{
	const ShaderConstant* c = Find(name, set);
	return c && c->count >= minCount ? c->reg : -1;
}

int FindConstant(const DWORD* code, const char* name, int set, int* count)
{
	ConstantTable t;
	ReadConstantTable(code, t);
	const ShaderConstant* c = t.Find(name, set);
	if (count)
		*count = c ? c->count : 0;
	return c ? c->reg : -1;
}

CameraRegisters FindCameraRegisters(const ConstantTable& table)
{
	// A float4x4 takes 4 registers at most; fewer would mean the compiler dropped rows (a world matrix
	// used for positions only needs 3), and then the camera can't be recovered from it.
	CameraRegisters r;
	r.wvp = table.Register("g_WorldViewProj", RS_FLOAT4, 4);
	r.world = table.Register("g_World", RS_FLOAT4, 4);
	return r;
}

} // namespace core
