// Fake d3d9.dll for testing AC2ShadowOverhaul' frame dump on a machine without a display adapter.
// Sits next to the test exe exactly like a ReShade d3d9.dll proxy would.
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
	bool depthCopy = false; // an INTZ texture was drawn into this target (see DrawPrimitiveUP)
	std::vector<unsigned char> data;
	UINT Bpp() const { return desc.Format == D3DFMT_R32F || desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8 ? 4 : desc.Format == D3DFMT_R16F ? 2 : 8; }
	void Alloc()
	{
		if (data.empty())
			data.resize((size_t)desc.Width * desc.Height * Bpp());
	}
	STDMETHOD(LockRect)(D3DLOCKED_RECT* lr, const RECT*, DWORD) override
	{
		Alloc();
		lr->pBits = data.data();
		lr->Pitch = desc.Width * Bpp();
		return S_OK;
	}
	STDMETHOD(UnlockRect)() override { return S_OK; }
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
	STDMETHOD(GetLevelDesc)(UINT i, D3DSURFACE_DESC* d) override;
	STDMETHOD(GetSurfaceLevel)(UINT i, IDirect3DSurface9** out) override
	{
		if (i >= count)
			return D3DERR_INVALIDCALL;
		levels[i]->AddRef();
		*out = levels[i];
		return S_OK;
	}
};

HRESULT MockTexture::GetLevelDesc(UINT i, D3DSURFACE_DESC* d)
{
	if (i >= count)
		return D3DERR_INVALIDCALL;
	*d = levels[i]->desc;
	return S_OK;
}

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

