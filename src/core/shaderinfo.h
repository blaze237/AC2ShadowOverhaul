// Shader constant tables: which register the HLSL compiler gave each named uniform of a shader.
//
// Every shader the game creates was compiled from HLSL with a constant table ("CTAB" comment, the
// first comment of the bytecode, before any instruction): the names of its uniforms, their register
// set (bool / int / float / sampler), first register and register count. The game's shaders share
// names (g_WorldViewProj, g_World, g_ShadowedDirect, g_ShadowSampler, ...) but not registers, so the
// effects look each one up by name when the shader is created and read its value at draw time.
//
// One reader for vertex and pixel shaders of any version with a table (vs/ps 2.x and 3.0); the
// shader rewrites, the effects' constant lookups and the frame dump all use it.
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace core
{

// Register sets, as in D3DXREGISTER_SET.
enum RegisterSet
{
	RS_BOOL = 0,
	RS_INT4 = 1,
	RS_FLOAT4 = 2,
	RS_SAMPLER = 3,
};

struct ShaderConstant
{
	std::string name;
	int set = 0;   // RegisterSet
	int reg = 0;   // first register
	int count = 0; // registers it occupies (the compiler leaves out trailing rows the shader never reads)
	int cls = 0, rows = 0, cols = 0; // its type: D3DXPARAMETER_CLASS, rows, columns
};

struct ConstantTable
{
	bool found = false; // the shader has a constant table
	std::vector<ShaderConstant> constants;

	const ShaderConstant* Find(const char* name, int set) const;
	// First register of `name` in `set`, or -1 if it isn't there or has fewer than minCount registers.
	int Register(const char* name, int set, int minCount = 1) const;
};

// Reads the constant table of vs / ps bytecode. Scans at most maxTokens tokens (the code's length
// when known). Returns false if there is no table.
bool ReadConstantTable(const DWORD* code, ConstantTable& out, size_t maxTokens = (size_t)1 << 18);

// One named constant: its first register (and register count), or -1.
int FindConstant(const DWORD* code, const char* name, int set, int* count = nullptr);

// Where a game vertex shader keeps the camera matrices: g_WorldViewProj and g_World, 4 float
// registers each, holding the matrix rows (clip.x = dot(position, c[wvp]) and so on).
struct CameraRegisters
{
	int wvp = -1, world = -1;
	bool Found() const { return wvp >= 0 && world >= 0; }
};
CameraRegisters FindCameraRegisters(const ConstantTable& table);

} // namespace core
