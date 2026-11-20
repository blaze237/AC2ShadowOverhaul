// What every effect needs to know about the game's frame: the main pass, its targets, the sky, the
// camera of this frame and the last one. One tracker, fed by the hooks (framedump.cpp) before any
// effect sees the call, so the effects read the same facts.
//
// The frame (from the F8 frame dumps): the sun shadow atlas first, then the main opaque pass with
// RT0 = scene colour (FP16) and RT1 = an R32F render-target texture that every opaque shader fills
// with clip z/w (the complete scene depth). The game unbinds RT1 between its opaque batches (and
// resets RT2 / RT3) and binds it again; after the last batch RT1 is unbound for good, the sky is
// drawn (viewport depth range 1..1), then alpha-blended transparents, then RT0 moves away for
// post-processing (which reads the scene colour as a texture or StretchRects it).
//
// The effects each decide when to run their passes (AO and SSGI at the end of the opaque pass, the
// volumetrics after the sky, TAA at the first read of the scene colour): those triggers are tuned to
// the real game frame and stay with the effects. What they share comes from here: the main pass, the
// camera, and the sun and fog of the game's lighting (contact shadows and the volumetrics both use
// the sun direction; they used to read it each on their own).
#pragma once

#include <windows.h>
#include <d3d9.h>
#include "core/camera.h"

namespace core
{

// What binding `s` as render target 1 next to `rt0` is. The game's z/w target: an R32F render
// target, not multisampled, the size of RT0, and a texture (the effects sample it).
struct ZwBinding
{
	bool zw = false;          // R32F render target, not multisampled, the size of RT0
	bool zwTexture = false;   // ... and a texture
	bool colourMsaa = false;  // RT0 is multisampled (the game's anti-aliasing is on)
	IDirect3DTexture9* colourTexture = nullptr; // RT0's texture (not referenced), null if it isn't one
	UINT width = 0, height = 0;
	// The main pass, as the effects can use it (with MSAA the z/w and colour targets can't be read).
	bool MainPass() const { return zw && zwTexture && !colourMsaa; }
};
ZwBinding InspectRt1(IDirect3DSurface9* s, IDirect3DSurface9* rt0);

// The sky (and the sun) is drawn behind everything with the viewport's depth range at 1..1.
inline bool IsSkyViewport(const D3DVIEWPORT9& vp) { return vp.MinZ >= 0.99f; }

struct SceneState
{
	unsigned frame = 0; // frames presented so far

	// The game's current bindings (not referenced).
	IDirect3DSurface9* rt0 = nullptr;
	IDirect3DSurface9* rt1 = nullptr;
	D3DVIEWPORT9 viewport = {}; // D3D9 resets it to the whole target when RT0 changes

	// This frame's main pass (the targets of its latest batch; not referenced).
	bool passSeen = false;     // it started this frame
	bool inOpaquePass = false; // its z/w target is bound as RT1 (false between batches)
	IDirect3DSurface9* sceneColour = nullptr;
	IDirect3DTexture9* sceneColourTexture = nullptr; // null if the scene colour isn't a texture
	IDirect3DSurface9* zw = nullptr;
	UINT width = 0, height = 0;
	bool skySeen = false; // a sky draw into the scene colour after the opaque pass this frame

	// The game's anti-aliasing (MSAA) was seen on the main pass recently (its z/w or colour target
	// multisampled), so the screen-space effects can't run. (A frame number, -1000 = never.)
	int msaaFrame = -1000;

	// The camera of this frame's first opaque draw that has g_WorldViewProj and g_World (ok = false
	// until then), and of the last frame that had one.
	Camera camera, prevCamera;

	// The sun and the fog, read from the pixel shader constants of the opaque draws: g_ShadowedDirect
	// (m_Direction, towards the sun, then m_Color) and g_FogColor, world space. The *Ok flags say
	// they were seen this frame; the values stay from the last frame that had them (contact shadows
	// keep using the last known direction, the volumetrics want this frame's).
	bool sunOk = false;       // sunDir read this frame
	bool sunColourOk = false; // sunColour read this frame
	bool fogOk = false;       // fogColour read this frame
	bool sunKnown = false;    // sunDir read in some frame
	float sunDir[3] = {};     // unit vector towards the sun
	float sunColour[3] = {};  // (negative components clamped to 0)
	float fogColour[4] = {};
};
const SceneState& Scene();
bool Scene_MsaaRecently(); // MSAA seen on the main pass in the last 120 frames
bool Scene_CameraCut();    // this frame's camera jumped from the last frame's (see IsCameraCut)

// ---- Hooks (framedump.cpp calls these first, before the effects) -------------------------------

void Scene_OnCreateVertexShader(IDirect3DVertexShader9* vs, const DWORD* code); // where its camera matrices are
void Scene_OnCreatePixelShader(IDirect3DPixelShader9* ps, const DWORD* code);  // where its sun and fog constants are
void Scene_AfterSetRenderTarget(DWORD index, IDirect3DSurface9* s);
void Scene_AfterSetViewport(const D3DVIEWPORT9* vp);
void Scene_BeforeDraw(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs, IDirect3DPixelShader9* ps); // may capture the camera, sun, fog
void Scene_EndFrame();     // at Present, after the effects: this frame's camera becomes the previous one
void Scene_OnLostDevice();  // before Reset: this frame and the cameras are forgotten
void Scene_OnDeviceReady(IDirect3DSurface9* backBuffer); // after creation / Reset: RT0 is the back buffer

} // namespace core
