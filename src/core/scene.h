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
//
// TAA (taa/taa.h) moves the scene draws by a sub-pixel offset each frame and averages the frames.
// What the other effects need to know about that is here too (SceneState::taa, rasterCamera),
// reported by TAA through Scene_TaaJitter / Scene_TaaFrame: the effects never ask TAA itself. They
// work without it; with it they reconstruct positions from the jittered z/w target with the jittered
// projection, and the fog (Scene_NoiseOffset) varies its noise per frame for TAA to average.
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

// TAA as the other effects see it, for one frame.
//
// Timing: TAA decides a frame's jitter at the Present before it, puts it into c255 when the game
// binds a scene-sized target, and resolves at the first read of the scene colour, after every other
// effect. `running`, `assist`, `historyValid` and `frame` are set when TAA recognises this frame's
// main pass (the first binding of its z/w target); the jitter when the camera is read (the first
// opaque draw with the camera matrices, which already has the jitter in c255). Both come before
// every effect pass of the frame (AO and SSGI at the end of the opaque pass, the volumetrics after
// the sky). At Present this frame's jitter becomes `prev*` and the rest is cleared.
//
// Without TAA (disabled in the ini, switched off in the menu, failed, or the scene multisampled)
// everything here stays false / 0.
struct TaaFrameState
{
	bool running = false;      // TAA resolves this frame: it is on and recognised this frame's main pass
	bool assist = false;       // ... and [TAA] AssistEffects: the effects may lean on its averaging
	bool historyValid = false; // TAA resolved the frame before too (so it blends this one with a
	                           // history); false on the first frame after it was switched on, a
	                           // frame it couldn't resolve, a Reset or a resolution change
	unsigned frame = 0;        // TAA's frame counter (its Halton phase is (frame & 7) + 1)

	// The sub-pixel offset the scene draws were rasterised with this frame (c255 at the camera's
	// draw): clip.xy += jitterNdc * clip.w, so pixel q shows the unjittered image at q - jitterPx.
	bool jittered = false;
	float jitterNdc[2] = {}; // NDC units (= c255.xy; y up)
	float jitterPx[2] = {};  // pixels (x right, y down)
	// The same for the frame of prevCamera (0 if it wasn't jittered).
	bool prevJittered = false;
	float prevJitterNdc[2] = {};
	float prevJitterPx[2] = {};
};

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
	// until then), and of the last frame that had one. These are the game's matrices, unjittered (TAA
	// adds its offset inside the vertex shaders): TAA's own reprojection wants them so.
	Camera camera, prevCamera;
	// The same cameras as the scene was rasterised: with that frame's TAA jitter folded into the
	// projection (Jittered in camera.h; equal to camera / prevCamera when not jittered). Anything that
	// turns the z/w target and a pixel position into a view-space position or ray, or maps a point
	// back to a pixel of a jittered target, must use these: with the unjittered projection every
	// pixel would be off by the jitter (up to half a pixel). Reprojecting into an earlier frame's
	// target uses that frame's raster camera (its own jitter).
	Camera rasterCamera, prevRasterCamera;
	TaaFrameState taa;

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

// The fog may lean on TAA this frame: it runs and [TAA] AssistEffects is on. Only the fog does: TAA
// has no motion vectors, so on characters it rejects its history, and per-frame noise in AO, contact
// shadows or SSGI showed through there as noise and swimming (see ao.cpp's top).
bool Scene_TaaAssist();
// An offset (0..1) to add to per-pixel noise before frac(): the golden ratio sequence over TAA's
// frames while Scene_TaaAssist(), so successive frames sample other steps; 0 otherwise, the same
// pattern every frame as without TAA (frac(n + 0) = n: the output doesn't change at all).
float Scene_NoiseOffset();

// ---- Hooks (framedump.cpp calls these first, before the effects) -------------------------------

void Scene_OnCreateVertexShader(IDirect3DVertexShader9* vs, const DWORD* code); // where its camera matrices are
void Scene_OnCreatePixelShader(IDirect3DPixelShader9* ps, const DWORD* code);  // where its sun and fog constants are
void Scene_AfterSetRenderTarget(DWORD index, IDirect3DSurface9* s);
void Scene_AfterSetViewport(const D3DVIEWPORT9* vp);
void Scene_BeforeDraw(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs, IDirect3DPixelShader9* ps); // may capture the camera, sun, fog
void Scene_EndFrame();     // at Present, after the effects: this frame's camera becomes the previous one
void Scene_OnLostDevice();  // before Reset: this frame and the cameras are forgotten
void Scene_OnDeviceReady(IDirect3DSurface9* backBuffer); // after creation / Reset: RT0 is the back buffer

// ---- TAA reports (taa.cpp only) ------------------------------------------------------------------

// What c255 holds from now on: TAA's jitter while it draws the scene (`on`), 0 otherwise. The camera
// read takes the value current at its draw.
void Scene_TaaJitter(bool on, const float ndc[2], const float px[2]);
// TAA recognised this frame's main pass and will resolve it.
void Scene_TaaFrame(bool historyValid, unsigned frame, bool assist);

} // namespace core
