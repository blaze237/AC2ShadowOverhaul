// Checks the shared constant table reader (src/core/shaderinfo.cpp):
//  - on our own compiled shaders (every effect's shaders_gen.h): every constant the HLSL declares;
//  - on the game's shaders exported by the F8 frame dump (pixel shaders in one folder, vertex shaders
//    in another, given on the command line; skipped if absent): the same table as the readers the
//    effects had before (kept below as references: the frame dump's full one, the shader rewrites'
//    one, and AO / SSGI's camera matrix lookup in vertex shaders);
//  - edge cases: no table, a truncated table, shader model 1.
// Usage: shaderinfo_test.exe [pixel shader folder] [vertex shader folder]
#include "core/shaderinfo.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "shaders_gen.h" // ambient occlusion (src/shaders_gen.h)
#include "ssgi/shaders_gen.h"
#include "vol/shaders_gen.h"
#include "taa/shaders_gen.h"

using namespace core;

static int g_fail = 0;
#define CHECK(cond, ...)                                                                                                              \
	do                                                                                                                                \
	{                                                                                                                                 \
		if (!(cond))                                                                                                                  \
		{                                                                                                                             \
			g_fail++;                                                                                                                 \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                               \
			printf(__VA_ARGS__);                                                                                                      \
			printf("\n");                                                                                                             \
		}                                                                                                                             \
	} while (0)

// ---- The frame dump's reader, as it was (no version check, any comment, type info) ----------------

struct OldEntry
{
	std::string name;
	int set = 0, reg = 0, count = 0, cls = 0, rows = 0, cols = 0;
};

static std::vector<OldEntry> OldReadCtab(const DWORD* code, size_t count)
{
	std::vector<OldEntry> out;
	for (size_t i = 1; i < count;)
	{
		DWORD tok = code[i];
		if (tok == 0x0000FFFF)
			break;
		if ((tok & 0xFFFF) != 0xFFFE)
		{
			i += 1 + ((tok >> 24) & 0xF);
			continue;
		}
		size_t len = (tok >> 16) & 0x7FFF;
		if (len < 8 || i + 1 + len > count || code[i + 1] != 0x42415443 /* 'CTAB' */)
		{
			i += 1 + len;
			continue;
		}
		const unsigned char* base = (const unsigned char*)&code[i + 2];
		size_t size = (len - 1) * 4;
		auto rd32 = [&](size_t off) -> DWORD {
			DWORD v = 0;
			if (off + 4 <= size)
				memcpy(&v, base + off, 4);
			return v;
		};
		auto rd16 = [&](size_t off) -> int {
			WORD v = 0;
			if (off + 2 <= size)
				memcpy(&v, base + off, 2);
			return v;
		};
		DWORD constants = rd32(12), info = rd32(16);
		for (DWORD c = 0; c < constants && c < 4096; c++)
		{
			size_t e = info + (size_t)c * 20;
			if (e + 20 > size)
				break;
			OldEntry ce;
			DWORD nameOff = rd32(e), type = rd32(e + 12);
			ce.set = rd16(e + 4);
			ce.reg = rd16(e + 6);
			ce.count = rd16(e + 8);
			for (size_t p = nameOff; p < size && base[p]; p++)
				ce.name += (char)base[p];
			if (type + 8 <= size)
			{
				ce.cls = rd16(type);
				ce.rows = rd16(type + 4);
				ce.cols = rd16(type + 6);
			}
			out.push_back(ce);
		}
		break;
	}
	return out;
}

// ---- AO's and SSGI's vertex shader lookup, as it was (leading comments only, 4 registers) ---------

