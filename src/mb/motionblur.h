// Camera motion blur for AC2, at engine level.
//
// At the first read of the scene colour (after TAA's resolve, before bloom and tone mapping), every
// pixel's motion over the last frame is worked out from the camera's: its position from the game's
// z/w target, reprojected with last frame's camera. Then a reconstruction filter
// (shaders/motionblur.hlsl: tile max, neighbour max and a depth-aware gather) blurs the HDR scene
// colour along it. Only the camera's motion blurs: the game has no motion vectors for its
// characters, and a figure the camera follows moves with the camera anyway.
//
// It runs on its own; with TAA it blurs TAA's resolved image (TAA's history stays sharp). Nothing
// runs while the camera stands still. The menu is [Menu] MotionBlurMenuKey (F6, mb/menu.cpp).
#pragma once

#include <windows.h>
#include <d3d9.h>

namespace mb
{

// [MotionBlur] settings; returns Enabled.
bool Mb_Init();
bool Mb_Enabled();
bool Mb_Active(); // enabled and switched on

struct MbSettings
{
	bool on;
	float strength; // scale of the camera's motion that is blurred
	float shutter;  // fraction of a frame's motion that is blurred (0.5 = a 180 degree shutter)
	int maxBlur;    // longest streak, pixels at 1080p (scaled with the resolution)
	int samples;    // gather samples per pixel
	int debugView;  // 0 off, 1 velocity
};
MbSettings Mb_GetSettings();
void Mb_SetSettings(const MbSettings& s); // clamps
const char* Mb_Status(); // one line for the menu

void Mb_OnDeviceCreated(IDirect3DDevice9* dev, DWORD behaviourFlags);
void Mb_OnSetTexture(DWORD stage, IDirect3DBaseTexture9* tex);
void Mb_BeforeDraw(IDirect3DDevice9* dev);                              // the scene read as a texture: the passes
void Mb_BeforeCopyFrom(IDirect3DDevice9* dev, IDirect3DSurface9* src); // StretchRect / GetRenderTargetData source
void Mb_OnPresent(IDirect3DDevice9* dev);
void Mb_OnLostDevice(); // before Reset

} // namespace mb
