// Frame dump: records how one frame is put together (render targets, depth buffers, clears,
// draw batches, which render-target textures are read where) and writes it to a text file
// when the hotkey is pressed. Used to work out which of AC2's depth buffers is the real scene
// depth and where in the frame effects such as AO should be injected.
//
// Hooks: the game's import of d3d9!Direct3DCreate9 -> IDirect3D9::CreateDevice -> the device
// vtable. If ReShade is installed as d3d9.dll these are ReShade's wrapper objects, so we see the
// calls exactly as the game makes them, and the return addresses point into the game exe.

#include "common.h"
#include "softshadows.h"
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <math.h>

// Vtable slots (checked against d3d9.h by tools/vtbl_check.c at build time).
enum
{
	D3D9_CreateDevice = 16,

	DEV_Reset = 16,
	DEV_Present = 17,
	DEV_CreateTexture = 23,
	DEV_CreateRenderTarget = 28,
	DEV_CreateDepthStencilSurface = 29,
	DEV_StretchRect = 34,
	DEV_ColorFill = 35,
	DEV_SetRenderTarget = 37,
	DEV_SetDepthStencilSurface = 39,
	DEV_BeginScene = 41,
	DEV_EndScene = 42,
	DEV_Clear = 43,
	DEV_SetViewport = 47,
	DEV_SetTexture = 65,
	DEV_DrawPrimitive = 81,
	DEV_DrawIndexedPrimitive = 82,
	DEV_DrawPrimitiveUP = 83,
	DEV_DrawIndexedPrimitiveUP = 84,
	DEV_CreatePixelShader = 106,
	DEV_SetPixelShader = 107,
};

static const GUID kIID_Texture9 = { 0x85c31227, 0x3de5, 0x4f00, { 0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5 } };

static int g_hotkey = VK_F8;
static int g_framesToCapture = 1;

static CRITICAL_SECTION g_lock;
struct Lock
{
	Lock() { EnterCriticalSection(&g_lock); }
	~Lock() { LeaveCriticalSection(&g_lock); }
};

// ---- Address formatting ------------------------------------------------------------------------

static uintptr_t g_exeBase, g_exeEnd, g_exePreferredBase;

static void InitExeRange()
{
	HMODULE exe = GetModuleHandleA(NULL);
	auto dos = (IMAGE_DOS_HEADER*)exe;
	auto nt = (IMAGE_NT_HEADERS*)((unsigned char*)exe + dos->e_lfanew);
	g_exeBase = (uintptr_t)exe;
	g_exeEnd = g_exeBase + nt->OptionalHeader.SizeOfImage;
	g_exePreferredBase = nt->OptionalHeader.ImageBase;
}

// Game addresses are printed at the exe's preferred base (0x400000) so they match a disassembler.
static std::string Where(void* addr)
{
	char buf[MAX_PATH + 32];
	uintptr_t a = (uintptr_t)addr;
	if (a >= g_exeBase && a < g_exeEnd)
	{
		snprintf(buf, sizeof(buf), "game:%08X", (unsigned)(a - g_exeBase + g_exePreferredBase));
		return buf;
	}
	HMODULE mod = NULL;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &mod) && mod)
	{
		char path[MAX_PATH];
		GetModuleFileNameA(mod, path, MAX_PATH);
		const char* name = strrchr(path, '\\');
		snprintf(buf, sizeof(buf), "%s+%X", name ? name + 1 : path, (unsigned)(a - (uintptr_t)mod));
		return buf;
	}
	snprintf(buf, sizeof(buf), "%08X", (unsigned)a);
	return buf;
}

#define CALLER Where(__builtin_return_address(0))

// ---- Formats -----------------------------------------------------------------------------------

static std::string FormatName(D3DFORMAT f)
{
	switch (f)
	{
#define F(x) case D3DFMT_##x: return #x;
		F(UNKNOWN) F(A8R8G8B8) F(X8R8G8B8) F(R5G6B5) F(X1R5G5B5) F(A1R5G5B5) F(A4R4G4B4) F(A8) F(A2B10G10R10)
		F(A8B8G8R8) F(X8B8G8R8) F(G16R16) F(A2R10G10B10) F(A16B16G16R16) F(L8) F(A8L8) F(L16) F(V8U8) F(Q8W8V8U8)
		F(V16U16) F(R16F) F(G16R16F) F(A16B16G16R16F) F(R32F) F(G32R32F) F(A32B32G32R32F) F(D16_LOCKABLE) F(D32)
		F(D15S1) F(D24S8) F(D24X8) F(D24X4S4) F(D16) F(D32F_LOCKABLE) F(D24FS8) F(DXT1) F(DXT2) F(DXT3) F(DXT4) F(DXT5)
#undef F
	default:
		break;
	}
	char buf[16];
	unsigned v = (unsigned)f;
	if (v > 0xFFFF) // FOURCC, e.g. INTZ / DF24 / NULL / RAWZ
		snprintf(buf, sizeof(buf), "%c%c%c%c", v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF);
	else
		snprintf(buf, sizeof(buf), "fmt%u", v);
	return buf;
}

// ---- Resource registry -------------------------------------------------------------------------
//
// Every render target / depth resource gets a short stable name:
//   T#   texture created with RENDERTARGET or DEPTHSTENCIL usage (T#.n = its mip level n surface)
//   R#   CreateRenderTarget surface      D#  CreateDepthStencilSurface surface
//   BB   swap chain back buffer          AUTODEPTH  device's automatic depth buffer
//   S#   any other surface seen during the dump

struct Res
{
	std::string name;
	UINT w = 0, h = 0;
	D3DFORMAT fmt = D3DFMT_UNKNOWN;    // as requested by the game
	D3DFORMAT actual = D3DFMT_UNKNOWN; // as created (differs if something upgrades formats)
	DWORD usage = 0;
	D3DMULTISAMPLE_TYPE ms = D3DMULTISAMPLE_NONE;
	std::string creator;
	bool isTexture = false; // can be bound with SetTexture
	bool transient = false; // resolved lazily during a dump; dropped at the next dump
};

static std::unordered_map<void*, Res> g_res;
static int g_nextId = 1;
static UINT g_bbW, g_bbH;

static std::string Describe(const Res& r)
{
	char buf[200];
	std::string fmt = FormatName(r.fmt);
	if (r.actual != D3DFMT_UNKNOWN && r.actual != r.fmt)
		fmt += " (created as " + FormatName(r.actual) + ")";
	snprintf(buf, sizeof(buf), "%s %ux%u %s%s%s%s", r.name.c_str(), r.w, r.h, fmt.c_str(),
		(r.usage & D3DUSAGE_RENDERTARGET) ? " RT" : "", (r.usage & D3DUSAGE_DEPTHSTENCIL) ? " DEPTH" : "",
		r.ms ? (" MSAAx" + std::to_string((int)r.ms)).c_str() : "");
	return buf;
}

