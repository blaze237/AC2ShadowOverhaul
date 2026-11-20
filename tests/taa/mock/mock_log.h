// Draw log shared by the mock d3d9.dll and the end-to-end test (GetProcAddress(d3d9, "MockGetLog")).
#pragma once
#include <d3d9.h>
#include <vector>

struct MockDraw
{
	IDirect3DSurface9* rt0;
	IDirect3DSurface9* rt1;
	float c255[4];
	IDirect3DPixelShader9* ps;
	IDirect3DVertexShader9* vs;
	IDirect3DBaseTexture9* tex0;
	DWORD lodBias0;
	DWORD colorWrite;
};
struct MockLog
{
	std::vector<MockDraw> draws;
	std::vector<std::vector<DWORD>> vsCode; // every vertex shader created, as the mock received it
	int resets = 0;
};
