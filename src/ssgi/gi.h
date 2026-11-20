// Screen-space global illumination (one bounce, plus more over frames) for Assassin's Creed II.
//
// Shader side (load time, gishader.cpp): every pixel shader of the main opaque pass (the ones that
// write the z/w target as oC1) gets its indirect light term raised by albedo x GI, where GI is read
// from our half-res GI texture (sampler s13) at the pixel's position in the previous frame. The game
// then fogs the result as usual, so the bounce light sits exactly where its own ambient light does.
//
// Device side (gi.cpp): when the opaque pass ends, the GI for the next frame is traced from this
// frame's lit colour and z/w target (horizon-based gathering with a cosine-weighted sector mask, at
// half resolution), accumulated over frames and filtered. Because the lit colour already holds the
// previous bounce, light keeps bouncing over successive frames.
#pragma once

#include <windows.h>
#include <d3d9.h>
#include <string>
#include <vector>

namespace ssgi
{

// Shader constants and sampler shared by every rewritten shader. The game's shaders use c0-c140
// and samplers s0-s4, s12, s14, s15 (F8 dump of 2026-11-19).
enum
{
	GI_SAMPLER = 13,
	GI_CONST = 216, // c216-c218: (z/w, vPos.x, vPos.y, 1) -> (u*w, v*w, w) in last frame's GI texture
};

// ---- Settings ([SSGI] in the ini) --------------------------------------------------------------

bool GI_Init();          // reads the ini, starts the hotkeys; returns Enabled

// Settings the debug menu changes live (clamped to the ini's ranges).
enum GiSettingId
{
	GS_ON,         // SSGI on (the toggle key)
	GS_VIEW,       // debug view 0..3
	GS_INTENSITY,
	GS_RADIUS,     // metres
	GS_MAX_SCREEN, // x screen height
	GS_THICKNESS,  // metres
	GS_SLICES,
	GS_STEPS,
	GS_BLEND,      // temporal blend
	GS_CLAMP,      // radiance clamp
	GS_COUNT
};
float GI_GetSetting(int id);
void GI_SetSetting(int id, float v);

struct GiStatus
{
	unsigned framesTraced = 0; // counts up while the passes run
	bool failed = false;       // resources couldn't be created (see the log)
	unsigned traceW = 0, traceH = 0;
	float gpuMs = -1;          // smoothed GPU time of the passes (-1 = not measured)
};
GiStatus GI_GetStatus();
void GI_SetTiming(bool on); // measure the GPU time (while the menu is shown)
bool GI_Enabled();       // shaders get rewritten (restart to change)
bool GI_Active();        // currently on (toggle key)
float GI_Intensity();
int GI_View();           // 0 = normal, 1 = GI light, 2 = raw trace, 3 = normals, 4 = scene without GI

// ---- Shader rewrite ----------------------------------------------------------------------------

enum
{
	GI_FORM_SUN_ALBEDO,  // mad lit, sun, albedo, indirect
	GI_FORM_AMB_PRODUCT, // mad lit, ambient, albedo, sunLight
	GI_FORM_ADD,         // add lit, sunLight, indirect (albedo from the sun or ambient product earlier)
};

// Returns false (code untouched) for shaders outside the opaque pass or where the lighting can't be
// traced; `reason` says why. Works on the game's code and on code AC2ShadowOverhaul already rewrote.
bool GI_RewritePS(const DWORD* code, std::vector<DWORD>& out, std::string& reason);

// Where the last successful rewrite put things, for tests.
struct GiRewriteLayout
{
	int form = -1;
	size_t combineAt = 0;    // token index (original code) of the combine
	DWORD albedo = 0;        // source token of the albedo factor, as read by the instruction at albedoAt
	size_t albedoAt = 0;     // token index (original code): the combine or an earlier ambient / sun product
	bool albedoCopied = false; // its register changes before the combine, so it is copied at albedoAt
	bool depthExact = false; // z/w copied from the oC1 code (else reprojection assumes "far away")
	int temp = -1, temp2 = -1, constReg = -1;
};
extern GiRewriteLayout g_giLastLayout;

// The camera and the reprojection rows for c216-c218 come from core/camera.h (ReprojectionRows,
// IdentityRows), the camera of each frame from core/scene.h.

// ---- Device side (called from the hooks, framedump.cpp, through features.cpp) -------------------

// Our own device calls go to the original (unhooked) functions (core/fxpass.h).

void GI_OnSetTexture(DWORD stage, IDirect3DBaseTexture9* tex);              // the game's own binding of our stage
// Render target changes: Before may run the GI passes (returns true if it did); After starts/ends the pass.
bool GI_BeforeSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s);
void GI_AfterSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s, IDirect3DSurface9* rt0);
bool GI_BeforeDraw(IDirect3DDevice9* dev); // true if the GI passes ran first
bool GI_InPass();
bool GI_BeforeOther(IDirect3DDevice9* dev);   // Clear, StretchRect, ColorFill, EndScene...
bool GI_BeforePresent(IDirect3DDevice9* dev); // also ends the frame
void GI_OnLostDevice();                       // before Reset: releases our textures

} // namespace ssgi
