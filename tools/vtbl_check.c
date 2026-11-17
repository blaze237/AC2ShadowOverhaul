/* Compile-only check that the vtable slots hard-coded in src/framedump.cpp match d3d9.h. */
#define CINTERFACE
#include <stddef.h>
#include <d3d9.h>

#define SLOT(vtbl, m, n) _Static_assert(offsetof(vtbl, m) / sizeof(void*) == n, #m)

SLOT(IDirect3D9Vtbl, CreateDevice, 16);
SLOT(IDirect3DDevice9Vtbl, Reset, 16);
SLOT(IDirect3DDevice9Vtbl, Present, 17);
SLOT(IDirect3DDevice9Vtbl, CreateTexture, 23);
SLOT(IDirect3DDevice9Vtbl, CreateRenderTarget, 28);
SLOT(IDirect3DDevice9Vtbl, CreateDepthStencilSurface, 29);
SLOT(IDirect3DDevice9Vtbl, StretchRect, 34);
SLOT(IDirect3DDevice9Vtbl, ColorFill, 35);
SLOT(IDirect3DDevice9Vtbl, SetRenderTarget, 37);
SLOT(IDirect3DDevice9Vtbl, SetDepthStencilSurface, 39);
SLOT(IDirect3DDevice9Vtbl, BeginScene, 41);
SLOT(IDirect3DDevice9Vtbl, EndScene, 42);
SLOT(IDirect3DDevice9Vtbl, Clear, 43);
SLOT(IDirect3DDevice9Vtbl, SetViewport, 47);
SLOT(IDirect3DDevice9Vtbl, SetRenderState, 57);
SLOT(IDirect3DDevice9Vtbl, SetTexture, 65);
SLOT(IDirect3DDevice9Vtbl, DrawPrimitive, 81);
SLOT(IDirect3DDevice9Vtbl, DrawIndexedPrimitive, 82);
SLOT(IDirect3DDevice9Vtbl, DrawPrimitiveUP, 83);
SLOT(IDirect3DDevice9Vtbl, DrawIndexedPrimitiveUP, 84);
SLOT(IDirect3DDevice9Vtbl, CreatePixelShader, 106);
SLOT(IDirect3DDevice9Vtbl, SetPixelShader, 107);
