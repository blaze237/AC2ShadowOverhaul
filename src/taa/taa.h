// Temporal anti-aliasing for AC2, at engine level.
//
// Jitter: every game vertex shader that transforms geometry adds c255.xy * w to its clip position
// (vsjitter.cpp). c255 holds this frame's sub-pixel offset (Halton 2,3, 8 phases) while the game
// draws into scene-sized render-target textures before the resolve (depth prepass, opaque pass,
// sky, transparents), and 0 otherwise (shadow maps, half-res passes, post-processing, HUD).
//
// Resolve: the main pass is recognised the way AC2ShadowOverhaul's AO does it (an R32F z/w target
// bound as render target 1 next to the scene colour). When the game first reads the scene colour
// after drawing it (its post-processing binds it as a texture, or StretchRect copies it), the
// resolve runs first: this frame + reprojected history -> new history -> back into the scene
// colour. Everything after (bloom, tone mapping, HUD) sees the anti-aliased image.
//
// Camera: this frame's and the history's (core/scene.h, core/camera.h). The matrices in the constants
// are unjittered: the jitter is added in the shader.
//
// The other effects: TAA reports its jitter (what c255 holds) and that it runs this frame to the
// scene tracker (core::Scene_TaaJitter / Scene_TaaFrame), and the effects read it from there:
// they reconstruct positions from the jittered z/w target with the jittered projection and, with
// [TAA] AssistEffects, vary their noise per frame for the resolve to average (core/scene.h).
#pragma once

#include <windows.h>
#include <d3d9.h>
#include <string>
#include <vector>

namespace taa
{

// [TAA] settings; returns Enabled (shaders are only rewritten when enabled at start).
bool Taa_Init();
bool Taa_Enabled();
bool Taa_Active(); // enabled and toggled on

// The live settings (the in-game menu reads and changes them; values are clamped on Set).
struct TaaSettings
{
	bool on;
	float feedback, motionFeedback, sharpness, mipBias, jitterScale, clipBox, depthTolerance;
	int debugView;
	bool assist; // [TAA] AssistEffects: the other effects vary their noise per frame for TAA to average
};
TaaSettings Taa_GetSettings();
void Taa_SetSettings(const TaaSettings& s);
const char* Taa_Status(); // one line for the menu: working, off, waiting for a 3D scene...

// Our own device calls go to the original (unhooked) functions (core/fxpass.h).
void Taa_OnDeviceCreated(IDirect3D9* d3d, UINT adapter, IDirect3DDevice9* dev);

// Vertex shaders: returns true with `out` set when the shader should be created from `out`.
bool Taa_RewriteVS(const DWORD* code, std::vector<DWORD>& out, struct VsJitterInfo& info);

// Device calls (After = after the call was passed on; Before = before).
void Taa_AfterSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s);
void Taa_OnSetTexture(DWORD stage, IDirect3DBaseTexture9* tex);
void Taa_BeforeDraw(IDirect3DDevice9* dev);
void Taa_BeforeCopyFrom(IDirect3DDevice9* dev, IDirect3DSurface9* src); // StretchRect / GetRenderTargetData source
void Taa_AfterVertexConstants(IDirect3DDevice9* dev, UINT start, UINT count);
DWORD Taa_SamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value); // value to pass on
void Taa_OnPresent(IDirect3DDevice9* dev);
void Taa_OnLostDevice(); // before Reset

} // namespace taa
