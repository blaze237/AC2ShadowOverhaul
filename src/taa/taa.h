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
// Camera: ViewProj = g_WorldViewProj * g_World^-1 from the first opaque draw that has both (the
// matrices in the constants are unjittered: the jitter is added in the shader).
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
};
TaaSettings Taa_GetSettings();
void Taa_SetSettings(const TaaSettings& s);
const char* Taa_Status(); // one line for the menu: working, off, waiting for a 3D scene...

// Camera from one draw's g_WorldViewProj and g_World (registers = matrix rows), and the matrix
// taking this frame's view space to the previous frame's clip space (rows), for the resolve.
struct TaaCamera
{
	bool ok = false;
	double VP[4][4];                // view-projection, rows
	double A, B, P00, P11, P02, P12; // z/w = A + B / z; ndc.x = P00 x / z + P02, ndc.y = P11 y / z + P12
};
bool Taa_CameraFromMatrices(const float wvp[16], const float world[16], TaaCamera& cam, char* why, size_t whySize);
bool Taa_ReprojectionMatrix(const TaaCamera& cur, const TaaCamera& prev, float out[4][4]);

// The original (unhooked) device functions, so our own calls don't go through the hooks.
struct TaaDeviceFns
{
	HRESULT(__stdcall* SetRenderTarget)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
	HRESULT(__stdcall* SetDepthStencilSurface)(IDirect3DDevice9*, IDirect3DSurface9*);
	HRESULT(__stdcall* SetTexture)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
	HRESULT(__stdcall* SetSamplerState)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
	HRESULT(__stdcall* DrawPrimitiveUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
	HRESULT(__stdcall* SetVertexShader)(IDirect3DDevice9*, IDirect3DVertexShader9*);
	HRESULT(__stdcall* SetVertexShaderConstantF)(IDirect3DDevice9*, UINT, const float*, UINT);
	HRESULT(__stdcall* CreateVertexShader)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
	HRESULT(__stdcall* CreateTexture)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
};
void Taa_SetDeviceFns(const TaaDeviceFns& fns);
void Taa_OnDeviceCreated(IDirect3D9* d3d, UINT adapter, IDirect3DDevice9* dev);

// Vertex shaders: returns true with `out` set when the shader should be created from `out`.
bool Taa_RewriteVS(const DWORD* code, std::vector<DWORD>& out, struct VsJitterInfo& info);
void Taa_OnVertexShaderCreated(IDirect3DVertexShader9* vs, const struct VsJitterInfo& info);
void Taa_OnSetVertexShader(IDirect3DVertexShader9* vs);

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
