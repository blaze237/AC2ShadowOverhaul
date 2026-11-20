// Fake d3d9.dll for the end-to-end tests on a machine without a Direct3D 9 adapter. Sits next to
// the test exe (AssassinsCreedIIGame.exe) exactly like a ReShade d3d9.dll proxy would.
//
// It keeps the device state the plugin reads and restores (constants, textures, sampler and render
// states, shaders, viewport, targets, depth, stream frequencies). State blocks snapshot everything
// but render targets and depth, like the real runtime's D3DSBT_ALL. It records what the tests check
// (see mock_api.h): AO counters, the SSGI view of the game's draws, and a log of every draw.
// Drawing an INTZ texture into a target marks it as a depth copy, which GetRenderTargetData then
// fills with a fake depth minus an "object" (for the frame dump's depth comparison).
#include "mock_gen.h"
#include "mock_api.h"
#include <stdio.h>
#include <string.h>
#include <vector>

static const GUID kIID_Tex = { 0x85c31227, 0x3de5, 0x4f00, { 0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5 } };
static const D3DFORMAT kINTZ = (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z');

static MockAoStats g_ao;
static MockGiStats g_gi;
static MockLog g_log;
extern "C" __declspec(dllexport) MockAoStats* WINAPI MockGetStats() { return &g_ao; }
extern "C" __declspec(dllexport) MockGiStats* WINAPI MockGetGiStats() { return &g_gi; }
extern "C" __declspec(dllexport) MockLog* WINAPI MockGetLog() { return &g_log; }

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

static std::vector<DWORD> CopyCode(const DWORD* code)
{
	std::vector<DWORD> v;
	for (size_t i = 0; code; i++)
	{
		v.push_back(code[i]);
		if (code[i] == 0x0000FFFF)
			break;
	}
	return v;
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
	State()
	{
		// The runtime's defaults for the render states the tests look at.
		rs[D3DRS_ZENABLE] = TRUE;
		rs[D3DRS_ZWRITEENABLE] = TRUE;
		rs[D3DRS_SRCBLEND] = D3DBLEND_ONE;
		rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
		rs[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
		rs[D3DRS_COLORWRITEENABLE] = 0xF;
	}
};

struct MockStateBlock : MockUnknown<IDirect3DStateBlock9>
{
	State* live = nullptr;
	State saved;
	int captures = 0, applies = 0;
	STDMETHOD(Capture)() override
	{
		captures++;
		saved = *live;
		return S_OK;
	}
	STDMETHOD(Apply)() override
	{
		applies++;
		*live = saved;
		return S_OK;
	}
};

// Sampler slot: 0-15 pixel samplers, 16 the displacement map sampler, 17-20 vertex samplers.
static int Slot(DWORD stage)
{
	if (stage < 16)
		return (int)stage;
	if (stage >= D3DDMAPSAMPLER && stage <= D3DVERTEXTEXTURESAMPLER3)
		return 16 + (int)(stage - D3DDMAPSAMPLER);
	return -1;
}

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
	State st;
	IDirect3DSurface9* rts[4] = {};
	IDirect3DSurface9* ds = nullptr;
	MockSurface* bb = nullptr;
	MockSurface* autoDs = nullptr;
	MockStateBlock* lastSb = nullptr;
	UINT freq[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };

	void CreateSwapChain(const D3DPRESENT_PARAMETERS* pp)
	{
		bb = NewSurface(pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, D3DUSAGE_RENDERTARGET, pp->MultiSampleType);
		autoDs = NewSurface(pp->BackBufferWidth, pp->BackBufferHeight, pp->AutoDepthStencilFormat, D3DUSAGE_DEPTHSTENCIL, pp->MultiSampleType);
		memset(rts, 0, sizeof(rts));
		rts[0] = bb;
		ds = autoDs;
		st = State();
		st.vp = { 0, 0, pp->BackBufferWidth, pp->BackBufferHeight, 0, 1 };
	}
	MockDevice(const D3DPRESENT_PARAMETERS* pp) { CreateSwapChain(pp); }
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

	// ---- Targets and depth -----------------------------------------------------------------------
	STDMETHOD(SetRenderTarget)(DWORD i, IDirect3DSurface9* s) override
	{
		if (i >= 4 || (i == 0 && !s))
			return D3DERR_INVALIDCALL;
		rts[i] = s;
		if (i == 0)
		{
			// Like the runtime: the viewport is reset to the whole new target.
			D3DSURFACE_DESC d;
			s->GetDesc(&d);
			st.vp = { 0, 0, d.Width, d.Height, 0, 1 };
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
	STDMETHOD(GetBackBuffer)(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9** out) override
	{
		bb->AddRef();
		*out = bb;
		return S_OK;
	}

	// ---- State -----------------------------------------------------------------------------------
	STDMETHOD(SetViewport)(const D3DVIEWPORT9* v) override
	{
		st.vp = *v;
		return S_OK;
	}
	STDMETHOD(GetViewport)(D3DVIEWPORT9* v) override
	{
		*v = st.vp;
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
	STDMETHOD(SetVertexShader)(IDirect3DVertexShader9* s) override
	{
		st.vs = s;
		return S_OK;
	}
	STDMETHOD(GetVertexShader)(IDirect3DVertexShader9** s) override
	{
		*s = st.vs;
		return S_OK;
	}
	STDMETHOD(SetPixelShader)(IDirect3DPixelShader9* s) override
	{
		st.ps = s;
		return S_OK;
	}
	STDMETHOD(GetPixelShader)(IDirect3DPixelShader9** s) override
	{
		*s = st.ps;
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
		sb->live = &st;
		sb->saved = st;
		lastSb = sb;
		*out = sb;
		return S_OK;
	}
	STDMETHOD(CreateVertexDeclaration)(const D3DVERTEXELEMENT9*, IDirect3DVertexDeclaration9** out) override
	{
		*out = new MockDecl;
		return S_OK;
	}

	// ---- Shaders ---------------------------------------------------------------------------------
	STDMETHOD(CreateVertexShader)(const DWORD* code, IDirect3DVertexShader9** out) override
	{
		auto vs = new MockVS;
		vs->code = CopyCode(code);
		g_log.vsCode.push_back(vs->code);
		*out = vs;
		return S_OK;
	}
	STDMETHOD(CreatePixelShader)(const DWORD* code, IDirect3DPixelShader9** out) override
	{
		auto ps = new MockPS;
		ps->code = CopyCode(code);
		g_gi.psCreated++;
		g_gi.psWithS13 += Declares(ps->code, 0xA00F080D);
		g_gi.psWithOC2 += WritesOC2(ps->code);
		*out = ps;
		return S_OK;
	}

	// ---- Draws -----------------------------------------------------------------------------------
	void NoteDraw(bool up)
	{
		g_ao.draws++;
		g_ao.lastRt0 = rts[0];
		g_ao.lastBlendOp = st.rs[D3DRS_BLENDOP];
		g_ao.lastAlphaBlend = st.rs[D3DRS_ALPHABLENDENABLE];
		g_ao.lastPs = st.ps;
		if (lastSb)
		{
			g_ao.stateCaptures = lastSb->captures;
			g_ao.stateApplies = lastSb->applies;
		}

		if (up)
		{
			g_gi.upDraws++;
			g_gi.upRt0 = rts[0];
		}
		else
		{
			g_gi.gameDraws++;
			g_gi.tex13 = st.tex[13];
			memcpy(g_gi.c216, st.psConst[216], sizeof(g_gi.c216));
			memcpy(g_gi.rt, rts, sizeof(rts));
			g_gi.freq1 = freq[1];
			g_gi.ps = st.ps;
		}

		MockDraw d = {};
		memcpy(d.rt, rts, sizeof(rts));
		d.ps = st.ps;
		d.psCode = st.ps ? static_cast<MockPS*>(st.ps)->code.data() : nullptr;
		d.vs = st.vs;
		d.vp = st.vp;
		d.alphaBlend = st.rs[D3DRS_ALPHABLENDENABLE];
		d.srcBlend = st.rs[D3DRS_SRCBLEND];
		d.destBlend = st.rs[D3DRS_DESTBLEND];
		d.blendOp = st.rs[D3DRS_BLENDOP];
		d.colorWrite = st.rs[D3DRS_COLORWRITEENABLE];
		memcpy(d.tex, st.tex, sizeof(d.tex));
		memcpy(d.psConst, st.psConst, sizeof(d.psConst));
		memcpy(d.c255, st.vsConst[255], sizeof(d.c255));
		d.lodBias0 = st.samp[0][D3DSAMP_MIPMAPLODBIAS];
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
	// Like the real runtime, nothing restores stream state here except explicit calls.
	STDMETHOD(DrawPrimitiveUP)(D3DPRIMITIVETYPE, UINT, const void*, UINT) override
	{
		for (UINT f : freq)
			if (f != 1)
			{
				g_ao.instancedUpDraws++;
				g_gi.instancedUpDraws++;
				break;
			}
		NoteDraw(true);
		auto t = static_cast<MockTexture*>(static_cast<IDirect3DTexture9*>(st.tex[0]));
		if (t && rts[0] && t->count && t->levels[0]->desc.Format == kINTZ)
			static_cast<MockSurface*>(rts[0])->depthCopy = true;
		return S_OK;
	}
	STDMETHOD(ColorFill)(IDirect3DSurface9*, const RECT*, D3DCOLOR) override
	{
		g_ao.colorFills++;
		return S_OK;
	}

	// ---- Resources and the swap chain --------------------------------------------------------------
	STDMETHOD(Reset)(D3DPRESENT_PARAMETERS* pp) override
	{
		g_log.resets++;
		CreateSwapChain(pp);
		return S_OK;
	}
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
};

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
	STDMETHOD(GetAdapterDisplayMode)(UINT, D3DDISPLAYMODE* m) override
	{
		memset(m, 0, sizeof(*m));
		m->Width = 1920;
		m->Height = 1080;
		m->Format = D3DFMT_X8R8G8B8;
		return S_OK;
	}
	STDMETHOD(CreateDevice)(UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) override
	{
		*out = new MockDevice(pp);
		return S_OK;
	}
};

extern "C" __declspec(dllexport) IDirect3D9* WINAPI Direct3DCreate9(UINT) { return new MockD3D; }
