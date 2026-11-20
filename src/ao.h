// Ambient occlusion on the game's indirect (ambient + bounce) light.
//
// Shader side (load time): every pixel shader of the main opaque pass (the ones that also write
// the z/w target as oC1) gets an extra output oC2 = indirect light x (1 - fog), traced through the
// shader's dataflow from g_AmbientCube / the world lightmap to where the sun term is added on.
// Shaders where that can't be traced write 0 (no AO on those surfaces).
//
// Device side: while the game has its z/w target bound as render target 1, our float16 "indirect"
// target is bound as render target 2. When the opaque pass ends (render target 1 unbound, before
// the next draw: the sky), GTAO is computed at half resolution from the z/w target, blurred, and
// (1 - AO) x indirect is subtracted from the scene colour. Fog, sun light, sky and everything
// drawn later (transparents, effects) are untouched.
#pragma once

#include <windows.h>
#include <d3d9.h>
#include <string>
#include <vector>

// [AmbientOcclusion] from the ini. Returns Enabled.
bool AO_Init();
bool AO_Enabled();     // configured on: shaders get the extra output (needs a restart to change)
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

// Contact shadows: short screen-space rays towards the sun through the scene depth, darkening the
// sun light (not the ambient) where the shadow map is too coarse to show them. [ContactShadows].
bool CS_Init();
bool CS_Active();
void CS_SetOn(bool on);
float CS_Length();    // metres
void CS_SetLength(float metres);

// Shader rewrite. Returns false (code untouched) for shaders that don't write oC1, or that can't
// be changed; `matched` says whether the indirect light was found (else oC2 = 0). `reason`
// explains a false return or an unmatched shader.
bool AO_RewritePS(const DWORD* code, std::vector<DWORD>& out, bool& matched, std::string& reason);

// Register of g_ShadowedDirect (the sun direction, its first member) in a ps_3_0 shader, or -1.
int AO_SunRegister(const DWORD* code);

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
};
extern AoRewriteLayout g_aoLastLayout;

// ---- Device side (called from the device hooks) ------------------------------------------------

// Our own device calls go to the original (unhooked) functions (core/fxpass.h).

void AO_OnCreatePixelShader(IDirect3DPixelShader9* ps, const DWORD* code);    // remembers where the sun direction is
// Render target changes: Before may apply AO (returns true if it did); After binds/unbinds ours.
bool AO_BeforeSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s);
void AO_AfterSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s, IDirect3DSurface9* rt0);
bool AO_BeforeDraw(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps); // true if AO was applied first
bool AO_InPass();                             // our target is bound (the opaque pass is drawing)
bool AO_BeforeOther(IDirect3DDevice9* dev);   // Clear, StretchRect, ColorFill, EndScene...
bool AO_BeforePresent(IDirect3DDevice9* dev); // also ends the frame
void AO_OnLostDevice();                       // before Reset: releases our render targets
std::string AO_FrameInfo();                   // the constants of the last AO pass, for the frame dump
