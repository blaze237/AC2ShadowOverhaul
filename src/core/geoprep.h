// The shared half-resolution geometry of the scene: view normal + view z per 2x2 block of the
// game's z/w target (src/core/shaders/geoprep.hlsl), for ambient occlusion and SSGI.
//
// Both reconstructed exactly this from the z/w target at the end of the opaque pass (GTAO inside
// its own pass, SSGI in its prep pass). Now the first of them to run asks for it and it is
// computed once per frame; the other reads the same texture. Two textures alternate between
// frames, so last frame's is still there for SSGI's temporal pass.
//
// Why not the volumetrics too: they march from the same z/w target, but read one depth per fog
// pixel and look depths up again in their blur and upsampling, the same number of fetches from this
// texture as from the z/w target (and twice the bytes each). Their fog resolution is a setting
// (1/1 to 1/4) and their fog pixel stands for its block's centre, not its corner, so sharing would
// buy nothing and shift their upsampling.
//
// Shading normals: when the opaque pass wrote the lit shaders' own normals (ao.h: octahedral, world
// space, in the alpha channels of AO's two full-res targets), the prep uses them, turned into view
// space with the camera's axes, wherever one was written and agrees with the surface the depth
// shows; elsewhere the normal from the depth as before. AO tells it per frame
// (Geo_SetShadingNormals), so SSGI gets them too whether or not AO runs its own passes.
#pragma once

#include <windows.h>
#include <d3d9.h>
#include "core/camera.h"

namespace core
{

// This frame's geometry: computes it if it hasn't been yet this frame, else returns it as is.
// Call between PassState::Save / BeginPasses and the effect's own passes, with the game's z/w
// texture and its size: it changes the pixel shader, render target 0 and viewport, texture and
// sampler 0 (and 1, 2 with shading normals: unbound after), and constants c0-c6 (the effect sets
// its own after). The vertex shader must be a
// pass-through fullscreen one (every effect's is). Null if it can't be made (logged once).
IDirect3DTexture9* Geo_Ensure(IDirect3DDevice9* dev, IDirect3DTexture9* zw, UINT w, UINT h, const CameraFloats& cam);
// The current frame's (null if not computed this frame) and last frame's (null unless computed in
// the frame just before this one at the same size).
IDirect3DTexture9* Geo_Current();
IDirect3DTexture9* Geo_Previous();
// How many times the prep pass has run (tests and the frame dump).
unsigned Geo_Runs();
// This frame's opaque pass wrote the shading normals into the alpha of these full-res textures
// (first and second octahedral component). Applies to the geometry made in this frame only.
void Geo_SetShadingNormals(IDirect3DTexture9* a, IDirect3DTexture9* b);
bool Geo_UsedShadingNormals(); // the current frame's geometry was made with them (tests, frame dump)
void Geo_OnLostDevice(); // before Reset: releases the textures

} // namespace core