static void Register(void* ptr, char prefix, UINT w, UINT h, D3DFORMAT fmt, D3DFORMAT actual, DWORD usage, D3DMULTISAMPLE_TYPE ms,
	bool isTexture, const std::string& creator)
{
	Res r;
	r.actual = actual;
	r.name = std::string(1, prefix) + std::to_string(g_nextId++);
	r.w = w;
	r.h = h;
	r.fmt = fmt;
	r.usage = usage;
	r.ms = ms;
	r.isTexture = isTexture;
	r.creator = creator;
	g_res[ptr] = r;
	LogInfo("Created %s (by %s)", Describe(r).c_str(), creator.c_str());
}

static void RegisterSwapChain(IDirect3DDevice9* dev, IDirect3DSurface9** bbOut, IDirect3DSurface9** dsOut)
{
	*bbOut = *dsOut = nullptr;
	for (auto it = g_res.begin(); it != g_res.end();)
		it = (it->second.name == "BB" || it->second.name == "AUTODEPTH") ? g_res.erase(it) : std::next(it);
	IDirect3DSurface9* s = nullptr;
	D3DSURFACE_DESC d;
	if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &s)) && s)
	{
		s->GetDesc(&d);
		Res r;
		r.name = "BB";
		g_bbW = d.Width;
		g_bbH = d.Height;
		r.w = d.Width; r.h = d.Height; r.fmt = d.Format; r.usage = d.Usage; r.ms = d.MultiSampleType;
		r.creator = "swap chain";
		g_res[s] = r;
		*bbOut = s;
		s->Release();
	}
	s = nullptr;
	if (SUCCEEDED(dev->GetDepthStencilSurface(&s)) && s)
	{
		s->GetDesc(&d);
		Res r;
		r.name = "AUTODEPTH";
		r.w = d.Width; r.h = d.Height; r.fmt = d.Format; r.usage = d.Usage; r.ms = d.MultiSampleType;
		r.creator = "swap chain";
		g_res[s] = r;
		*dsOut = s;
		s->Release();
	}
}

// Name of a surface bound as render target / depth. Surfaces of RT textures are resolved to
// their texture via GetContainer.
static std::string SurfName(IDirect3DSurface9* s, bool full)
{
	if (!s)
		return "null";
	auto it = g_res.find(s);
	if (it != g_res.end())
		return full ? Describe(it->second) : it->second.name;

	Res r;
	D3DSURFACE_DESC d;
	if (SUCCEEDED(s->GetDesc(&d)))
	{
		r.w = d.Width; r.h = d.Height; r.fmt = d.Format; r.usage = d.Usage; r.ms = d.MultiSampleType;
	}
	r.transient = true;
	IDirect3DTexture9* tex = nullptr;
	if (SUCCEEDED(s->GetContainer(kIID_Texture9, (void**)&tex)) && tex)
	{
		auto t = g_res.find(tex);
		int level = 0;
		for (DWORD i = 0; i < tex->GetLevelCount(); i++)
		{
			IDirect3DSurface9* lv = nullptr;
			if (SUCCEEDED(tex->GetSurfaceLevel(i, &lv)) && lv)
			{
				bool match = lv == s;
				lv->Release();
				if (match)
				{
					level = (int)i;
					break;
				}
			}
		}
		r.name = (t != g_res.end() ? t->second.name : "T?" + std::to_string(g_nextId++)) + "." + std::to_string(level);
		tex->Release();
	}
	else
	{
		r.name = "S" + std::to_string(g_nextId++);
	}
	g_res[s] = r;
	return full ? Describe(r) : r.name;
}

// ---- Shader hashes -----------------------------------------------------------------------------

static std::unordered_map<void*, uint32_t> g_psHash;
// Soft shadows: shader handed to the game (rewritten) -> the game's original shader, used when
// soft shadows are toggled off and for exporting the original bytecode.
struct SoftSet
{
	IDirect3DPixelShader9* original = nullptr; // owned
	IDirect3DPixelShader9* byWidth[5] = {};    // owned, except byWidth[returned] (the game's)
	int returned = 0;                          // width of the shader handed to the game
	void Release()
	{
		if (original)
			original->Release();
		for (int w = 1; w <= 4; w++)
			if (byWidth[w] && w != returned)
				byWidth[w]->Release();
	}
};
static std::unordered_map<void*, SoftSet> g_softOriginal;
static volatile bool g_softAny = false;
static LONG g_softRewritten = 0, g_softSkipped = 0, g_softFailed = 0;

static uint32_t HashShader(const DWORD* code)
{
	uint32_t h = 2166136261u;
	if (!code)
		return 0;
	for (size_t i = 0; i < (1u << 20); i++)
	{
		DWORD tok = code[i];
		for (int b = 0; b < 4; b++)
		{
			h ^= (tok >> (8 * b)) & 0xFF;
			h *= 16777619u;
		}
		if (tok == 0x0000FFFF) // D3DSIO_END
			break;
	}
	return h;
}

// ---- Tracked device state ----------------------------------------------------------------------

enum { kTexStages = 20 }; // 16 pixel samplers + displacement + 4 vertex samplers (mapped to 16..19)

static IDirect3DSurface9* g_rt[4];
static IDirect3DSurface9* g_ds;
static IDirect3DBaseTexture9* g_tex[kTexStages];
static void* g_ps;
static D3DVIEWPORT9 g_vp;

static void ResetTrackedState(IDirect3DSurface9* bb, IDirect3DSurface9* autoDepth)
{
	memset(g_rt, 0, sizeof(g_rt));
	memset(g_tex, 0, sizeof(g_tex));
	g_rt[0] = bb;
	g_ds = autoDepth;
	g_ps = nullptr;
	memset(&g_vp, 0, sizeof(g_vp));
}

// ---- Capture -----------------------------------------------------------------------------------

static volatile bool g_capturing = false;
static int g_framesLeft = 0;
static std::string g_out;
static int g_lines = 0;
static int g_event = 0;
static std::string g_deviceInfo;

struct Segment
{
	int draws = 0, prims = 0, zTest = 0, zWrite = 0, blend = 0, noColor = 0;
	std::vector<void*> ps;
	std::vector<std::pair<void*, int>> reads; // RT texture, stage
} g_seg;

// Pixel shaders that sample the sun shadow atlas this capture, with the sampler stage and draw count.
struct ShadowPs
{
	void* ps;
	int stage, draws;
};
static std::vector<ShadowPs> g_shadowPs;

// The sun shadow atlas: a render-target texture four cascades wide (e.g. 16384x4096 R32F).
static bool IsShadowAtlas(const Res& r)
{
	return r.isTexture && r.h >= 256 && r.w == 4 * r.h;
}