static const D3DFORMAT kINTZ = (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z');

struct MockStateBlock : IDirect3DStateBlock9
{
	int captures = 0, applies = 0;
	IDirect3DPixelShader9** livePs = nullptr; // the device's current pixel shader (saved and restored)
	IDirect3DPixelShader9* savedPs = nullptr;
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
	STDMETHOD(Capture)() override
	{
		captures++;
		savedPs = *livePs;
		return S_OK;
	}
	STDMETHOD(Apply)() override
	{
		applies++;
		*livePs = savedPs;
		return S_OK;
	}
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

// What the AO passes did, for the test exe (GetProcAddress(d3d9, "MockAoStats")).
struct MockAoStats
{
	int draws;                 // DrawPrimitiveUP / DrawIndexedPrimitive calls
	int colorFills;
	int stateCaptures, stateApplies;
	IDirect3DSurface9* lastRt0; // target of the last draw
	DWORD lastBlendOp, lastAlphaBlend;
	IDirect3DPixelShader9* lastPs;
};
static MockAoStats g_stats;
extern "C" __declspec(dllexport) MockAoStats* WINAPI MockGetStats() { return &g_stats; }

struct MockDevice : Gen_IDirect3DDevice9
{
	float vsConst[256 * 4] = {}, psConst[224 * 4] = {};
	IDirect3DPixelShader9* curPs = nullptr;
	IDirect3DSurface9* rt0 = nullptr;
	IDirect3DSurface9* rts[4] = {};
	IDirect3DBaseTexture9* tex0 = nullptr;
	DWORD blendOp = D3DBLENDOP_ADD, alphaBlend = FALSE;
	MockStateBlock* sb = nullptr;
	STDMETHOD(SetRenderTarget)(DWORD i, IDirect3DSurface9* s) override
	{
		if (i >= 4 || (i == 0 && !s))
			return D3DERR_INVALIDCALL;
		if (i == 0)
			rt0 = s;
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
	STDMETHOD(SetRenderState)(D3DRENDERSTATETYPE st, DWORD v) override
	{
		if (st == D3DRS_BLENDOP)
			blendOp = v;
		if (st == D3DRS_ALPHABLENDENABLE)
			alphaBlend = v;
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
		sb = new MockStateBlock;
		sb->livePs = &curPs;
		sb->savedPs = curPs;
		*out = sb;
		return S_OK;
	}
	STDMETHOD(CreateVertexDeclaration)(const D3DVERTEXELEMENT9*, IDirect3DVertexDeclaration9** out) override
	{
		*out = new MockDecl;
		return S_OK;
	}
	STDMETHOD(ColorFill)(IDirect3DSurface9*, const RECT*, D3DCOLOR) override
	{
		g_stats.colorFills++;
		return S_OK;
	}
	void NoteDraw()
	{
		g_stats.draws++;
		g_stats.lastRt0 = rt0;
		g_stats.lastBlendOp = blendOp;
		g_stats.lastAlphaBlend = alphaBlend;
		g_stats.lastPs = curPs;
		if (sb)
		{
			g_stats.stateCaptures = sb->captures;
			g_stats.stateApplies = sb->applies;
		}
	}
	STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) override
	{
		NoteDraw();
		return S_OK;
	}
	STDMETHOD(SetTexture)(DWORD stage, IDirect3DBaseTexture9* t) override
	{
		if (stage == 0)
			tex0 = t;
		return S_OK;
	}
	// Drawing an INTZ texture into a target marks it as a depth copy, which GetRenderTargetData then
	// fills with the fake depth minus an "object" (to test the dump's depth comparison).
	STDMETHOD(DrawPrimitiveUP)(D3DPRIMITIVETYPE, UINT, const void*, UINT) override
	{
		NoteDraw();
		auto t = static_cast<MockTexture*>(static_cast<IDirect3DTexture9*>(tex0));
		if (t && rt0 && t->count && t->levels[0]->desc.Format == kINTZ)
			static_cast<MockSurface*>(rt0)->depthCopy = true;
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
	MockSurface* bb;
	MockSurface* ds;
	int presents = 0;
	MockDevice(const D3DPRESENT_PARAMETERS* pp)
	{
		bb = NewSurface(pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, D3DUSAGE_RENDERTARGET, pp->MultiSampleType);
		ds = NewSurface(pp->BackBufferWidth, pp->BackBufferHeight, pp->AutoDepthStencilFormat, D3DUSAGE_DEPTHSTENCIL, pp->MultiSampleType);
		rt0 = rts[0] = bb;
	}
	STDMETHOD_(ULONG, AddRef)() override { return 1; }
	STDMETHOD_(ULONG, Release)() override { return 0; }
	STDMETHOD(GetBackBuffer)(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9** out) override
	{
		bb->AddRef();
		*out = bb;
		return S_OK;
	}
	STDMETHOD(GetDepthStencilSurface)(IDirect3DSurface9** out) override
	{
		ds->AddRef();
		*out = ds;
		return S_OK;
	}
	STDMETHOD(Reset)(D3DPRESENT_PARAMETERS* pp) override
	{
		bb = NewSurface(pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, D3DUSAGE_RENDERTARGET);
		ds = NewSurface(pp->BackBufferWidth, pp->BackBufferHeight, pp->AutoDepthStencilFormat, D3DUSAGE_DEPTHSTENCIL);
		return S_OK;
	}
	STDMETHOD(Present)(const RECT*, const RECT*, HWND, const RGNDATA*) override
	{
		presents++;
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
	STDMETHOD(CreateOffscreenPlainSurface)(UINT w, UINT h, D3DFORMAT f, D3DPOOL, IDirect3DSurface9** out, HANDLE*) override
	{
		*out = NewSurface(w, h, f, 0);
		return S_OK;
	}
	STDMETHOD(GetRenderTargetData)(IDirect3DSurface9* src, IDirect3DSurface9* dst) override
	{
		auto s = static_cast<MockSurface*>(src);
		auto d = static_cast<MockSurface*>(dst);
		// Fake "linear depth": cleared (1.0) top third, gradient below.
		s->Alloc();
		if (s->desc.Format == D3DFMT_R32F)
			for (UINT y = 0; y < s->desc.Height; y++)
				for (UINT x = 0; x < s->desc.Width; x++)
				{
					float v = y < s->desc.Height / 3 ? 1.0f : 0.5f * (1.0f - (float)y / s->desc.Height) + 0.001f * x / s->desc.Width;
					bool hole = x >= s->desc.Width / 4 && x < s->desc.Width / 2 && y >= s->desc.Height / 2 && y < s->desc.Height * 3 / 4;
					if (s->depthCopy && hole)
						v = 1.0f - (1.0f - v) * 0.5f; // the depth buffer sees twice as far here
					((float*)s->data.data())[y * s->desc.Width + x] = v;
				}
		d->data = s->data;
		return S_OK;
	}
	STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE st, DWORD* v) override
	{
		*v = st == D3DRS_COLORWRITEENABLE ? 0xF : 1;
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
};

extern "C" __declspec(dllexport) IDirect3D9* WINAPI Direct3DCreate9(UINT)
{
	return new MockD3D;
}
