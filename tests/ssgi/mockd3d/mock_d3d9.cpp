// Fake d3d9.dll for the end-to-end test on a machine without a D3D9 adapter. Sits next to the test
// exe like a ReShade d3d9.dll proxy would. Records what the test checks (MockGetStats).
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

template <class I> struct MockUnknown : I
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

struct MockDevice;
// State blocks restore shaders, textures, viewport and pixel shader constants (as D3DSBT_ALL does).
struct MockStateBlock : MockUnknown<IDirect3DStateBlock9>
{
	MockDevice* dev;
	IDirect3DPixelShader9* ps = nullptr;
	IDirect3DVertexShader9* vs = nullptr;
	IDirect3DBaseTexture9* tex[16] = {};
	float psConst[224 * 4];
	STDMETHOD(Capture)() override;
	STDMETHOD(Apply)() override;
};

// What happened, for the test exe.
struct MockStats
{
	int gameDraws;                 // DrawIndexedPrimitive / DrawPrimitive (the "game")
	int upDraws;                   // DrawPrimitiveUP (mods' fullscreen passes)
	int psCreated, psWithS13, psWithOC2;
	IDirect3DBaseTexture9* tex13;  // stage 13 at the last game draw
	float c216[3][4];              // c216-c218 at the last game draw
	IDirect3DSurface9* rt[4];      // targets at the last game draw
	IDirect3DSurface9* upRt0;      // target of the last DrawPrimitiveUP
	int instancedUpDraws;          // DrawPrimitiveUP with a stream frequency set (invalid)
	UINT freq1;                    // stream 1 frequency at the last game draw
	IDirect3DPixelShader9* ps;     // pixel shader at the last game draw
};
static MockStats g_stats;
extern "C" __declspec(dllexport) MockStats* WINAPI MockGetStats() { return &g_stats; }

static bool Declares(const std::vector<DWORD>& code, DWORD tok)
{
	for (size_t i = 0; i + 2 < code.size(); i++)
		if ((code[i] & 0xFFFF) == 0x1F && code[i + 2] == tok)
			return true;
	return false;
}
static bool WritesOC2(const std::vector<DWORD>& code)
{
	for (DWORD t : code)
		if ((t & 0x80000000u) && ((t >> 28) & 7) == 0 && ((t >> 8) & 0x18) == 8 && (t & 0x7FF) == 2 && (t & 0x000F0000))
			return true; // a COLOROUT (8) register 2 destination
	return false;
}