static void Out(const char* fmt, ...)
{
	if (g_lines > 100000)
		return;
	char buf[1024];
	va_list a;
	va_start(a, fmt);
	vsnprintf(buf, sizeof(buf), fmt, a);
	va_end(a);
	g_out += buf;
	g_out += '\n';
	if (++g_lines == 100000)
		g_out += "... (truncated)\n";
}

static std::string Bindings()
{
	std::string s = "RT0=" + SurfName(g_rt[0], false);
	for (int i = 1; i < 4; i++)
		if (g_rt[i])
			s += " RT" + std::to_string(i) + "=" + SurfName(g_rt[i], false);
	s += " DS=" + SurfName(g_ds, false);
	return s;
}

static void FlushSegment()
{
	if (!g_seg.draws)
		return;
	std::string ps;
	for (size_t i = 0; i < g_seg.ps.size() && i < 8; i++)
	{
		auto it = g_psHash.find(g_seg.ps[i]);
		char h[16];
		if (!g_seg.ps[i])
			snprintf(h, sizeof(h), "none");
		else if (it != g_psHash.end())
			snprintf(h, sizeof(h), "%08X", it->second);
		else
			snprintf(h, sizeof(h), "?");
		ps += (i ? " " : "") + std::string(h);
	}
	if (g_seg.ps.size() > 8)
		ps += " +" + std::to_string(g_seg.ps.size() - 8);
	std::string reads;
	for (auto& r : g_seg.reads)
	{
		auto it = g_res.find(r.first);
		reads += " " + (it != g_res.end() ? it->second.name : std::string("?")) + "@s" + std::to_string(r.second);
	}
	Out("      %4d draws %7d prims | ztest %d zwrite %d blend %d colour-off %d | [%s] | ps: %s%s%s",
		g_seg.draws, g_seg.prims, g_seg.zTest, g_seg.zWrite, g_seg.blend, g_seg.noColor, Bindings().c_str(), ps.c_str(),
		reads.empty() ? "" : " | reads:", reads.c_str());
	g_seg = Segment();
}

static void Event(const std::string& text, const std::string& caller)
{
	FlushSegment();
	Out("%5d %-90s <- %s", ++g_event, text.c_str(), caller.c_str());
}

static std::vector<void*> g_capTex; // RT textures sampled during the dump (alive this frame)

static void OnDraw(IDirect3DDevice9* dev, UINT prims)
{
	if (!g_capturing)
		return;
	// Query the real state (the game may set it through state blocks, which bypass SetRenderState).
	DWORD zEnable = 0, zWrite = 0, blend = 0, colorWrite = 0xF;
	dev->GetRenderState(D3DRS_ZENABLE, &zEnable);
	dev->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
	dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
	dev->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
	Lock l;
	g_seg.draws++;
	g_seg.prims += prims;
	if (zEnable && g_ds)
	{
		g_seg.zTest++;
		if (zWrite)
			g_seg.zWrite++;
	}
	if (blend)
		g_seg.blend++;
	if (!(colorWrite & 0xF) || !g_rt[0])
		g_seg.noColor++;
	if (std::find(g_seg.ps.begin(), g_seg.ps.end(), g_ps) == g_seg.ps.end())
		g_seg.ps.push_back(g_ps);
	for (int i = 0; i < kTexStages; i++)
	{
		if (!g_tex[i])
			continue;
		auto it = g_res.find(g_tex[i]);
		if (it == g_res.end() || !it->second.isTexture)
			continue;
		if (std::find(g_capTex.begin(), g_capTex.end(), g_tex[i]) == g_capTex.end())
			g_capTex.push_back(g_tex[i]);
		std::pair<void*, int> key(g_tex[i], i);
		if (std::find(g_seg.reads.begin(), g_seg.reads.end(), key) == g_seg.reads.end())
			g_seg.reads.push_back(key);
		if (g_ps && i < 16 && IsShadowAtlas(it->second))
		{
			auto sp = std::find_if(g_shadowPs.begin(), g_shadowPs.end(), [&](const ShadowPs& x) { return x.ps == g_ps && x.stage == i; });
			if (sp == g_shadowPs.end())
				g_shadowPs.push_back({ g_ps, i, 1 });
			else
				sp->draws++;
		}
	}
}

