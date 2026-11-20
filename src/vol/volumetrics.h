// Volumetric fog and sun shafts for Assassin's Creed II.
//
// The game's frame (from AC2 Shadow Overhaul's F8 frame dumps): the sun shadow atlas is drawn
// first, then the main opaque pass with RT0 = scene colour and RT1 = an R32F target that every
// opaque shader fills with clip z/w (the complete scene depth). The game unbinds RT1 between its
// opaque batches and after the last one, then draws the sky (viewport depth range 1..1), then
// alpha-blended transparents, then switches RT0 away for post-processing.
//
// We read what we need from the shader constants of the opaque draws (camera, the shadow atlas's
// light-space matrix and cascade layout, sun direction and colour, fog colour, cloud shadows),
// and after the sky - before the transparents - run our passes and blend the result onto the scene
// colour. See volumetrics.cpp for the frame tracking and src/shaders/volumetrics.hlsl for the passes.
#pragma once
#include <d3d9.h>

namespace vol
{

// Original device functions (our own draws go straight to them, not through our hooks).
struct DeviceFns
{
	HRESULT(__stdcall* SetRenderTarget)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
	HRESULT(__stdcall* SetViewport)(IDirect3DDevice9*, const D3DVIEWPORT9*);
	HRESULT(__stdcall* DrawPrimitiveUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
	HRESULT(__stdcall* SetVertexShader)(IDirect3DDevice9*, IDirect3DVertexShader9*);
	HRESULT(__stdcall* SetPixelShader)(IDirect3DDevice9*, IDirect3DPixelShader9*);
	HRESULT(__stdcall* CreateVertexShader)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
	HRESULT(__stdcall* CreatePixelShader)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
};

// Settings. Written by the ini loader, the hotkey thread and the menu; read on the render thread.
struct VolSettings
{
	bool fogConfigured = true, shaftsConfigured = true; // [..] Enabled: off = never set up
	volatile bool fogOn = true, shaftsOn = true;          // switched from the menu (or optional keys)
	volatile int view = 0; // 0 normal, 1 fog only, 2 fog transmittance, 3 shafts only

	// [VolumetricFog]
	volatile float density = 0.0012f;      // extinction at and below BaseHeight (per metre)
	volatile float heightFalloff = 0.02f;  // density falls by e every 1/HeightFalloff metres above BaseHeight
	volatile float baseHeight = 0.0f;      // world height (m)
	volatile float sunIntensity = 0.35f;   // sunlight scattered (god rays)
	volatile float anisotropy = 0.45f;     // Henyey-Greenstein g: 0 = even, towards 1 = mostly towards the sun
	volatile float ambientIntensity = 0.25f; // sky light scattered (the game's fog colour)
	volatile float maxDistance = 300.0f;   // metres of fog marched
	volatile int steps = 40;               // ray march samples per pixel
	int resolution = 2;                    // fog computed at 1/N of the screen size
	int placement = 0;                     // 0 = after the sky, before transparents; 1 = after transparents

	// [SunShafts]
	volatile float shaftsIntensity = 0.35f;
	volatile float shaftsLength = 0.75f;   // share of the way to the sun the streaks reach back
	volatile float shaftsThreshold = 0.5f; // sky brightness that starts to count
	volatile float shaftsFalloff = 12.0f;  // how tightly the source is gathered around the sun
	volatile float shaftsDecay = 0.93f;

};
extern VolSettings g_set;

bool Vol_Init();          // reads the ini; false if both effects are disabled
void Vol_LoadSettings();  // (re)reads the tunable values from the ini
void Vol_ClampSettings();
void Vol_SetDeviceFns(const DeviceFns& fns);

// Hooks (render thread).
void Vol_OnCreateVertexShader(IDirect3DVertexShader9* vs, const DWORD* code);
void Vol_OnCreatePixelShader(IDirect3DPixelShader9* ps, const DWORD* code);
void Vol_BeforeSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s);
void Vol_AfterSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s);
void Vol_BeforeDraw(IDirect3DDevice9* dev);
void Vol_AfterSetVertexShader(IDirect3DVertexShader9* vs);
void Vol_AfterSetPixelShader(IDirect3DPixelShader9* ps);
void Vol_BeforeStretchRect(IDirect3DDevice9* dev, IDirect3DSurface9* src);
void Vol_BeforePresent(IDirect3DDevice9* dev);
void Vol_OnLostDevice();
void Vol_OnDeviceReady(IDirect3DDevice9* dev);

// Status for the menu.
const char* Vol_Status();

// Tuning menu (menu.cpp).
bool Menu_Init();
void Menu_OnPresent(IDirect3DDevice9* dev);
void Menu_Flush();

} // namespace vol
