// Fake d3d9.dll for testing AC2Shadows' frame dump on a machine without a display adapter.
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

struct MockDevice : Gen_IDirect3DDevice9
{
	MockSurface* bb;
	MockSurface* ds;
	int presents = 0;
	MockDevice(const D3DPRESENT_PARAMETERS* pp)
	{
		bb = NewSurface(pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, D3DUSAGE_RENDERTARGET, pp->MultiSampleType);
		ds = NewSurface(pp->BackBufferWidth, pp->BackBufferHeight, pp->AutoDepthStencilFormat, D3DUSAGE_DEPTHSTENCIL, pp->MultiSampleType);
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
					((float*)s->data.data())[y * s->desc.Width + x] = y < s->desc.Height / 3 ? 1.0f : 0.5f * (1.0f - (float)y / s->desc.Height) + 0.001f * x / s->desc.Width;
		d->data = s->data;
		return S_OK;
	}
	STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE st, DWORD* v) override
	{
		*v = st == D3DRS_COLORWRITEENABLE ? 0xF : 1;
		return S_OK;
	}
	IDirect3DPixelShader9* curPs = nullptr;
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