static void BeginCapture()
{
	Lock l;
	for (auto it = g_res.begin(); it != g_res.end();)
		it = it->second.transient ? g_res.erase(it) : std::next(it);
	g_out.clear();
	g_lines = 0;
	g_event = 0;
	g_seg = Segment();
	g_capTex.clear();
	g_shadowPs.clear();
	g_framesLeft = g_framesToCapture;

	SYSTEMTIME t;
	GetLocalTime(&t);
	Out("AC2Shadows frame dump %04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
	Out("%s", g_deviceInfo.c_str());
	Out("");
	Out("Render target / depth resources (name, size, format, created by):");
	std::vector<std::pair<int, const Res*>> list;
	for (auto& kv : g_res)
	{
		const Res& r = kv.second;
		int order = atoi(r.name.c_str() + 1);
		list.push_back({ r.name[0] == 'B' || r.name[0] == 'A' ? -1 : order, &r });
	}
	std::sort(list.begin(), list.end(), [](const std::pair<int, const Res*>& a, const std::pair<int, const Res*>& b) { return a.first < b.first; });
	for (auto& e : list)
		Out("  %-60s %s", Describe(*e.second).c_str(), e.second->creator.c_str());
	Out("");
	Out("Legend: event lines show state changes with the calling game address. Indented lines summarise the");
	Out("draws between two events: draw / primitive counts, how many had depth test / depth write / alpha blend");
	Out("enabled or colour writes off (depth-only), the bindings, pixel shader hashes and RT textures sampled.");
	Out("");
	Out("Start bindings: %s", Bindings().c_str());
	g_capturing = true;
}

// ---- Readback of single-channel float targets (to see what a "depth" colour target contains) ---

static uint32_t Crc32(const unsigned char* p, size_t n, uint32_t c = 0xFFFFFFFFu)
{
	static uint32_t table[256];
	if (!table[1])
		for (uint32_t i = 0; i < 256; i++)
		{
			uint32_t v = i;
			for (int k = 0; k < 8; k++)
				v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
			table[i] = v;
		}
	for (size_t i = 0; i < n; i++)
		c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
	return c;
}

static void Put32(std::string& s, uint32_t v)
{
	s += (char)(v >> 24);
	s += (char)(v >> 16);
	s += (char)(v >> 8);
	s += (char)v;
}

static void PngChunk(std::string& out, const char* type, const std::string& data)
{
	Put32(out, (uint32_t)data.size());
	std::string body = std::string(type, 4) + data;
	out += body;
	Put32(out, Crc32((const unsigned char*)body.data(), body.size()) ^ 0xFFFFFFFFu);
}

// 8-bit greyscale PNG, uncompressed (stored deflate blocks).
static bool WriteGreyPng(const char* path, const std::vector<unsigned char>& px, unsigned w, unsigned h)
{
	std::string raw;
	raw.reserve((size_t)(w + 1) * h);
	for (unsigned y = 0; y < h; y++)
	{
		raw += '\0';
		raw.append((const char*)&px[(size_t)y * w], w);
	}
	std::string z("\x78\x01", 2);
	size_t pos = 0;
	do
	{
		size_t len = std::min<size_t>(65535, raw.size() - pos);
		bool last = pos + len >= raw.size();
		z += (char)(last ? 1 : 0);
		z += (char)(len & 0xFF);
		z += (char)(len >> 8);
		z += (char)(~len & 0xFF);
		z += (char)((~len >> 8) & 0xFF);
		z.append(raw, pos, len);
		pos += len;
	} while (pos < raw.size());
	uint32_t a = 1, b = 0;
	for (unsigned char c : raw)
	{
		a = (a + c) % 65521;
		b = (b + a) % 65521;
	}
	Put32(z, (b << 16) | a);

	std::string png("\x89PNG\r\n\x1a\n", 8);
	std::string ihdr;
	Put32(ihdr, w);
	Put32(ihdr, h);
	ihdr += std::string("\x08\x00\x00\x00\x00", 5); // 8-bit greyscale
	PngChunk(png, "IHDR", ihdr);
	PngChunk(png, "IDAT", z);
	PngChunk(png, "IEND", "");
	FILE* f = fopen(path, "wb");
	if (!f)
		return false;
	fwrite(png.data(), 1, png.size(), f);
	fclose(f);
	return true;
}

static float HalfToFloat(uint16_t h)
{
	uint32_t sign = (uint32_t)(h >> 15) << 31, exp = (h >> 10) & 0x1F, man = h & 0x3FF, bits;
	if (exp == 0)
	{
		float v = man / 1024.0f / 16384.0f;
		return sign ? -v : v;
	}
	if (exp == 31)
		bits = sign | 0x7F800000u | (man << 13);
	else
		bits = sign | ((exp + 112) << 23) | (man << 13);
	float f;
	memcpy(&f, &bits, 4);
	return f;
}

static std::string ReadbackFloatTarget(IDirect3DDevice9* dev, IDirect3DTexture9* tex, const D3DSURFACE_DESC& d, const char* pngPath)
{
	IDirect3DSurface9 *src = nullptr, *sys = nullptr;
	std::string result;
	if (FAILED(tex->GetSurfaceLevel(0, &src)) || !src)
		return "GetSurfaceLevel failed";
	if (FAILED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL)) || !sys)
	{
		src->Release();
		return "CreateOffscreenPlainSurface failed";
	}
	D3DLOCKED_RECT lr;
	if (FAILED(dev->GetRenderTargetData(src, sys)) || FAILED(sys->LockRect(&lr, NULL, D3DLOCK_READONLY)))
	{
		result = "GetRenderTargetData/LockRect failed";
	}
	else
	{
		std::vector<float> v((size_t)d.Width * d.Height);
		for (UINT y = 0; y < d.Height; y++)
		{
			const unsigned char* row = (const unsigned char*)lr.pBits + (size_t)y * lr.Pitch;
			for (UINT x = 0; x < d.Width; x++)
				v[(size_t)y * d.Width + x] = d.Format == D3DFMT_R32F ? ((const float*)row)[x] : HalfToFloat(((const uint16_t*)row)[x]);
		}
		sys->UnlockRect();

		const float clearValue = 1.0f; // the game clears its float targets to white
		std::vector<float> geo;
		geo.reserve(v.size() / 4);
		size_t cleared = 0, above1 = 0, negative = 0, samples = 0;
		for (size_t i = 0; i < v.size(); i += 4) // every 4th pixel is plenty for statistics
		{
			samples++;
			if (v[i] == clearValue)
				cleared++;
			else
				geo.push_back(v[i]);
			if (v[i] > 1.0f)
				above1++;
			if (v[i] < 0.0f)
				negative++;
		}
		std::sort(geo.begin(), geo.end());
		auto pct = [&](double p) { return geo.empty() ? 0.0f : geo[(size_t)(p * (geo.size() - 1))]; };
		float lo = pct(0.01), hi = pct(0.99);
		auto at = [&](double fx, double fy) { return v[(size_t)(fy * (d.Height - 1)) * d.Width + (size_t)(fx * (d.Width - 1))]; };
		char buf[700];
		snprintf(buf, sizeof(buf),
			"%s %ux%u | cleared (==1.0) %.1f%% | >1.0 %.1f%% | <0 %.1f%% | geometry min %g p1 %g p25 %g p50 %g p75 %g p99 %g max %g | "
			"centre %g, lower-centre %g, left %g, right %g",
			FormatName(d.Format).c_str(), d.Width, d.Height, 100.0 * cleared / samples, 100.0 * above1 / samples,
			100.0 * negative / samples, geo.empty() ? 0.0f : geo.front(), lo, pct(0.25), pct(0.5), pct(0.75), hi,
			geo.empty() ? 0.0f : geo.back(), at(0.5, 0.5), at(0.5, 0.75), at(0.25, 0.5), at(0.75, 0.5));
		result = buf;

		// Image: geometry stretched from p1 (black) to p99 (light grey), cleared pixels white.
		unsigned step = (d.Width + 1279) / 1280;
		unsigned ow = d.Width / step, oh = d.Height / step;
		std::vector<unsigned char> px((size_t)ow * oh);
		float range = hi > lo ? hi - lo : 1.0f;
		for (unsigned y = 0; y < oh; y++)
			for (unsigned x = 0; x < ow; x++)
			{
				float f = v[(size_t)(y * step) * d.Width + x * step];
				float n = (f - lo) / range;
				n = n < 0 ? 0 : (n > 1 ? 1 : n);
				px[(size_t)y * ow + x] = f == clearValue ? 255 : (unsigned char)(sqrtf(n) * 230.0f);
			}
		if (WriteGreyPng(pngPath, px, ow, oh))
		{
			const char* name = strrchr(pngPath, '\\');
			result += std::string(" | image ") + (name ? name + 1 : pngPath);
		}
	}
	sys->Release();
	src->Release();
	return result;
}

// ---- Shadow shader export ----------------------------------------------------------------------
// Writes every pixel shader that sampled the shadow atlas: raw bytecode as <hash>.cso (for offline
// work) and a D3DX disassembly into one text file. D3DX comes from the d3dx9_42.dll the game
// already loads; only the one function and ID3DXBuffer's vtable are declared here.

struct D3DXBufferVtbl
{
	void* qi;
	void* addRef;
	ULONG(__stdcall* Release)(void*);
	void*(__stdcall* GetBufferPointer)(void*);
	DWORD(__stdcall* GetBufferSize)(void*);
};
struct D3DXBuffer
{
	D3DXBufferVtbl* vt;
};
typedef HRESULT(WINAPI* PFN_D3DXDisassembleShader)(const DWORD*, BOOL, LPCSTR, D3DXBuffer**);

