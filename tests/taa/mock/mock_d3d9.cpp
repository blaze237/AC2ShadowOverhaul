// Fake d3d9.dll for the end-to-end test (no Direct3D 9 GPU needed). It keeps the device state the
// plugin reads and restores (constants, textures, sampler and render states, shaders, targets)
// and logs every draw: the render target, c255 (the jitter), the pixel shader, the texture on
// sampler 0 and its mip bias. State blocks snapshot everything but render targets and depth, like
// the real runtime. Sits next to the test exe the way ReShade's d3d9.dll would.
#include "mock_gen.h"
#include <stdio.h>
#include <string.h>
#include <vector>

static const GUID kIID_Tex = { 0x85c31227, 0x3de5, 0x4f00, { 0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5 } };

struct MockTexture;

struct MockSurface : Gen_IDirect3DSurface9
{
	LONG ref = 1;
	D3DSURFACE_DESC desc = {};
	MockTexture* container = nullptr;
	STDMETHOD_(ULONG, AddRef)() override { return ++ref; }
	STDMETHOD_(ULONG, Release)() override { return --ref; } // never freed (keeps pointers unique)
	STDMETHOD(GetDesc)(D3DSURFACE_DESC* d) override
	{
		*d = desc;
		return S_OK;
	}
	STDMETHOD(GetContainer)(REFIID riid, void** out) override;
};

struct MockTexture : Gen_IDirect3DTexture9
{
	LONG ref = 1;
	MockSurface* level = nullptr;
	STDMETHOD_(ULONG, AddRef)() override { return ++ref; }
	STDMETHOD_(ULONG, Release)() override { return --ref; }
	STDMETHOD_(DWORD, GetLevelCount)() override { return 1; }
	STDMETHOD(GetLevelDesc)(UINT i, D3DSURFACE_DESC* d) override
	{
		if (i)
			return D3DERR_INVALIDCALL;
		*d = level->desc;
		return S_OK;
	}
	STDMETHOD(GetSurfaceLevel)(UINT i, IDirect3DSurface9** out) override
	{
		if (i)
			return D3DERR_INVALIDCALL;
		level->AddRef();
		*out = level;
		return S_OK;
	}
};

HRESULT MockSurface::GetContainer(REFIID riid, void** out)
{
	if (container && IsEqualGUID(riid, kIID_Tex))
	{
		container->AddRef();
		*out = static_cast<IDirect3DTexture9*>(container);
		return S_OK;
	}
	*out = nullptr;
	return E_NOINTERFACE;
}

static MockSurface* NewSurface(UINT w, UINT h, D3DFORMAT f, DWORD usage, D3DMULTISAMPLE_TYPE ms = D3DMULTISAMPLE_NONE)
{
	auto s = new MockSurface;
	s->desc.Width = w;
	s->desc.Height = h;
	s->desc.Format = f;
	s->desc.Usage = usage;
	s->desc.MultiSampleType = ms;
	s->desc.Type = D3DRTYPE_SURFACE;
	return s;
}

template <typename Base> struct MockUnknown : Base
{
	STDMETHOD(QueryInterface)(REFIID, void** out) override
	{
		*out = nullptr;
		return E_NOINTERFACE;
	}
	STDMETHOD_(ULONG, AddRef)() override { return 1; }
	STDMETHOD_(ULONG, Release)() override { return 0; }
	STDMETHOD(GetDevice)(IDirect3DDevice9** out) override
	{
		*out = nullptr;
		return E_NOTIMPL;
	}
};

struct MockPS : Gen_IDirect3DPixelShader9
{
	std::vector<DWORD> code;
	STDMETHOD_(ULONG, AddRef)() override { return 1; }
	STDMETHOD_(ULONG, Release)() override { return 0; }
	STDMETHOD(GetFunction)(void* data, UINT* size) override
	{
		*size = (UINT)(code.size() * 4);
		if (data)
			memcpy(data, code.data(), code.size() * 4);
		return S_OK;
	}
};

struct MockVS : MockUnknown<IDirect3DVertexShader9>
{
	std::vector<DWORD> code;
	STDMETHOD(GetFunction)(void* data, UINT* size) override
	{
		*size = (UINT)(code.size() * 4);
		if (data)
			memcpy(data, code.data(), code.size() * 4);
		return S_OK;
	}
};