static bool OldFindVsMatrices(const DWORD* code, int& wvp, int& world)
{
	wvp = world = -1;
	if (!code || (code[0] & 0xFFFF0000) != 0xFFFE0000)
		return false;
	for (size_t i = 1; i < 4096 && code[i] != 0x0000FFFF;)
	{
		DWORD tok = code[i];
		if ((tok & 0xFFFF) != 0xFFFE)
			return false;
		size_t len = (tok >> 16) & 0x7FFF;
		if (len >= 8 && code[i + 1] == 0x42415443)
		{
			const BYTE* base = (const BYTE*)&code[i + 2];
			size_t size = (len - 1) * 4;
			auto rd32 = [&](size_t o) -> DWORD { return o + 4 <= size ? *(const DWORD*)(base + o) : 0; };
			auto rd16 = [&](size_t o) -> WORD { return o + 2 <= size ? *(const WORD*)(base + o) : 0; };
			DWORD count = rd32(12), infoOff = rd32(16);
			for (DWORD k = 0; k < count && infoOff + 20 * (k + 1) <= size; k++)
			{
				size_t e = infoOff + 20 * k;
				DWORD nameOff = rd32(e);
				WORD set = rd16(e + 4), index = rd16(e + 6), regs = rd16(e + 8);
				char name[32] = {};
				for (size_t c = 0; c < sizeof(name) - 1 && nameOff + c < size && base[nameOff + c]; c++)
					name[c] = (char)base[nameOff + c];
				if (set == 2 && regs == 4 && strcmp(name, "g_WorldViewProj") == 0)
					wvp = index;
				if (set == 2 && regs == 4 && strcmp(name, "g_World") == 0)
					world = index;
			}
			return wvp >= 0 && world >= 0;
		}
		i += 1 + len;
	}
	return false;
}

// ---- Checks ---------------------------------------------------------------------------------------

static std::vector<DWORD> ReadShader(const std::string& path)
{
	std::vector<DWORD> code;
	if (FILE* f = fopen(path.c_str(), "rb"))
	{
		fseek(f, 0, SEEK_END);
		long n = ftell(f);
		fseek(f, 0, SEEK_SET);
		code.resize((size_t)n / 4);
		if (fread(code.data(), 4, code.size(), f) != code.size())
			code.clear();
		fclose(f);
	}
	return code;
}

// The new reader against the frame dump's old one; for vertex shaders also the camera lookup.
static void CheckGameShader(const std::string& name, const std::vector<DWORD>& code, int& tables, int& cameras)
{
	ConstantTable t;
	bool found = ReadConstantTable(code.data(), t, code.size());
	std::vector<OldEntry> ref = OldReadCtab(code.data(), code.size());
	CHECK(found, "%s: no constant table found", name.c_str());
	CHECK(t.constants.size() == ref.size(), "%s: %u constants, the old reader %u", name.c_str(), (unsigned)t.constants.size(),
		(unsigned)ref.size());
	for (size_t i = 0; i < t.constants.size() && i < ref.size(); i++)
	{
		const ShaderConstant& a = t.constants[i];
		const OldEntry& b = ref[i];
		CHECK(a.name == b.name && a.set == b.set && a.reg == b.reg && a.count == b.count && a.cls == b.cls && a.rows == b.rows &&
				a.cols == b.cols,
			"%s: constant %u differs (%s / %s)", name.c_str(), (unsigned)i, a.name.c_str(), b.name.c_str());
		CHECK(t.Find(a.name.c_str(), a.set) != nullptr, "%s: Find(%s) failed", name.c_str(), a.name.c_str());
	}
	tables += found;
	if ((code[0] & 0xFFFF0000) == 0xFFFE0000)
	{
		int wvp, world;
		bool old = OldFindVsMatrices(code.data(), wvp, world);
		CameraRegisters r = FindCameraRegisters(t);
		CHECK(r.Found() == old && (!old || (r.wvp == wvp && r.world == world)), "%s: camera matrices c%d / c%d, before c%d / c%d",
			name.c_str(), r.wvp, r.world, wvp, world);
		cameras += r.Found();
	}
}