typedef HRESULT(WINAPI* PFN_D3DDisassemble)(const void*, SIZE_T, UINT, LPCSTR, D3DXBuffer**); // ID3DBlob = same vtable

static std::string DisassembleShader(const DWORD* code, UINT size)
{
	// The game's d3dx9_42.dll first; D3DCompiler (also handles SM3 bytecode) as a fallback.
	static PFN_D3DXDisassembleShader d3dx = nullptr;
	static PFN_D3DDisassemble compiler = nullptr;
	static bool looked = false;
	if (!looked)
	{
		looked = true;
		for (const char* d : { "d3dx9_42.dll", "d3dx9_43.dll" })
		{
			HMODULE m = GetModuleHandleA(d);
			if (!m)
				m = LoadLibraryA(d);
			if (m && (d3dx = (PFN_D3DXDisassembleShader)GetProcAddress(m, "D3DXDisassembleShader")) != nullptr)
				break;
		}
		if (!d3dx)
			for (const char* d : { "D3DCompiler_43.dll", "D3DCompiler_47.dll" })
			{
				HMODULE m = LoadLibraryA(d);
				if (m && (compiler = (PFN_D3DDisassemble)GetProcAddress(m, "D3DDisassemble")) != nullptr)
					break;
			}
	}
	D3DXBuffer* buf = nullptr;
	HRESULT hr = E_FAIL;
	if (d3dx)
		hr = d3dx(code, FALSE, nullptr, &buf);
	else if (compiler)
		hr = compiler(code, size, 0, nullptr, &buf);
	else
		return "(no shader disassembler available)\n";
	if (FAILED(hr) || !buf)
		return "(disassembly failed)\n";
	std::string text((const char*)buf->vt->GetBufferPointer(buf), buf->vt->GetBufferSize(buf));
	buf->vt->Release(buf);
	while (!text.empty() && text.back() == '\0')
		text.pop_back();
	return text;
}

// True if SM2/SM3 bytecode declares sampler register `stage` (a "dcl_* sN" instruction). The game
// leaves the atlas bound for the whole frame, so being bound doesn't mean a shader samples it.
static bool ShaderDeclaresSampler(const DWORD* code, size_t count, int stage)
{
	if (count < 2 || (code[0] & 0xFFFF0000) != 0xFFFF0000 || ((code[0] >> 8) & 0xFF) < 2)
		return false;
	for (size_t i = 1; i < count;)
	{
		DWORD tok = code[i];
		if (tok == 0x0000FFFF) // end
			break;
		if ((tok & 0xFFFF) == 0xFFFE) // comment: length in bits 16..30
		{
			i += 1 + ((tok >> 16) & 0x7FFF);
			continue;
		}
		size_t len = (tok >> 24) & 0xF; // SM2+: instruction length in tokens
		if ((tok & 0xFFFF) == 0x1F && len >= 2 && i + 2 < count) // dcl usage, dest
		{
			DWORD dst = code[i + 2];
			int type = (int)(((dst >> 28) & 7) | ((dst >> 8) & 0x18));
			if (type == 10 /* D3DSPR_SAMPLER */ && (int)(dst & 0x7FF) == stage)
				return true;
		}
		i += 1 + len;
	}
	return false;
}

static void ExportShadowShaders(const char* base)
{
	std::vector<ShadowPs> list;
	{
		Lock l;
		list = g_shadowPs;
	}
	std::sort(list.begin(), list.end(), [](const ShadowPs& a, const ShadowPs& b) { return a.draws > b.draws; });

	struct Found
	{
		ShadowPs sp;
		std::vector<DWORD> code;
		UINT size;
		uint32_t hash;
	};
	std::vector<Found> found;
	for (const ShadowPs& sp : list)
	{
		auto ps = (IDirect3DPixelShader9*)sp.ps;
		{
			Lock l;
			auto it = g_softOriginal.find(sp.ps);
			if (it != g_softOriginal.end() && it->second.original)
				ps = it->second.original; // export the game's original, not our rewrite
		}
		UINT size = 0;
		if (FAILED(ps->GetFunction(nullptr, &size)) || !size || size > (1u << 20))
			continue;
		std::vector<DWORD> code((size + 3) / 4);
		if (FAILED(ps->GetFunction(code.data(), &size)) || !ShaderDeclaresSampler(code.data(), code.size(), sp.stage))
			continue;
		uint32_t h = HashShader(code.data());
		found.push_back({ sp, std::move(code), size, h });
	}
	Out("");
	Out("Pixel shaders sampling the shadow atlas: %d of %d drawn with it bound (disassembly in %s_shaders.txt)", (int)found.size(),
		(int)list.size(), strrchr(base, '\\') ? strrchr(base, '\\') + 1 : base);
	if (found.empty())
		return;

	char dir[MAX_PATH];
	snprintf(dir, sizeof(dir), "%s_shaders", base);
	CreateDirectoryA(dir, nullptr);
	std::string text;
	int written = 0;
	for (Found& fd : found)
	{
		const ShadowPs& sp = fd.sp;
		std::vector<DWORD>& code = fd.code;
		UINT size = fd.size;
		uint32_t h = fd.hash;
		Out("  PS %08X  atlas at s%d  %d draws  %u bytes", h, sp.stage, sp.draws, size);

		char path[MAX_PATH];
		snprintf(path, sizeof(path), "%s\\%08X.cso", dir, h);
		if (FILE* f = fopen(path, "wb"))
		{
			fwrite(code.data(), 1, size, f);
			fclose(f);
		}
		char head[160];
		snprintf(head, sizeof(head), "==== PS %08X | shadow atlas at s%d | %d draws | %u bytes ====\n", h, sp.stage, sp.draws, size);
		text += head;
		text += DisassembleShader(code.data(), size);
		text += "\n\n";
		written++;
	}
	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%s_shaders.txt", base);
	if (FILE* f = fopen(path, "w"))
	{
		fwrite(text.data(), 1, text.size(), f);
		fclose(f);
	}
	LogInfo("Exported %d shadow-sampling pixel shaders to %s_shaders.txt / %s.", written, base, dir);
}