struct MockDecl : MockUnknown<IDirect3DVertexDeclaration9>
{
	STDMETHOD(GetDeclaration)(D3DVERTEXELEMENT9*, UINT* n) override
	{
		*n = 0;
		return S_OK;
	}
};

// The state a D3DSBT_ALL state block holds (not render targets or depth).
struct State
{
	float vsConst[256][4] = {}, psConst[224][4] = {};
	IDirect3DBaseTexture9* tex[21] = {};
	DWORD samp[21][14] = {};
	DWORD rs[256] = {};
	IDirect3DVertexShader9* vs = nullptr;
	IDirect3DPixelShader9* ps = nullptr;
	D3DVIEWPORT9 vp = {};
};

struct MockStateBlock : MockUnknown<IDirect3DStateBlock9>
{
	State* live = nullptr;
	State saved;
	STDMETHOD(Capture)() override
	{
		saved = *live;
		return S_OK;
	}
	STDMETHOD(Apply)() override
	{
		*live = saved;
		return S_OK;
	}
};

#include "mock_log.h"
static MockLog g_log;
extern "C" __declspec(dllexport) MockLog* WINAPI MockGetLog() { return &g_log; }

static int Slot(DWORD stage)
{
	if (stage < 16)
		return (int)stage;
	if (stage >= D3DDMAPSAMPLER && stage <= D3DVERTEXTEXTURESAMPLER3)
		return 16 + (int)(stage - D3DDMAPSAMPLER);
	return -1;
}

struct MockDevice : Gen_IDirect3DDevice9
{
	State st;
	IDirect3DSurface9* rts[4] = {};
	IDirect3DSurface9* ds = nullptr;
	MockSurface* bb;
	MockSurface* autoDs;
	UINT freq[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };

	MockDevice(const D3DPRESENT_PARAMETERS* pp)
	{
		bb = NewSurface(pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, D3DUSAGE_RENDERTARGET, pp->MultiSampleType);
		autoDs = NewSurface(pp->BackBufferWidth, pp->BackBufferHeight, pp->AutoDepthStencilFormat, D3DUSAGE_DEPTHSTENCIL);
		rts[0] = bb;
		ds = autoDs;
		st.rs[D3DRS_COLORWRITEENABLE] = 0xF;
	}
	STDMETHOD_(ULONG, AddRef)() override { return 1; }
	STDMETHOD_(ULONG, Release)() override { return 0; }
	STDMETHOD(GetDeviceCaps)(D3DCAPS9* caps) override
	{
		memset(caps, 0, sizeof(*caps));
		caps->DeviceType = D3DDEVTYPE_HAL;
		caps->NumSimultaneousRTs = 4;
		caps->PrimitiveMiscCaps = D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS | D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING;
		caps->PixelShaderVersion = D3DPS_VERSION(3, 0);
		caps->VertexShaderVersion = D3DVS_VERSION(3, 0);
		caps->MaxPixelShader30InstructionSlots = 32768;
		caps->MaxPShaderInstructionsExecuted = 65535;
		return S_OK;
	}
	STDMETHOD(SetRenderTarget)(DWORD i, IDirect3DSurface9* s) override
	{
		if (i >= 4 || (i == 0 && !s))
			return D3DERR_INVALIDCALL;
		rts[i] = s;
		return S_OK;
	}
	STDMETHOD(GetRenderTarget)(DWORD i, IDirect3DSurface9** out) override
	{
		*out = i < 4 ? rts[i] : nullptr;
		if (!*out)
			return D3DERR_NOTFOUND;
		(*out)->AddRef();
		return S_OK;
	}
	STDMETHOD(SetDepthStencilSurface)(IDirect3DSurface9* s) override
	{
		ds = s;
		return S_OK;
	}
	STDMETHOD(GetDepthStencilSurface)(IDirect3DSurface9** out) override
	{
		*out = ds;
		if (!ds)
			return D3DERR_NOTFOUND;
		ds->AddRef();
		return S_OK;
	}
	STDMETHOD(SetRenderState)(D3DRENDERSTATETYPE t, DWORD v) override
	{
		if ((UINT)t < 256)
			st.rs[t] = v;
		return S_OK;
	}
	STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE t, DWORD* v) override
	{
		*v = (UINT)t < 256 ? st.rs[t] : 0;
		return S_OK;
	}
	STDMETHOD(SetSamplerState)(DWORD s, D3DSAMPLERSTATETYPE t, DWORD v) override
	{
		int k = Slot(s);
		if (k < 0 || (UINT)t >= 14)
			return D3DERR_INVALIDCALL;
		st.samp[k][t] = v;
		return S_OK;
	}
	STDMETHOD(GetSamplerState)(DWORD s, D3DSAMPLERSTATETYPE t, DWORD* v) override
	{
		int k = Slot(s);
		*v = k >= 0 && (UINT)t < 14 ? st.samp[k][t] : 0;
		return S_OK;
	}
	STDMETHOD(SetTexture)(DWORD s, IDirect3DBaseTexture9* t) override
	{
		int k = Slot(s);
		if (k < 0)
			return D3DERR_INVALIDCALL;
		st.tex[k] = t;
		return S_OK;
	}
	STDMETHOD(GetTexture)(DWORD s, IDirect3DBaseTexture9** t) override
	{
		int k = Slot(s);
		*t = k >= 0 ? st.tex[k] : nullptr;
		if (*t)
			(*t)->AddRef();
		return S_OK;
	}
	STDMETHOD(SetViewport)(const D3DVIEWPORT9* vp) override
	{
		st.vp = *vp;
		return S_OK;
	}
	STDMETHOD(SetVertexShaderConstantF)(UINT r, const float* v, UINT n) override
	{
		if (r + n > 256)
			return D3DERR_INVALIDCALL;
		memcpy(st.vsConst[r], v, n * 16);
		return S_OK;
	}
	STDMETHOD(GetVertexShaderConstantF)(UINT r, float* v, UINT n) override
	{
		if (r + n > 256)
			return D3DERR_INVALIDCALL;
		memcpy(v, st.vsConst[r], n * 16);
		return S_OK;
	}
	STDMETHOD(SetPixelShaderConstantF)(UINT r, const float* v, UINT n) override
	{
		if (r + n > 224)
			return D3DERR_INVALIDCALL;
		memcpy(st.psConst[r], v, n * 16);
		return S_OK;
	}
	STDMETHOD(GetPixelShaderConstantF)(UINT r, float* v, UINT n) override
	{
		if (r + n > 224)
			return D3DERR_INVALIDCALL;
		memcpy(v, st.psConst[r], n * 16);
		return S_OK;
	}
	STDMETHOD(SetVertexShader)(IDirect3DVertexShader9* vs) override
	{
		st.vs = vs;
		return S_OK;
	}
	STDMETHOD(GetVertexShader)(IDirect3DVertexShader9** vs) override
	{
		*vs = st.vs;
		return S_OK;
	}
	STDMETHOD(SetPixelShader)(IDirect3DPixelShader9* ps) override
	{
		st.ps = ps;
		return S_OK;
	}
	STDMETHOD(GetPixelShader)(IDirect3DPixelShader9** ps) override
	{
		*ps = st.ps;
		return S_OK;
	}
	STDMETHOD(CreateVertexShader)(const DWORD* code, IDirect3DVertexShader9** out) override
	{
		auto vs = new MockVS;
		for (size_t i = 0; code; i++)
		{
			vs->code.push_back(code[i]);
			if (code[i] == 0x0000FFFF)
				break;
		}
		g_log.vsCode.push_back(vs->code);
		*out = vs;
		return S_OK;
	}
	STDMETHOD(CreatePixelShader)(const DWORD* code, IDirect3DPixelShader9** out) override
	{
		auto ps = new MockPS;
		for (size_t i = 0; code; i++)
		{
			ps->code.push_back(code[i]);
			if (code[i] == 0x0000FFFF)
				break;
		}
		*out = ps;
		return S_OK;
	}
	STDMETHOD(CreateStateBlock)(D3DSTATEBLOCKTYPE, IDirect3DStateBlock9** out) override
	{
		auto sb = new MockStateBlock;
		sb->live = &st;
		sb->saved = st;
		*out = sb;
		return S_OK;
	}
	STDMETHOD(CreateVertexDeclaration)(const D3DVERTEXELEMENT9*, IDirect3DVertexDeclaration9** out) override
	{
		*out = new MockDecl;
		return S_OK;
	}
	STDMETHOD(SetStreamSourceFreq)(UINT i, UINT f) override
	{
		if (i < 8)
			freq[i] = f;
		return S_OK;
	}
	STDMETHOD(GetStreamSourceFreq)(UINT i, UINT* f) override
	{
		*f = i < 8 ? freq[i] : 1;
		return S_OK;
	}
	void Note()
	{
		MockDraw d;
		d.rt0 = rts[0];
		d.rt1 = rts[1];
		memcpy(d.c255, st.vsConst[255], 16);
		d.ps = st.ps;
		d.vs = st.vs;
		d.tex0 = st.tex[0];
		d.lodBias0 = st.samp[0][D3DSAMP_MIPMAPLODBIAS];
		d.colorWrite = st.rs[D3DRS_COLORWRITEENABLE];
		g_log.draws.push_back(d);
	}
	STDMETHOD(DrawPrimitive)(D3DPRIMITIVETYPE, UINT, UINT) override
	{
		Note();
		return S_OK;
	}
	STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) override
	{
		Note();
		return S_OK;
	}
	STDMETHOD(DrawPrimitiveUP)(D3DPRIMITIVETYPE, UINT, const void*, UINT) override
	{
		Note();
		return S_OK;
	}
	STDMETHOD(GetBackBuffer)(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9** out) override
	{
		bb->AddRef();
		*out = bb;
		return S_OK;
	}
	STDMETHOD(Reset)(D3DPRESENT_PARAMETERS* pp) override
	{
		g_log.resets++;
		bb = NewSurface(pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, D3DUSAGE_RENDERTARGET);
		rts[0] = bb;
		rts[1] = rts[2] = rts[3] = nullptr;
		State fresh;
		fresh.rs[D3DRS_COLORWRITEENABLE] = 0xF;
		st = fresh;
		return S_OK;
	}
	STDMETHOD(CreateTexture)(UINT w, UINT h, UINT, DWORD usage, D3DFORMAT f, D3DPOOL, IDirect3DTexture9** out, HANDLE*) override
	{
		auto t = new MockTexture;
		t->level = NewSurface(w, h, f, usage);
		t->level->container = t;
		*out = t;
		return S_OK;
	}
	STDMETHOD(CreateRenderTarget)(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD, WINBOOL, IDirect3DSurface9** out, HANDLE*) override
	{
		*out = NewSurface(w, h, f, D3DUSAGE_RENDERTARGET, ms);
		return S_OK;
	}
	STDMETHOD(CreateDepthStencilSurface)(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD, WINBOOL, IDirect3DSurface9** out, HANDLE*) override
	{
		*out = NewSurface(w, h, f, D3DUSAGE_DEPTHSTENCIL, ms);
		return S_OK;
	}
	STDMETHOD(CreateOffscreenPlainSurface)(UINT w, UINT h, D3DFORMAT f, D3DPOOL, IDirect3DSurface9** out, HANDLE*) override
	{
		*out = NewSurface(w, h, f, 0);
		return S_OK;
	}
};

struct MockD3D : Gen_IDirect3D9
{
	STDMETHOD_(ULONG, AddRef)() override { return 1; }
	STDMETHOD_(ULONG, Release)() override { return 0; }
	STDMETHOD_(UINT, GetAdapterCount)() override { return 1; }
	STDMETHOD(CreateDevice)(UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) override
	{
		*out = new MockDevice(pp);
		return S_OK;
	}
	STDMETHOD(GetAdapterDisplayMode)(UINT, D3DDISPLAYMODE* m) override
	{
		memset(m, 0, sizeof(*m));
		m->Width = 1920;
		m->Height = 1080;
		m->Format = D3DFMT_X8R8G8B8;
		return S_OK;
	}
};

extern "C" __declspec(dllexport) IDirect3D9* WINAPI Direct3DCreate9(UINT)
{
	return new MockD3D;
}