static void CheckFolder(const char* dir, const char* label)
{
	if (!dir || !*dir)
	{
		printf("%s: skipped (no folder given)\n", label);
		return;
	}
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((std::string(dir) + "\\*.cso").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
	{
		printf("%s: skipped (no .cso files in %s)\n", label, dir);
		return;
	}
	int shaders = 0, tables = 0, cameras = 0;
	do
	{
		std::vector<DWORD> code = ReadShader(std::string(dir) + "\\" + fd.cFileName);
		if (code.size() < 2)
			continue;
		shaders++;
		CheckGameShader(fd.cFileName, code, tables, cameras);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	printf("%s: %d shaders, %d with a constant table, %d with the camera matrices\n", label, shaders, tables, cameras);
}

// Our own shaders: the constants each one's HLSL declares and uses.
static void CheckOwnShaders()
{
	struct
	{
		const char* label;
		const DWORD* code;
	} const all[] = {
		{ "kAoFullscreenVs", kAoFullscreenVs },
		{ "kAoGtaoPs", kAoGtaoPs },
		{ "kAoBlurPs", kAoBlurPs },
		{ "kAoCompositePs", kAoCompositePs },
		{ "kDepthToColourPs", kDepthToColourPs },
		{ "kGiPrepPs", ssgi::kGiPrepPs },
		{ "kGiTracePs", ssgi::kGiTracePs },
		{ "kGiTemporalPs", ssgi::kGiTemporalPs },
		{ "kGiBlurPs", ssgi::kGiBlurPs },
		{ "kGiDebugPs", ssgi::kGiDebugPs },
		{ "kVolFogMarchPs", vol::kVolFogMarchPs },
		{ "kVolFogBlurPs", vol::kVolFogBlurPs },
		{ "kVolShaftsMaskPs", vol::kVolShaftsMaskPs },
		{ "kVolShaftsBlurPs", vol::kVolShaftsBlurPs },
		{ "kVolCompositePs", vol::kVolCompositePs },
		{ "kTaaResolvePs", taa::kTaaResolvePs },
		{ "kTaaOutputPs", taa::kTaaOutputPs },
	};
	int n = 0;
	for (const auto& s : all)
	{
		size_t len = 0;
		while (s.code[len] != 0x0000FFFF)
			len++;
		std::vector<DWORD> code(s.code, s.code + len + 1);
		int tables = 0, cameras = 0;
		CheckGameShader(s.label, code, tables, cameras);
		n++;
	}
	printf("own shaders: %d checked\n", n);
}

static void CheckEdgeCases()
{
	ConstantTable t;
	// No table: version, one instruction (mov oC0, c0 style tokens), end.
	const DWORD noTable[] = { 0xFFFF0300, 0x02000001, 0x800F0800, 0xA0E40000, 0x0000FFFF };
	CHECK(!ReadConstantTable(noTable, t) && !t.found && t.constants.empty(), "a shader without a table reported one");
	CHECK(FindConstant(noTable, "g_World", RS_FLOAT4) == -1, "FindConstant without a table");
	// Not a shader.
	const DWORD junk[] = { 0x12345678, 0x0000FFFF };
	CHECK(!ReadConstantTable(junk, t), "junk read as a shader");
	CHECK(!ReadConstantTable(nullptr, t), "null read as a shader");
	// A table comment that claims more tokens than the shader has.
	const DWORD truncated[] = { 0xFFFE0300, 0x0020FFFE, 0x42415443, 0x1C, 0, 0xFFFE0300, 0, 0, 0, 0x0000FFFF };
	CHECK(!ReadConstantTable(truncated, t, sizeof(truncated) / 4), "a truncated table was read");
	// Shader model 1 has no parameter counts in its instruction tokens: stop at the first instruction.
	const DWORD sm1[] = { 0xFFFF0104, 0x00000001, 0x800F0000, 0xA0E40000, 0x0000FFFF };
	CHECK(!ReadConstantTable(sm1, t), "a ps_1_4 without a table reported one");
	// A real table, Register's minimum count, and FindCameraRegisters on a pixel shader (none).
	CHECK(ReadConstantTable(taa::kTaaResolvePs, t), "TAA resolve: no table");
	for (const ShaderConstant& c : t.constants)
	{
		CHECK(t.Register(c.name.c_str(), c.set, c.count) == c.reg, "Register(%s, count %d)", c.name.c_str(), c.count);
		CHECK(t.Register(c.name.c_str(), c.set, c.count + 1) == -1, "Register(%s) with too high a count", c.name.c_str());
	}
	CHECK(!FindCameraRegisters(t).Found(), "camera matrices in the TAA resolve shader");
}

int main(int argc, char** argv)
{
	CheckEdgeCases();
	CheckOwnShaders();
	CheckFolder(argc > 1 ? argv[1] : nullptr, "game pixel shaders");
	CheckFolder(argc > 2 ? argv[2] : nullptr, "game vertex shaders");
	printf(g_fail ? "%d FAILURES\n" : "all constant table checks passed\n", g_fail);
	return g_fail ? 1 : 0;
}