static void EndCapture(IDirect3DDevice9* dev)
{
	SYSTEMTIME t;
	GetLocalTime(&t);
	char base[MAX_PATH];
	snprintf(base, sizeof(base), "%sAC2Shadows_FrameDump_%04d%02d%02d_%02d%02d%02d", ModuleDir(), t.wYear, t.wMonth, t.wDay,
		t.wHour, t.wMinute, t.wSecond);

	std::string text;
	{
		Lock l;
		FlushSegment();
		g_capturing = false;
		Out("");
		Out("Screen-sized float render targets sampled this frame (read back at Present):");
		int n = 0;
		for (void* tex : g_capTex)
		{
			auto it = g_res.find(tex);
			if (it == g_res.end())
				continue;
			D3DSURFACE_DESC d = {};
			auto t9 = (IDirect3DTexture9*)tex;
			if (FAILED(t9->GetLevelDesc(0, &d)) || d.Width != g_bbW || d.Height != g_bbH)
				continue;
			if (d.Format != D3DFMT_R32F && d.Format != D3DFMT_R16F)
				continue;
			char png[MAX_PATH];
			snprintf(png, sizeof(png), "%s_%s.png", base, it->second.name.c_str());
			Out("  %s: %s", it->second.name.c_str(), ReadbackFloatTarget(dev, t9, d, png).c_str());
			n++;
		}
		if (!n)
			Out("  (none)");
		ExportShadowShaders(base);
		text.swap(g_out);
	}
	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%s.txt", base);
	FILE* f = fopen(path, "w");
	if (f)
	{
		fwrite(text.data(), 1, text.size(), f);
		fclose(f);
		LogInfo("Frame dump written to %s (%d lines).", path, g_lines);
		MessageBeep(MB_OK);
	}
	else
	{
		LogWarn("Could not write frame dump to %s.", path);
	}
}

// ---- Device hooks ------------------------------------------------------------------------------

#define ORIG(name, ret, ...) typedef ret(__stdcall* PFN_##name)(IDirect3DDevice9*, ##__VA_ARGS__); static PFN_##name o_##name;

ORIG(Reset, HRESULT, D3DPRESENT_PARAMETERS*)
ORIG(Present, HRESULT, const RECT*, const RECT*, HWND, const RGNDATA*)
ORIG(CreateTexture, HRESULT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*)
ORIG(CreateRenderTarget, HRESULT, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*)
ORIG(CreateDepthStencilSurface, HRESULT, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*)
ORIG(StretchRect, HRESULT, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE)
ORIG(ColorFill, HRESULT, IDirect3DSurface9*, const RECT*, D3DCOLOR)
ORIG(SetRenderTarget, HRESULT, DWORD, IDirect3DSurface9*)
ORIG(SetDepthStencilSurface, HRESULT, IDirect3DSurface9*)
ORIG(BeginScene, HRESULT)
ORIG(EndScene, HRESULT)
ORIG(Clear, HRESULT, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD)
ORIG(SetViewport, HRESULT, const D3DVIEWPORT9*)
ORIG(SetTexture, HRESULT, DWORD, IDirect3DBaseTexture9*)
ORIG(DrawPrimitive, HRESULT, D3DPRIMITIVETYPE, UINT, UINT)
ORIG(DrawIndexedPrimitive, HRESULT, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT)
ORIG(DrawPrimitiveUP, HRESULT, D3DPRIMITIVETYPE, UINT, const void*, UINT)
ORIG(DrawIndexedPrimitiveUP, HRESULT, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT)
ORIG(CreatePixelShader, HRESULT, const DWORD*, IDirect3DPixelShader9**)
ORIG(SetPixelShader, HRESULT, IDirect3DPixelShader9*)

static std::string DescribePresent(const D3DPRESENT_PARAMETERS* pp)
{
	char buf[512];
	snprintf(buf, sizeof(buf),
		"back buffer %ux%u %s x%u, MSAA type %d quality %lu, windowed %d, auto depth %d (%s), swap effect %d, flags 0x%lX, interval 0x%X",
		pp->BackBufferWidth, pp->BackBufferHeight, FormatName(pp->BackBufferFormat).c_str(), pp->BackBufferCount,
		(int)pp->MultiSampleType, pp->MultiSampleQuality, pp->Windowed, pp->EnableAutoDepthStencil,
		FormatName(pp->AutoDepthStencilFormat).c_str(), (int)pp->SwapEffect, pp->Flags, pp->PresentationInterval);
	return buf;
}

static void AfterDeviceReady(IDirect3DDevice9* dev)
{
	Lock l;
	IDirect3DSurface9 *bb, *ds;
	RegisterSwapChain(dev, &bb, &ds);
	ResetTrackedState(bb, ds);
}

static HRESULT __stdcall h_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
	LogInfo("Device Reset: %s", DescribePresent(pp).c_str());
	Tuning_OnLostDevice();
	HRESULT hr = o_Reset(dev, pp);
	if (SUCCEEDED(hr))
	{
		AfterDeviceReady(dev);
		Tuning_OnResetDevice();
	}
	else
		LogWarn("Device Reset failed: 0x%08lX", hr);
	return hr;
}

static HRESULT __stdcall h_Present(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND w, const RGNDATA* r)
{
	static bool first = true;
	if (first)
	{
		first = false;
		LogInfo("First Present seen; frame dump hotkey is 0x%02X.", g_hotkey);
	}
	if (g_capturing)
	{
		bool done;
		{
			Lock l;
			Event("Present", CALLER);
			done = --g_framesLeft <= 0;
		}
		if (done)
			EndCapture(dev);
	}

	if (!g_capturing && Tuning_Enabled())
		Tuning_OnPresent(dev, g_bbW, g_bbH);
	HRESULT hr = o_Present(dev, a, b, w, r);

	static bool wasDown = false;
	bool down = g_hotkey && (GetAsyncKeyState(g_hotkey) & 0x8000) != 0;
	if (down && !wasDown && !g_capturing && GameHasFocus())
		BeginCapture();
	wasDown = down;
	return hr;
}

static HRESULT __stdcall h_CreateTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
	IDirect3DTexture9** out, HANDLE* shared)
{
	HRESULT hr = o_CreateTexture(dev, w, h, levels, usage, fmt, pool, out, shared);
	if (SUCCEEDED(hr) && out && *out && (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)))
	{
		Lock l;
		D3DSURFACE_DESC d = {};
		(*out)->GetLevelDesc(0, &d);
		Register(*out, 'T', w, h, fmt, d.Format, usage, D3DMULTISAMPLE_NONE, true, CALLER);
	}
	return hr;
}

static HRESULT __stdcall h_CreateRenderTarget(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD q,
	BOOL lockable, IDirect3DSurface9** out, HANDLE* shared)
{
	HRESULT hr = o_CreateRenderTarget(dev, w, h, fmt, ms, q, lockable, out, shared);
	if (SUCCEEDED(hr) && out && *out)
	{
		Lock l;
		D3DSURFACE_DESC d = {};
		(*out)->GetDesc(&d);
		Register(*out, 'R', w, h, fmt, d.Format, D3DUSAGE_RENDERTARGET, ms, false, CALLER);
	}
	return hr;
}

