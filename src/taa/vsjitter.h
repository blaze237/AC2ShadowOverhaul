// Vertex shader rewrite that lets us move the whole rendered image by a fraction of a pixel.
//
// Every game vertex shader whose position output depends on a shader constant (that is, it
// transforms geometry with the game's matrices) gets two instructions appended at the end of main:
//
//     mad rT.xy, c255.xy, rT.w, rT.xy     // clip.xy += jitter * clip.w  (= NDC offset of jitter)
//     mov oPos, rT
//
// where every write to the position output is redirected into the free temp rT. With c255 = 0 the
// result is bit-identical to the original. c255 is unused by all 106 vertex shaders in AC2's frame
// dump (the highest constant used is c245, the end of g_BoneMatrixArray); shaders that do use it,
// that address constants relatively without a constant table, that have no free temp, or whose
// position doesn't depend on a constant (full-screen passes) are left alone.
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace taa
{

enum { kJitterReg = 255 };

struct VsJitterInfo
{
	int posType = -1;   // register type of the position output (6 = o#, 4 = oPos)
	int posReg = -1;    // its number
	int temp = -1;      // temp register the position goes through
	size_t insertAt = 0; // token index (original code) where the two instructions are inserted
	int wvpReg = -1;    // g_WorldViewProj (4 registers), -1 if not in the constant table
	int worldReg = -1;  // g_World (4 registers)
};

// Returns true and fills `out` when the shader was rewritten; otherwise `reason` says why not.
// `info` is filled as far as the analysis got (the matrices are found even when not rewritten).
bool VsJitter_Rewrite(const DWORD* code, std::vector<DWORD>& out, VsJitterInfo& info, std::string& reason);

} // namespace taa
