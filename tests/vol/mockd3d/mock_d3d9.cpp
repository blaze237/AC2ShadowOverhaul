// Fake d3d9.dll for the end-to-end test on a machine without a display adapter. Sits next to the
// test exe exactly like a ReShade d3d9.dll proxy would, records every draw (render targets,
// shaders, viewport, blending and the first 32 pixel shader constants) for the test to inspect.
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
	MockSurface* levels[4] = {};
	DWORD count = 0;
	STDMETHOD_(ULONG, AddRef)() override { return ++ref; }
	STDMETHOD_(ULONG, Release)() override { return --ref; }
	STDMETHOD_(DWORD, GetLevelCount)() override { return count; }
	STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_TEXTURE; }
	STDMETHOD(GetLevelDesc)(UINT i, D3DSURFACE_DESC* d) override
	{
		if (i >= count)
			return D3DERR_INVALIDCALL;
		*d = levels[i]->desc;
		return S_OK;
	}
	STDMETHOD(GetSurfaceLevel)(UINT i, IDirect3DSurface9** out) override
	{
		if (i >= count)
			return D3DERR_INVALIDCALL;
		levels[i]->AddRef();
		*out = levels[i];
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

struct MockVS : IDirect3DVertexShader9
{
	std::vector<DWORD> code;
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
	STDMETHOD(GetFunction)(void* data, UINT* size) override
	{
		*size = (UINT)(code.size() * 4);
		if (data)
			memcpy(data, code.data(), code.size() * 4);
		return S_OK;
	}
};

// State block: covers the pixel shader, the viewport, render states and shader constants (enough to
// check restores).
struct MockDevice;
struct MockStateBlock : IDirect3DStateBlock9
{
	MockDevice* dev = nullptr;
	IDirect3DPixelShader9* ps = nullptr;
	D3DVIEWPORT9 vp = {};
	DWORD rs[256] = {};
	float vsConst[256 * 4] = {}, psConst[224 * 4] = {};
	int captures = 0, applies = 0;
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
	STDMETHOD(Capture)() override;
	STDMETHOD(Apply)() override;
};

struct MockDecl : IDirect3DVertexDeclaration9
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
	STDMETHOD(GetDeclaration)(D3DVERTEXELEMENT9*, UINT* n) override
	{
		*n = 0;
		return S_OK;
	}
};

// What the test reads back (GetProcAddress(d3d9, "MockGetLog")).
struct MockDraw
{
	IDirect3DSurface9* rt[4];
	IDirect3DPixelShader9* ps;
	const DWORD* psCode; // bytecode the shader was created from (copy)
	D3DVIEWPORT9 vp;
	DWORD alphaBlend, srcBlend, destBlend, blendOp, colorWrite;
	IDirect3DBaseTexture9* tex[16];
	float psConst[32][4];
	bool up; // DrawPrimitiveUP
};
struct MockLog
{
	std::vector<MockDraw> draws;
	int presents = 0;
};
static MockLog g_log;
extern "C" __declspec(dllexport) MockLog* WINAPI MockGetLog() { return &g_log; }