struct MockDevice : Gen_IDirect3DDevice9
{
	float psConst[224 * 4] = {}, vsConst[256 * 4] = {};
	IDirect3DPixelShader9* ps = nullptr;
	IDirect3DVertexShader9* vs = nullptr;
	IDirect3DSurface9* rts[4] = {};
	IDirect3DSurface9* ds = nullptr;
	IDirect3DBaseTexture9* tex[16] = {};
	UINT freq[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
	MockSurface* bb;
	MockSurface* autoDs;
	MockDevice(const D3DPRESENT_PARAMETERS* pp)
	{
		bb = NewSurface(pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, D3DUSAGE_RENDERTARGET);
		autoDs = NewSurface(pp->BackBufferWidth, pp->BackBufferHeight, pp->AutoDepthStencilFormat, D3DUSAGE_DEPTHSTENCIL);
		rts[0] = bb;
		ds = autoDs;
	}
	STDMETHOD_(ULONG, AddRef)() override { return 1; }
	STDMETHOD_(ULONG, Release)() override { return 0; }
	STDMETHOD(GetDeviceCaps)(D3DCAPS9* caps) override
	{
		memset(caps, 0, sizeof(*caps));
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
	STDMETHOD(SetTexture)(DWORD stage, IDirect3DBaseTexture9* t) override
	{
		if (stage < 16)
			tex[stage] = t;
		return S_OK;
	}
	STDMETHOD(GetTexture)(DWORD stage, IDirect3DBaseTexture9** t) override
	{
		*t = stage < 16 ? tex[stage] : nullptr;
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
	STDMETHOD(CreateStateBlock)(D3DSTATEBLOCKTYPE, IDirect3DStateBlock9** out) override
	{
		auto sb = new MockStateBlock;
		sb->dev = this;
		*out = sb;
		return S_OK;
	}
	STDMETHOD(CreateVertexDeclaration)(const D3DVERTEXELEMENT9*, IDirect3DVertexDeclaration9** out) override
	{
		*out = new MockDecl;
		return S_OK;
	}
	void NoteGameDraw()
	{
		g_stats.gameDraws++;
		g_stats.tex13 = tex[13];
		memcpy(g_stats.c216, &psConst[216 * 4], sizeof(g_stats.c216));
		memcpy(g_stats.rt, rts, sizeof(rts));
		g_stats.freq1 = freq[1];
		g_stats.ps = ps;
	}
	STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) override
	{
		NoteGameDraw();
		return S_OK;
	}
	STDMETHOD(DrawPrimitive)(D3DPRIMITIVETYPE, UINT, UINT) override
	{
		NoteGameDraw();
		return S_OK;
	}
	STDMETHOD(DrawPrimitiveUP)(D3DPRIMITIVETYPE, UINT, const void*, UINT) override
	{
		for (UINT f : freq)
			if (f != 1)
			{
				g_stats.instancedUpDraws++;
				break;
			}
		g_stats.upDraws++;
		g_stats.upRt0 = rts[0];
		return S_OK;
	}
	STDMETHOD(SetVertexShaderConstantF)(UINT r, const float* v, UINT n) override
	{
		if (r + n > 256)
			return D3DERR_INVALIDCALL;
		memcpy(&vsConst[r * 4], v, n * 16);
		return S_OK;
	}
	STDMETHOD(GetVertexShaderConstantF)(UINT r, float* v, UINT n) override
	{
		if (r + n > 256)
			return D3DERR_INVALIDCALL;
		memcpy(v, &vsConst[r * 4], n * 16);
		return S_OK;
	}
	STDMETHOD(SetPixelShaderConstantF)(UINT r, const float* v, UINT n) override
	{
		if (r + n > 224)
			return D3DERR_INVALIDCALL;
		memcpy(&psConst[r * 4], v, n * 16);
		return S_OK;
	}
	STDMETHOD(GetPixelShaderConstantF)(UINT r, float* v, UINT n) override
	{
		if (r + n > 224)
			return D3DERR_INVALIDCALL;
		memcpy(v, &psConst[r * 4], n * 16);
		return S_OK;
	}
	STDMETHOD(CreateVertexShader)(const DWORD* code, IDirect3DVertexShader9** out) override
	{
		auto s = new MockVS;
		for (size_t i = 0; code; i++)
		{
			s->code.push_back(code[i]);
			if (code[i] == 0x0000FFFF)
				break;
		}
		*out = s;
		return S_OK;
	}
	STDMETHOD(SetVertexShader)(IDirect3DVertexShader9* s) override
	{
		vs = s;
		return S_OK;
	}
	STDMETHOD(GetVertexShader)(IDirect3DVertexShader9** s) override
	{
		*s = vs;
		return S_OK;
	}
	STDMETHOD(CreatePixelShader)(const DWORD* code, IDirect3DPixelShader9** out) override
	{
		auto s = new MockPS;
		for (size_t i = 0; code; i++)
		{
			s->code.push_back(code[i]);
			if (code[i] == 0x0000FFFF)
				break;
		}
		g_stats.psCreated++;
		g_stats.psWithS13 += Declares(s->code, 0xA00F080D);
		g_stats.psWithOC2 += WritesOC2(s->code);
		*out = s;
		return S_OK;
	}
	STDMETHOD(SetPixelShader)(IDirect3DPixelShader9* s) override
	{
		ps = s;
		return S_OK;
	}
	STDMETHOD(GetPixelShader)(IDirect3DPixelShader9** s) override
	{
		*s = ps;
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
		bb = NewSurface(pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, D3DUSAGE_RENDERTARGET);
		autoDs = NewSurface(pp->BackBufferWidth, pp->BackBufferHeight, pp->AutoDepthStencilFormat, D3DUSAGE_DEPTHSTENCIL);
		memset(rts, 0, sizeof(rts));
		memset(tex, 0, sizeof(tex));
		rts[0] = bb;
		ds = autoDs;
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
	STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE st, DWORD* v) override
	{
		*v = st == D3DRS_COLORWRITEENABLE ? 0xF : 1;
		return S_OK;
	}
};

HRESULT MockStateBlock::Capture()
{
	ps = dev->ps;
	vs = dev->vs;
	memcpy(tex, dev->tex, sizeof(tex));
	memcpy(psConst, dev->psConst, sizeof(psConst));
	return S_OK;
}
HRESULT MockStateBlock::Apply()
{
	dev->ps = ps;
	dev->vs = vs;
	memcpy(dev->tex, tex, sizeof(tex));
	memcpy(dev->psConst, psConst, sizeof(psConst));
	return S_OK;
}

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
};

extern "C" __declspec(dllexport) IDirect3D9* WINAPI Direct3DCreate9(UINT)
{
	return new MockD3D;
}
