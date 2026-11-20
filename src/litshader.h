// The game's lit opaque pixel shaders: one lighting dataflow analysis, and one rewrite that adds
// what ambient occlusion and SSGI need to them in a single pass (litshader.cpp).
//
//   AO   oC2 = the game's baked indirect light x (1 - fog), oC3 = shadowed sun light x (1 - fog)
//        (the AO composite removes (1 - AO) x oC2 + (1 - contact shadow) x oC3 from the scene)
//   SSGI the indirect term gets albedo x tex2D(s13, reprojected position) added before the combine
//
// With both, oC2 is captured from the game's own indirect term, before the bounce light is added
// to it: SSGI's trace already accounts for what occludes the bounce within its radius, so AO must
// not darken it a second time. Each part is added on its own wherever the other doesn't apply.
#pragma once

#include <windows.h>
#include <string>
#include <vector>

struct LitParts
{
	bool ao = false; // AO / contact shadow outputs oC2, oC3
	bool gi = false; // SSGI bounce light
};

struct LitRewriteResult
{
	bool ao = false;        // the AO outputs were added (with the light, or 0 where it wasn't found)
	bool aoMatched = false; // ... and carry the light
	bool gi = false;        // the bounce light was added
	std::string aoWhy;      // why AO was not added or not matched
	std::string giWhy;      // why the bounce light was not added
};

// Rewrites `code` (the game's shader, possibly already rewritten for soft shadows) with the parts
// asked for that apply to it. Returns false (out untouched) if none does. Shaders that don't write
// the z/w target (oC1) are never changed.
bool Lit_RewritePS(const DWORD* code, LitParts parts, std::vector<DWORD>& out, LitRewriteResult& r);