struct MockDevice : Gen_IDirect3DDevice9
{
	float vsConst[256 * 4] = {}, psConst[224 * 4] = {};
	IDirect3DPixelShader9* curPs = nullptr;
	IDirect3DSurface9* rts[4] = {};
	IDirect3DSurface9* ds = nullptr;
	IDirect3DBaseTexture9* tex[20] = {};
	DWORD rs[256] = {};
	D3DVIEWPORT9 vp = {};
	MockSurface* bb;
	MockSurface* autoDs;
	MockDevice(const D3DPRESENT_PARAMETERS* pp)
	{
		bb = NewSurface(pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, D3DUSAGE_RENDERTARGET, pp->MultiSampleType);
		autoDs = NewSurface(pp->BackBufferWidth, pp->BackBufferHeight, pp->AutoDepthStencilFormat, D3DUSAGE_DEPTHSTENCIL, pp->MultiSampleType);
		rts[0] = bb;
		ds = autoDs;
		vp = { 0, 0, pp->BackBufferWidth, pp->BackBufferHeight, 0, 1 };
		rs[D3DRS_COLORWRITEENABLE] = 0xF;
	}
	STDMETHOD_(ULONG, AddRef)() override { return 1; }
	STDMETHOD_(ULONG, Release)() override { return 0; }
	STDMETHOD(SetRenderTarget)(DWORD i, IDirect3DSurface9* s) override
	{
		if (i >= 4 || (i == 0 && !s))
			return D3DERR_INVALIDCALL;
		rts[i] = s;
		if (i == 0)
		{
			D3DSURFACE_DESC d;
			s->GetDesc(&d);
			vp = { 0, 0, d.Width, d.Height, 0, 1 };
		}
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
	STDMETHOD(SetViewport)(const D3DVIEWPORT9* v) override
	{
		vp = *v;
		return S_OK;
	}
	STDMETHOD(GetViewport)(D3DVIEWPORT9* v) override
	{
		*v = vp;
		return S_OK;
	}
	STDMETHOD(SetRenderState)(D3DRENDERSTATETYPE st, DWORD v) override
	{
		if ((unsigned)st < 256)
			rs[st] = v;
		return S_OK;
	}
	STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE st, DWORD* v) override
	{
		*v = (unsigned)st < 256 ? rs[st] : 0;
		return S_OK;
	}
	STDMETHOD(SetTexture)(DWORD stage, IDirect3DBaseTexture9* t) override
	{
		if (stage < 20)
			tex[stage] = t;
		return S_OK;
	}
	STDMETHOD(GetTexture)(DWORD stage, IDirect3DBaseTexture9** out) override
	{
		*out = stage < 20 ? tex[stage] : nullptr;
		if (*out)
			(*out)->AddRef();
		return S_OK;
	}
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
	STDMETHOD(ColorFill)(IDirect3DSurface9*, const RECT*, D3DCOLOR) override { return S_OK; }
	UINT freq[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
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
	void NoteDraw(bool up)
	{
		MockDraw d = {};
		memcpy(d.rt, rts, sizeof(rts));
		d.ps = curPs;
		d.psCode = curPs ? static_cast<MockPS*>(curPs)->code.data() : nullptr;
		d.vp = vp;
		d.alphaBlend = rs[D3DRS_ALPHABLENDENABLE];
		d.srcBlend = rs[D3DRS_SRCBLEND];
		d.destBlend = rs[D3DRS_DESTBLEND];
		d.blendOp = rs[D3DRS_BLENDOP];
		d.colorWrite = rs[D3DRS_COLORWRITEENABLE];
		memcpy(d.tex, tex, sizeof(d.tex));
		memcpy(d.psConst, psConst, sizeof(d.psConst));
		d.up = up;
		g_log.draws.push_back(d);
	}
	STDMETHOD(DrawPrimitive)(D3DPRIMITIVETYPE, UINT, UINT) override
	{
		NoteDraw(false);
		return S_OK;
	}
	STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) override
	{
		NoteDraw(false);
		return S_OK;
	}
	STDMETHOD(DrawPrimitiveUP)(D3DPRIMITIVETYPE, UINT, const void*, UINT) override
	{
		NoteDraw(true);
		return S_OK;
	}
	STDMETHOD(SetVertexShaderConstantF)(UINT r, const float* v, UINT n) override
	{
		if (r + n <= 256)
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
		if (r + n <= 224)
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
		auto vs = new MockVS;
		for (size_t i = 0; code; i++)
		{
			vs->code.push_back(code[i]);
			if (code[i] == 0x0000FFFF)
				break;
		}
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
	STDMETHOD(SetPixelShader)(IDirect3DPixelShader9* ps) override
	{
		curPs = ps;
		return S_OK;
	}
	STDMETHOD(GetPixelShader)(IDirect3DPixelShader9** ps) override
	{
		*ps = curPs;
		return S_OK;
	}
	STDMETHOD(GetBackBuffer)(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9** out) override
	{
		bb->AddRef();
		*out = bb;
		return S_OK;
	}
	STDMETHOD(Reset)(D3DPRESENT_PARAMETERS*) override { return S_OK; }
	STDMETHOD(Present)(const RECT*, const RECT*, HWND, const RGNDATA*) override
	{
		g_log.presents++;
		return S_OK;
	}
	STDMETHOD(CreateTexture)(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL, IDirect3DTexture9** out, HANDLE*) override
	{
		auto t = new MockTexture;
		t->count = levels ? (levels > 4 ? 4 : levels) : 1;
		for (DWORD i = 0; i < t->count; i++)
		{
			t->levels[i] = NewSurface(w >> i, h >> i, f, usage);
			t->levels[i]->container = t;
		}
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
};

HRESULT MockStateBlock::Capture()
{
	captures++;
	ps = dev->curPs;
	vp = dev->vp;
	memcpy(rs, dev->rs, sizeof(rs));
	memcpy(vsConst, dev->vsConst, sizeof(vsConst));
	memcpy(psConst, dev->psConst, sizeof(psConst));
	return S_OK;
}

HRESULT MockStateBlock::Apply()
{
	applies++;
	dev->curPs = ps;
	dev->vp = vp;
	memcpy(dev->rs, rs, sizeof(rs));
	memcpy(dev->vsConst, vsConst, sizeof(vsConst));
	memcpy(dev->psConst, psConst, sizeof(psConst));
	return S_OK;
}

struct MockD3D : Gen_IDirect3D9
{
	STDMETHOD_(ULONG, AddRef)() override { return 1; }
	STDMETHOD_(ULONG, Release)() override { return 0; }
	STDMETHOD_(UINT, GetAdapterCount)() override { return 1; }
	STDMETHOD(GetAdapterIdentifier)(UINT, DWORD, D3DADAPTER_IDENTIFIER9* id) override
	{
		memset(id, 0, sizeof(*id));
		strcpy(id->Description, "Mock adapter");
		return S_OK;
	}
	STDMETHOD(CreateDevice)(UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) override
	{
		*out = new MockDevice(pp);
		return S_OK;
	}
};

extern "C" __declspec(dllexport) IDirect3D9* WINAPI Direct3DCreate9(UINT) { return new MockD3D; }