static HRESULT __stdcall h_CreateDepthStencilSurface(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms,
	DWORD q, BOOL discard, IDirect3DSurface9** out, HANDLE* shared)
{
	HRESULT hr = o_CreateDepthStencilSurface(dev, w, h, fmt, ms, q, discard, out, shared);
	if (SUCCEEDED(hr) && out && *out)
	{
		Lock l;
		D3DSURFACE_DESC d = {};
		(*out)->GetDesc(&d);
		Register(*out, 'D', w, h, fmt, d.Format, D3DUSAGE_DEPTHSTENCIL, ms, false, CALLER);
	}
	return hr;
}

static HRESULT __stdcall h_StretchRect(IDirect3DDevice9* dev, IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst,
	const RECT* dr, D3DTEXTUREFILTERTYPE filter)
{
	if (g_capturing)
	{
		Lock l;
		Event("StretchRect " + SurfName(src, true) + " -> " + SurfName(dst, true), CALLER);
	}
	return o_StretchRect(dev, src, sr, dst, dr, filter);
}

static HRESULT __stdcall h_ColorFill(IDirect3DDevice9* dev, IDirect3DSurface9* s, const RECT* r, D3DCOLOR c)
{
	if (g_capturing)
	{
		Lock l;
		char buf[32];
		snprintf(buf, sizeof(buf), " colour %08lX", c);
		Event("ColorFill " + SurfName(s, true) + buf, CALLER);
	}
	return o_ColorFill(dev, s, r, c);
}

static HRESULT __stdcall h_SetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s)
{
	HRESULT hr = o_SetRenderTarget(dev, index, s);
	if (SUCCEEDED(hr) && index < 4)
	{
		bool changed = g_rt[index] != s;
		if (g_capturing && changed)
		{
			Lock l;
			Event("SetRenderTarget(" + std::to_string(index) + ", " + SurfName(s, true) + ")", CALLER);
		}
		g_rt[index] = s;
		if (index == 0 && s)
		{
			// D3D9 resets the viewport to the full target when RT0 changes.
			D3DSURFACE_DESC d;
			if (SUCCEEDED(s->GetDesc(&d)))
			{
				g_vp.X = g_vp.Y = 0;
				g_vp.Width = d.Width;
				g_vp.Height = d.Height;
			}
		}
	}
	return hr;
}

static HRESULT __stdcall h_SetDepthStencilSurface(IDirect3DDevice9* dev, IDirect3DSurface9* s)
{
	HRESULT hr = o_SetDepthStencilSurface(dev, s);
	if (SUCCEEDED(hr))
	{
		if (g_capturing && g_ds != s)
		{
			Lock l;
			Event("SetDepthStencilSurface(" + SurfName(s, true) + ")", CALLER);
		}
		g_ds = s;
	}
	return hr;
}

static HRESULT __stdcall h_BeginScene(IDirect3DDevice9* dev)
{
	if (g_capturing)
	{
		Lock l;
		Event("BeginScene", CALLER);
	}
	return o_BeginScene(dev);
}

static HRESULT __stdcall h_EndScene(IDirect3DDevice9* dev)
{
	if (g_capturing)
	{
		Lock l;
		Event("EndScene", CALLER);
	}
	return o_EndScene(dev);
}

static HRESULT __stdcall h_Clear(IDirect3DDevice9* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR c, float z, DWORD stencil)
{
	if (g_capturing)
	{
		Lock l;
		char buf[160];
		snprintf(buf, sizeof(buf), "Clear%s%s%s colour %08lX z %g stencil %lu rects %lu | ", (flags & D3DCLEAR_TARGET) ? " COLOUR" : "",
			(flags & D3DCLEAR_ZBUFFER) ? " DEPTH" : "", (flags & D3DCLEAR_STENCIL) ? " STENCIL" : "", c, z, stencil, count);
		Event(buf + Bindings(), CALLER);
	}
	return o_Clear(dev, count, rects, flags, c, z, stencil);
}

static HRESULT __stdcall h_SetViewport(IDirect3DDevice9* dev, const D3DVIEWPORT9* vp)
{
	HRESULT hr = o_SetViewport(dev, vp);
	if (SUCCEEDED(hr) && vp)
	{
		if (g_capturing && memcmp(vp, &g_vp, sizeof(*vp)) != 0)
		{
			Lock l;
			char buf[128];
			snprintf(buf, sizeof(buf), "SetViewport %lu,%lu %lux%lu z %g..%g", vp->X, vp->Y, vp->Width, vp->Height, vp->MinZ, vp->MaxZ);
			Event(buf, CALLER);
		}
		g_vp = *vp;
	}
	return hr;
}

static HRESULT __stdcall h_SetTexture(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* tex)
{
	int slot = -1;
	if (stage < 16)
		slot = (int)stage;
	else if (stage >= D3DVERTEXTEXTURESAMPLER0 && stage <= D3DVERTEXTEXTURESAMPLER3)
		slot = 16 + (int)(stage - D3DVERTEXTEXTURESAMPLER0);
	if (slot >= 0)
		g_tex[slot] = tex;
	return o_SetTexture(dev, stage, tex);
}

static HRESULT __stdcall h_DrawPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT start, UINT count)
{
	OnDraw(dev, count);
	return o_DrawPrimitive(dev, t, start, count);
}

static HRESULT __stdcall h_DrawIndexedPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, INT base, UINT minIdx, UINT numV, UINT start, UINT count)
{
	OnDraw(dev, count);
	return o_DrawIndexedPrimitive(dev, t, base, minIdx, numV, start, count);
}

static HRESULT __stdcall h_DrawPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT count, const void* data, UINT stride)
{
	OnDraw(dev, count);
	return o_DrawPrimitiveUP(dev, t, count, data, stride);
}

static HRESULT __stdcall h_DrawIndexedPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT minIdx, UINT numV, UINT count,
	const void* idx, D3DFORMAT idxFmt, const void* data, UINT stride)
{
	OnDraw(dev, count);
	return o_DrawIndexedPrimitiveUP(dev, t, minIdx, numV, count, idx, idxFmt, data, stride);
}

