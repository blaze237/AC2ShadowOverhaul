// Ambient occlusion on the game's baked indirect (ambient cube + lightmap) light.
//
// Shader side (load time, litshader.cpp): every pixel shader of the main opaque pass (the ones that
// also write the z/w target as oC1) gets an extra output oC2 = indirect light x (1 - fog), traced
// through the shader's dataflow from g_AmbientCube / the world lightmap to where the sun term is
// added on. Shaders where that can't be traced write 0 (no AO on those surfaces). SSGI's bounce
// light, added to the same term by the same rewrite, is not in oC2: its trace already accounts for
// what occludes it, and AO would darken it twice.
//
// Device side: while the game has its z/w target bound as render target 1, our float16 "indirect"
// target is bound as render target 2 and the "sun" target as 3, each only in frames where something
// reads it: the indirect light for AO (or its debug view), the sun light for contact shadows (or
// its view), both for the shading normals (below). A slot left unbound discards the shader's write
// to it (D3D9: "if oCn is written but there is no corresponding render target, the write is
// ignored"), and the composite then doesn't read that target either.
//
// Shading normals: the rewrite also packs the lit shader's own per-pixel normal (world space, the
// one it evaluates g_AmbientCube with: normal maps included) into the free alpha channels, oC2.w
// and oC3.w (octahedral, + 2, so 0 = none). While [AmbientOcclusion] ShadingNormals is on and AO,
// contact shadows or SSGI run, both targets are bound and handed to the shared geometry prep
// (core/geoprep.h), which uses that normal instead of the one from the depth where there is one. When the opaque pass ends (render target 1 unbound, before
// the next draw: the sky), GTAO is computed at half resolution from the z/w target (normals from the geometry
// prep shared with SSGI, core/geoprep.h), blurred, and
// (1 - AO) x indirect is subtracted from the scene colour. Fog, sun light, sky and everything
// drawn later (transparents, effects) are untouched.
#pragma once

#include <windows.h>
#include <d3d9.h>
#include <string>
#include <vector>

// [AmbientOcclusion] from the ini. Returns Enabled.
bool AO_Init();
bool AO_Enabled();     // AO or contact shadows configured on (needs a restart to change)
// SSGI is configured on (called once at startup, after its ini is read): the opaque shaders then get
// the outputs too, for the shading normals alone if AO and contact shadows are off.
void AO_NoteSsgi(bool configured);
bool AO_Outputs();     // the opaque shaders get oC2 / oC3 (AO, contact shadows or SSGI configured on)
bool AO_Active();      // currently applied (menu switch)
void AO_SetOn(bool on);
float AO_Radius();     // metres
void AO_SetRadius(float metres);
float AO_Strength();   // exponent on the AO term (1 = as computed)
void AO_SetStrength(float s);
// 0 = normal, 1 = show AO only, 2 = show the indirect light buffer, 3 = show contact shadows only,
// 4 = show the sun light buffer
int AO_View();
void AO_SetView(int view);
// [AmbientOcclusion] ShadingNormals: GTAO, contact shadows and SSGI use the lit shaders' normals
// (live switch; off = the normals from the depth, exactly as before).
bool AO_ShadingNormals();
void AO_SetShadingNormals(bool on);

// Contact shadows: short screen-space rays towards the sun through the scene depth, darkening the
// sun light (not the ambient) where the shadow map is too coarse to show them. [ContactShadows].
bool CS_Init();
bool CS_Active();
void CS_SetOn(bool on);
float CS_Length();    // metres
void CS_SetLength(float metres);

// Shader rewrite, the AO outputs alone (Lit_RewritePS in litshader.h does them together with the
// SSGI bounce light). Returns false (code untouched) for shaders that don't write oC1, or that can't
// be changed; `matched` says whether the indirect light was found (else oC2 = 0). `reason`
// explains a false return or an unmatched shader.
bool AO_RewritePS(const DWORD* code, std::vector<DWORD>& out, bool& matched, std::string& reason);

// Where the rewrite put things, for tests.
struct AoRewriteLayout
{
	bool matched = false;
	int temp = -1;        // temp register holding the indirect light
	int sunTemp = -1;     // temp register holding the shadowed sun light
	int constReg = -1;    // constant defined as (0, 1, 0, 0)
	size_t combineAt = 0; // token index (original code) of the "lit = sun * shadow + indirect" mad / add
	DWORD addend = 0;     // its source token carrying the ambient light (zeroing it removes the indirect term)
	DWORD sunSrc = 0;     // its source token carrying the shadow (zeroing it removes the sun term)
	size_t finalAt = 0;   // token index (original code) of the instruction writing oC0.xyz
	bool normal = false;  // the shading normal was found and is packed into oC2.w / oC3.w
	int normalTemp = -1;  // temp register it is copied to
	size_t normalAt = 0;  // token index (original code) of the instruction squaring it for g_AmbientCube
	DWORD normalSrc = 0;  // its source token there (the normal)
};
extern AoRewriteLayout g_aoLastLayout;

// ---- Device side (called from the device hooks) ------------------------------------------------

// Our own device calls go to the original (unhooked) functions (core/fxpass.h).

// Render target changes: Before may apply AO (returns true if it did); After binds/unbinds ours.
bool AO_BeforeSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s);
void AO_AfterSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s, IDirect3DSurface9* rt0);
bool AO_BeforeDraw(IDirect3DDevice9* dev); // true if AO was applied first
bool AO_InPass();                             // our target is bound (the opaque pass is drawing)
bool AO_BeforeOther(IDirect3DDevice9* dev);   // Clear, StretchRect, ColorFill, EndScene...
bool AO_BeforePresent(IDirect3DDevice9* dev); // also ends the frame
void AO_OnLostDevice();                       // before Reset: releases our render targets
std::string AO_FrameInfo();                   // the constants of the last AO pass, for the frame dump
