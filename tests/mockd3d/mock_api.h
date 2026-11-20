// What the mock d3d9.dll (mock_d3d9.cpp) records, shared with the test programs. Each record is
// read through an export of the mock: GetProcAddress(GetModuleHandleA("d3d9.dll"), name).
//   MockGetStats   - MockAoStats: counters the shadow / AO test reads (framedump_test.cpp)
//   MockGetGiStats - MockGiStats: the game draw state the SSGI test reads
//   MockGetLog     - MockLog: every draw with its state, every vertex shader created, Presents and
//                    Resets (volumetrics, TAA and combined tests)
#pragma once
#include <d3d9.h>
#include <vector>

struct MockAoStats
{
	int draws;                  // DrawPrimitive(UP) / DrawIndexedPrimitive calls
	int colorFills;
	int stateCaptures, stateApplies; // of the last state block created
	IDirect3DSurface9* lastRt0; // target of the last draw
	DWORD lastBlendOp, lastAlphaBlend;
	IDirect3DPixelShader9* lastPs;
	int instancedUpDraws;       // DrawPrimitiveUP with a stream frequency set (invalid in D3D9)
};

struct MockGiStats
{
	int gameDraws;                // DrawIndexedPrimitive / DrawPrimitive (the "game")
	int upDraws;                  // DrawPrimitiveUP (mods' fullscreen passes)
	int psCreated, psWithS13, psWithOC2;
	IDirect3DBaseTexture9* tex13; // stage 13 at the last game draw
	float c216[3][4];             // c216-c218 at the last game draw
	IDirect3DSurface9* rt[4];     // targets at the last game draw
	IDirect3DSurface9* upRt0;     // target of the last DrawPrimitiveUP
	int instancedUpDraws;         // DrawPrimitiveUP with a stream frequency set (invalid)
	UINT freq1;                   // stream 1 frequency at the last game draw
	IDirect3DPixelShader9* ps;    // pixel shader at the last game draw
};

struct MockDraw
{
	IDirect3DSurface9* rt[4];
	IDirect3DPixelShader9* ps;
	const DWORD* psCode; // bytecode the pixel shader was created from (the mock's copy)
	IDirect3DVertexShader9* vs;
	D3DVIEWPORT9 vp;
	DWORD alphaBlend, srcBlend, destBlend, blendOp, colorWrite;
	IDirect3DBaseTexture9* tex[16];
	float psConst[32][4];
	float c255[4]; // vertex shader constant 255 (the TAA jitter)
	DWORD lodBias0; // sampler 0's mip bias
	bool up;       // DrawPrimitiveUP
};

struct MockLog
{
	std::vector<MockDraw> draws;
	std::vector<std::vector<DWORD>> vsCode; // every vertex shader created, as the mock received it
	int presents = 0;
	int resets = 0;
};

typedef MockAoStats*(WINAPI* PFN_MockGetStats)();
typedef MockGiStats*(WINAPI* PFN_MockGetGiStats)();
typedef MockLog*(WINAPI* PFN_MockGetLog)();

// The mock's exports, looked up in the loaded d3d9.dll (nullptr if it isn't the mock).
template <class T> static T MockExport(const char* name)
{
	HMODULE m = GetModuleHandleA("d3d9.dll");
	return m ? (T)(void*)GetProcAddress(m, name) : nullptr;
}
