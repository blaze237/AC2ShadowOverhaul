// The merged effects behind the one Direct3D hook layer (framedump.cpp's hooks call these).
//
// AC2 Shadow Overhaul used to be four plugins (shadows + AO, SSGI, volumetrics, TAA), each with
// its own hooks chained in ASI load order. Now there is one set of hooks, and each device call is
// passed to the effects here in a fixed order:
//
//   end of the opaque pass:  ambient occlusion + contact shadows, then SSGI (traces the AO'd image)
//   after the sky:           volumetric fog + sun shafts
//   first read of the scene: TAA resolve (so everything above is anti-aliased)
//
// Every effect draws its own passes through the original device functions, below the hooks, so no
// effect ever sees another's draws. Text overlays (the menus, the MSAA warning) go through the
// device's vtable, so the hooks pass straight through while g_fxPassthrough is set.
#pragma once

#include <windows.h>
#include <d3d9.h>
#include <vector>
#include <string>
#include <stdint.h>

extern volatile bool g_fxPassthrough;

// The original (unhooked) device functions the effects draw through are core::Orig() (core/fxpass.h).

// Startup: reads the ini of every merged effect and their menus. Returns true if any of them
// needs the device hooks.
bool Fx_Init();
void Fx_Flush(); // at exit: saves pending menu changes

void Fx_OnDeviceCreated(IDirect3D9* d3d, UINT adapter, IDirect3DDevice9* dev, DWORD behaviourFlags, const D3DPRESENT_PARAMETERS* pp);
void Fx_OnLostDevice();                                   // before Reset
void Fx_OnResetDevice(IDirect3DDevice9* dev, const D3DPRESENT_PARAMETERS* pp); // after a successful Reset

// Device calls. Before* may run effect passes first.
void Fx_BeforePresent(IDirect3DDevice9* dev);
void Fx_DrawOverlays(IDirect3DDevice9* dev); // menus + warnings, just before Present
void Fx_BeforeCopyFrom(IDirect3DDevice9* dev, IDirect3DSurface9* src, bool stretchRect); // StretchRect / GetRenderTargetData
void Fx_BeforeOther(IDirect3DDevice9* dev);              // ColorFill, Clear, EndScene
void Fx_BeforeSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s);
void Fx_AfterSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s, IDirect3DSurface9* rt0);
void Fx_BeforeSetTexture(DWORD stage, IDirect3DBaseTexture9* tex);
void Fx_AfterSetTexture(DWORD stage, IDirect3DBaseTexture9* tex);
DWORD Fx_SamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value); // value to pass on
void Fx_AfterVertexConstants(IDirect3DDevice9* dev, UINT start, UINT count);
void Fx_BeforeDraw(IDirect3DDevice9* dev);
void Fx_AfterSetVertexShader(IDirect3DVertexShader9* vs);
void Fx_AfterSetPixelShader(IDirect3DPixelShader9* ps);

// Shaders. RewriteVS returns true with `out` set when the vertex shader should be created from it
// (TAA jitter). OnCreateVertexShader gets the game's code either way.
bool Fx_RewriteVS(const DWORD* code, std::vector<DWORD>& out);
void Fx_OnCreateVertexShader(IDirect3DVertexShader9* vs, const DWORD* gameCode);
// The opaque shaders get the SSGI bounce light (litshader.h; framedump.cpp's CreatePs does the
// rewrite, AO's and SSGI's parts in one go). NoteGiRewrite logs how that went for one shader.
bool Fx_GiShaders();
void Fx_NoteGiRewrite(uint32_t hash, bool applied, const std::string& reason);
void Fx_OnCreatePixelShader(IDirect3DPixelShader9* ps, const DWORD* gameCode);