static HRESULT __stdcall h_CreatePixelShader(IDirect3DDevice9* dev, const DWORD* code, IDirect3DPixelShader9** out)
{
	SoftSet set;
	bool haveSet = false;
	HRESULT hr = E_FAIL;
	if (code && out)
	{
		const int width = SoftShadows_Width();
		std::vector<DWORD> soft;
		std::string reason;
		if (SoftShadows_Rewrite(code, width, soft, reason))
		{
			hr = o_CreatePixelShader(dev, soft.data(), out);
			if (FAILED(hr))
			{
				LONG n = InterlockedIncrement(&g_softFailed);
				if (n <= 10)
					LogWarn("Soft shadows: the driver rejected rewritten shader %08X (0x%08lX); using the original.", HashShader(code), hr);
			}
			else
			{
				haveSet = true;
				set.returned = width;
				set.byWidth[width] = *out;
				if (FAILED(o_CreatePixelShader(dev, code, &set.original)))
					set.original = nullptr;
				// Debug menu: every width ready to switch to.
				for (int w = 1; w <= 4 && SoftShadows_BuildAllWidths(); w++)
				{
					if (w == width)
						continue;
					std::vector<DWORD> v;
					std::string why;
					if (SoftShadows_Rewrite(code, w, v, why) && FAILED(o_CreatePixelShader(dev, v.data(), &set.byWidth[w])))
						set.byWidth[w] = nullptr;
				}
				LONG n = InterlockedIncrement(&g_softRewritten);
				if (n == 1 || n % 50 == 0)
					LogInfo("Soft shadows: %ld shadow shaders rewritten so far.", n);
			}
		}
		else if (reason != "no shadow sampler" && reason != "not ps_3_0")
		{
			LONG n = InterlockedIncrement(&g_softSkipped);
			if (n <= 20)
				LogInfo("Soft shadows: shader %08X left unchanged (%s).", HashShader(code), reason.c_str());
		}
	}
	if (FAILED(hr))
		hr = o_CreatePixelShader(dev, code, out);
	if (SUCCEEDED(hr) && out && *out)
	{
		uint32_t h = HashShader(code);
		Lock l;
		g_psHash[*out] = h;
		// The address may belong to a shader the game released earlier: drop its stale set.
		auto it = g_softOriginal.find(*out);
		if (it != g_softOriginal.end())
		{
			it->second.Release();
			g_softOriginal.erase(it);
		}
		if (haveSet)
		{
			g_softOriginal[*out] = set;
			g_softAny = true;
		}
	}
	return hr;
}

static HRESULT __stdcall h_SetPixelShader(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps)
{
	g_ps = ps;
	if (ps && g_softAny)
	{
		bool active = SoftShadows_Active();
		int width = SoftShadows_LiveWidth();
		if (!active || width != SoftShadows_Width())
		{
			Lock l;
			auto it = g_softOriginal.find(ps);
			if (it != g_softOriginal.end())
			{
				IDirect3DPixelShader9* sub = active ? it->second.byWidth[width] : it->second.original;
				if (sub)
					ps = sub;
			}
		}
	}
	return o_SetPixelShader(dev, ps);
}

template <typename T> static void HookSlot(void** vtbl, int index, T hook, T& orig)
{
	if (vtbl[index] == (void*)hook)
		return; // already hooked (device re-created on the same class)
	orig = (T)vtbl[index];
	void* p = (void*)hook;
	WriteMemory((uintptr_t)&vtbl[index], &p, sizeof(p));
}

static void HookDevice(IDirect3DDevice9* dev)
{
	void** vt = *(void***)dev;
#define H(name) HookSlot(vt, DEV_##name, h_##name, o_##name)
	H(Reset);
	H(Present);
	H(CreateTexture);
	H(CreateRenderTarget);
	H(CreateDepthStencilSurface);
	H(StretchRect);
	H(ColorFill);
	H(SetRenderTarget);
	H(SetDepthStencilSurface);
	H(BeginScene);
	H(EndScene);
	H(Clear);
	H(SetViewport);
	H(SetTexture);
	H(DrawPrimitive);
	H(DrawIndexedPrimitive);
	H(DrawPrimitiveUP);
	H(DrawIndexedPrimitiveUP);
	H(CreatePixelShader);
	H(SetPixelShader);
#undef H
}

// ---- IDirect3D9 hooks --------------------------------------------------------------------------

typedef HRESULT(__stdcall* PFN_CreateDevice)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
static PFN_CreateDevice o_CreateDevice;
typedef IDirect3D9*(WINAPI* PFN_Direct3DCreate9)(UINT);
static PFN_Direct3DCreate9 o_Direct3DCreate9;

static HRESULT __stdcall h_CreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags, D3DPRESENT_PARAMETERS* pp,
	IDirect3DDevice9** out)
{
	HRESULT hr = o_CreateDevice(d3d, adapter, type, wnd, flags, pp, out);
	std::string params = pp ? DescribePresent(pp) : "(no params)";
	if (FAILED(hr) || !out || !*out)
	{
		LogWarn("CreateDevice failed: 0x%08lX (%s)", hr, params.c_str());
		return hr;
	}
	char buf[1024];
	snprintf(buf, sizeof(buf), "Device: adapter %u, behaviour flags 0x%lX, %s", adapter, flags, params.c_str());
	g_deviceInfo = buf;
	LogInfo("%s", buf);
	HookDevice(*out);
	AfterDeviceReady(*out);
	return hr;
}

static IDirect3D9* WINAPI h_Direct3DCreate9(UINT sdk)
{
	IDirect3D9* d3d = o_Direct3DCreate9(sdk);
	if (d3d)
	{
		void** vt = *(void***)d3d;
		HookSlot(vt, D3D9_CreateDevice, h_CreateDevice, o_CreateDevice);
		LogInfo("Direct3DCreate9(%u) hooked (CreateDevice at %s).", sdk, Where((void*)o_CreateDevice).c_str());
	}
	return d3d;
}

static void LogD3D9Module()
{
	HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
	char path[MAX_PATH] = "(not loaded yet)";
	if (d3d9)
		GetModuleFileNameA(d3d9, path, MAX_PATH);
	bool reshade = d3d9 && GetProcAddress(d3d9, "ReShadeRegisterAddon");
	LogInfo("d3d9.dll: %s%s", path, reshade ? " (ReShade)" : "");
	if (GetModuleHandleA("ReShade32.dll"))
		LogInfo("ReShade32.dll is loaded.");
}

void FrameDump_Init(bool deviceHooksNeeded)
{
	bool dump = IniInt("Debug", "FrameDump", 1) != 0;
	if (!dump)
	{
		LogInfo("Frame dump disabled.");
		if (!deviceHooksNeeded)
			return;
	}
	g_hotkey = dump ? IniInt("Debug", "FrameDumpKey", VK_F8) : 0;
	g_framesToCapture = IniInt("Debug", "FrameDumpFrames", 1);
	if (g_framesToCapture < 1)
		g_framesToCapture = 1;
	if (g_framesToCapture > 4)
		g_framesToCapture = 4;
	InitializeCriticalSection(&g_lock);
	InitExeRange();
	LogD3D9Module();

	void* prev = PatchImport(NULL, "d3d9.dll", "Direct3DCreate9", (void*)h_Direct3DCreate9);
	if (!prev)
	{
		LogWarn("Could not hook the game's Direct3DCreate9 import (frame dump and soft shadows unavailable).");
		return;
	}
	o_Direct3DCreate9 = (PFN_Direct3DCreate9)prev;
	if (dump)
		LogInfo("Frame dump ready: press key 0x%02X in game to write AC2Shadows_FrameDump_*.txt (%d frame(s)).", g_hotkey, g_framesToCapture);
	else
		LogInfo("Direct3D hooks installed (needed by soft shadows).");
}
