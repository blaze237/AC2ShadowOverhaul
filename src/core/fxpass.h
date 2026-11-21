// Plumbing every effect's fullscreen passes share: the original device functions, saving and
// restoring the game's state around the passes, the fullscreen triangle, render target textures.
//
// The effects draw their passes in the middle of the game's frame, through the original (unhooked)
// device functions, so no effect (and not the frame dump) sees another's draws. Everything they
// touch is put back before the game's next call.
#pragma once

#include <windows.h>
#include <d3d9.h>

namespace core
{

// The original (unhooked) device functions, filled in by the hooks (framedump.cpp HookDevice).
struct DeviceFns
{
	HRESULT(__stdcall* Reset)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
	HRESULT(__stdcall* CreateTexture)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
	HRESULT(__stdcall* StretchRect)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
	HRESULT(__stdcall* ColorFill)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, D3DCOLOR);
	HRESULT(__stdcall* SetRenderTarget)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
	HRESULT(__stdcall* SetDepthStencilSurface)(IDirect3DDevice9*, IDirect3DSurface9*);
	HRESULT(__stdcall* SetViewport)(IDirect3DDevice9*, const D3DVIEWPORT9*);
	HRESULT(__stdcall* SetTexture)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
	HRESULT(__stdcall* SetSamplerState)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
	HRESULT(__stdcall* DrawPrimitiveUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
	HRESULT(__stdcall* CreateVertexShader)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
	HRESULT(__stdcall* SetVertexShader)(IDirect3DDevice9*, IDirect3DVertexShader9*);
	HRESULT(__stdcall* SetVertexShaderConstantF)(IDirect3DDevice9*, UINT, const float*, UINT);
	HRESULT(__stdcall* CreatePixelShader)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
	HRESULT(__stdcall* SetPixelShader)(IDirect3DDevice9*, IDirect3DPixelShader9*);
};
void SetDeviceFns(const DeviceFns& fns);
const DeviceFns& Orig();

// The vertex shader constant that holds TAA's jitter (taa/vsjitter.h kJitterReg): the game's
// rewritten vertex shaders add c255.xy * clip.w to their position. It is non-zero while the game
// draws its scene, which is when AO, SSGI and the volumetrics run their passes.
enum { kJitterRegister = 255 };

// IID_IDirect3DTexture9, for GetContainer (a surface's texture), without linking dxguid.
inline const GUID kIID_Texture9 = { 0x85c31227, 0x3de5, 0x4f00, { 0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5 } };

// The game's state around an effect's passes.
class PassState
{
public:
	HRESULT Create(IDirect3DDevice9* dev); // the state block (D3DPOOL_DEFAULT: released before a Reset)
	void Release();
	bool Ready() const { return m_block != nullptr; }

	// Saves everything the passes touch. The state block holds render and sampler states, shaders,
	// constants, textures and the viewport; render targets, the depth buffer and the vertex input
	// are saved by hand (state blocks don't hold them). The game draws instanced geometry (foliage),
	// and our DrawPrimitiveUP calls reset stream 0 and must run with instancing off: a stale stream
	// or frequency on the game's next instanced draw shows up as long stretched triangles. So the
	// streams and their frequencies are saved too, and instancing is switched off here.
	void Save(IDirect3DDevice9* dev);
	// Fullscreen pass setup: no depth buffer, render targets 1-3 unbound, `decl` and `vs`, and the
	// render states every pass wants (no depth / stencil / alpha test, no blending or culling, all
	// colour channels written, no sRGB, fog or clip planes). c255 (TAA's jitter) is set to 0: our
	// fullscreen vertex shaders are created through the original function, so they are never
	// rewritten to read it, but no pass should run with the scene's jitter in place either way.
	void BeginPasses(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9* decl, IDirect3DVertexShader9* vs);
	// Puts all of it back and drops the references Save took.
	void Restore(IDirect3DDevice9* dev);

private:
	enum { kStreams = 8 };
	IDirect3DStateBlock9* m_block = nullptr;
	IDirect3DSurface9* m_rt[4] = {};
	IDirect3DSurface9* m_ds = nullptr;
	IDirect3DVertexBuffer9* m_vb[kStreams] = {};
	UINT m_offset[kStreams] = {}, m_stride[kStreams] = {}, m_freq[kStreams] = {};
	IDirect3DIndexBuffer9* m_ib = nullptr;
	IDirect3DVertexDeclaration9* m_decl = nullptr;
	DWORD m_fvf = 0;
};

// The vertex declaration of the fullscreen triangle (float4 position), for every effect's passes.
IDirect3DVertexDeclaration9* FullscreenDecl(IDirect3DDevice9* dev);
// A triangle covering the target (the vertex shaders pass the position through).
void DrawFullscreen(IDirect3DDevice9* dev);
// Render target 0 and a viewport covering it.
void SetTarget(IDirect3DDevice9* dev, IDirect3DSurface9* s, UINT w, UINT h);
// Clamped sampling without mipmaps, point or linear filtered, no mip bias. (Through the original
// function, so TAA's mip bias, which it adds to the game's own SetSamplerState calls and to samplers
// 0-15 while the scene is drawn, is replaced by 0 for our passes; the state block puts it back.)
void SetSamplerClamp(IDirect3DDevice9* dev, DWORD sampler, D3DTEXTUREFILTERTYPE filter);

// A render target texture and its surface (D3DPOOL_DEFAULT: released before a Reset).
struct RenderTarget
{
	IDirect3DTexture9* tex = nullptr;
	IDirect3DSurface9* surf = nullptr;
	HRESULT Create(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt);
	void Release();
};

} // namespace core
