// Frame dump: records how one frame is put together (render targets, depth buffers, clears,
// draw batches, which render-target textures are read where) and writes it to a text file
// when the hotkey is pressed. Used to work out which of AC2's depth buffers is the real scene
// depth and where in the frame effects such as AO should be injected.
//
// With [Debug] FrameDumpDepth the game's depth buffers are created as INTZ textures, so the dump
// can also read them back and compare each with the game's own z/w target (to see whether the
// depth prepass holds the whole scene). Every shader drawn is exported with the values of its
// named constants at its first draw (camera matrices, sun direction, ambient terms...).
//
// Hooks: the game's import of d3d9!Direct3DCreate9 -> IDirect3D9::CreateDevice -> the device
// vtable. If ReShade is installed as d3d9.dll these are ReShade's wrapper objects, so we see the
// calls exactly as the game makes them, and the return addresses point into the game exe.

#include "common.h"
#include "softshadows.h"
#include "ao.h"
#include "litshader.h"
#include "features.h"
#include "shaders_gen.h"
#include "core/shaderinfo.h"
#include "core/scene.h"
#include "core/fxpass.h"
#include "core/geoprep.h"
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
	DEV_GetRenderTargetData = 32,
	DEV_StretchRect = 34,
	DEV_ColorFill = 35,
	DEV_SetRenderTarget = 37,
	DEV_SetDepthStencilSurface = 39,
	DEV_BeginScene = 41,
	DEV_EndScene = 42,
	DEV_Clear = 43,
	DEV_SetViewport = 47,
	DEV_SetTexture = 65,
	DEV_SetSamplerState = 69,
	DEV_DrawPrimitive = 81,
	DEV_DrawIndexedPrimitive = 82,
	DEV_DrawPrimitiveUP = 83,
	DEV_DrawIndexedPrimitiveUP = 84,
	DEV_CreateVertexShader = 91,
	DEV_SetVertexShader = 92,
	DEV_SetVertexShaderConstantF = 94,
	DEV_CreatePixelShader = 106,
	DEV_SetPixelShader = 107,
};

static const GUID kIID_Texture9 = { 0x85c31227, 0x3de5, 0x4f00, { 0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5 } };
static const D3DFORMAT kFmtINTZ = (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z');

// [Debug] FrameDumpDepth: the game's depth buffers are created as INTZ depth textures (same
// 24-bit depth + 8-bit stencil, but readable from a shader) so the dump can read them back.
static bool g_readableDepth = false;
static bool g_intzSupported = false;

static bool g_dumpEnabled = false;            // [Debug] FrameDump
static volatile bool g_dumpRequested = false; // from the menu; starts at the next Present
static int g_dumpKey = 0;                      // [Debug] FrameDumpKey: optional hotkey, 0 = none (the default)
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
	bool intz = false;      // depth surface we created as level 0 of an INTZ texture
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
static std::unordered_map<void*, uint32_t> g_vsHash;
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
// With the frame dump on: the game's own bytecode of every pixel shader we changed (soft shadows
// and/or the AO output), so dumps export what the game shipped.
static std::unordered_map<void*, std::vector<DWORD>> g_gameCode;
// Pixel shaders (as the game knows them) that write the AO output; drawing anything else in the
// opaque pass leaves the AO target undefined there, which gets logged.
static std::unordered_map<void*, bool> g_aoOutput;
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
static void* g_vs;
static D3DVIEWPORT9 g_vp;

static void ResetTrackedState(IDirect3DSurface9* bb, IDirect3DSurface9* autoDepth)
{
	memset(g_rt, 0, sizeof(g_rt));
	memset(g_tex, 0, sizeof(g_tex));
	g_rt[0] = bb;
	g_ds = autoDepth;
	g_ps = nullptr;
	g_vs = nullptr;
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

// How each depth buffer was used this capture (to tell the prepass buffer from the main one, and
// the viewport depth range its values were written with).
struct DepthUse
{
	int draws = 0, zWrite = 0, colourOff = 0, firstEvent = 0;
	float minZ = 0, maxZ = 1; // viewport depth range of the last depth-writing draw
	bool mixedRange = false;  // depth-writing draws used more than one viewport depth range
};
static std::unordered_map<void*, DepthUse> g_depthUse;
static std::vector<void*> g_capDepth; // depth surfaces in order of first use (alive this frame)

// Shader constants at the first draw of each vertex / pixel shader this capture, to find where
// the game keeps its camera matrices, sun direction, ambient terms and so on.
enum { kVsConsts = 256, kPsConsts = 224, kMaxSnaps = 400 };
struct ShaderSnap
{
	void* shader;
	bool vertex;
	int draws, event;
	std::string bindings;
	std::vector<float> consts; // float4 registers c0..
};
static std::vector<ShaderSnap> g_snaps;
static std::unordered_map<void*, size_t> g_snapIndex[2]; // [vertex]

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
	bool colourOff = !(colorWrite & 0xF) || !g_rt[0];
	if (colourOff)
		g_seg.noColor++;
	if (g_ds)
	{
		auto ins = g_depthUse.emplace(g_ds, DepthUse());
		DepthUse& u = ins.first->second;
		if (ins.second)
		{
			u.firstEvent = g_event;
			g_capDepth.push_back(g_ds);
		}
		u.draws++;
		if (colourOff)
			u.colourOff++;
		if (zEnable && zWrite)
		{
			if (u.zWrite && (u.minZ != g_vp.MinZ || u.maxZ != g_vp.MaxZ))
				u.mixedRange = true;
			u.zWrite++;
			u.minZ = g_vp.MinZ;
			u.maxZ = g_vp.MaxZ;
		}
	}
	for (int vertex = 0; vertex < 2; vertex++)
	{
		void* sh = vertex ? g_vs : g_ps;
		if (!sh)
			continue;
		auto it = g_snapIndex[vertex].find(sh);
		if (it != g_snapIndex[vertex].end())
		{
			g_snaps[it->second].draws++;
			continue;
		}
		if (g_snaps.size() >= kMaxSnaps)
			continue;
		ShaderSnap sn;
		sn.shader = sh;
		sn.vertex = vertex != 0;
		sn.draws = 1;
		sn.event = g_event;
		sn.bindings = Bindings();
		sn.consts.assign((vertex ? kVsConsts : kPsConsts) * 4, 0.0f);
		if (vertex)
			dev->GetVertexShaderConstantF(0, sn.consts.data(), kVsConsts);
		else
			dev->GetPixelShaderConstantF(0, sn.consts.data(), kPsConsts);
		g_snapIndex[vertex][sh] = g_snaps.size();
		g_snaps.push_back(std::move(sn));
	}
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
	g_depthUse.clear();
	g_capDepth.clear();
	g_snaps.clear();
	g_snapIndex[0].clear();
	g_snapIndex[1].clear();
	g_framesLeft = g_framesToCapture;

	SYSTEMTIME t;
	GetLocalTime(&t);
	Out("AC2ShadowOverhaul frame dump %04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
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

// 8-bit greyscale (channels 1) or RGB (channels 3) PNG, uncompressed (stored deflate blocks).
static bool WritePng(const char* path, const std::vector<unsigned char>& px, unsigned w, unsigned h, unsigned channels = 1)
{
	std::string raw;
	size_t row = (size_t)w * channels;
	raw.reserve((row + 1) * h);
	for (unsigned y = 0; y < h; y++)
	{
		raw += '\0';
		raw.append((const char*)&px[(size_t)y * row], row);
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
	ihdr += std::string(channels == 3 ? "\x08\x02\x00\x00\x00" : "\x08\x00\x00\x00\x00", 5); // 8-bit grey / RGB
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

static std::vector<DWORD> ShaderCode(void* shader, bool vertex)
{
	UINT size = 0;
	std::vector<DWORD> code;
	if (vertex)
	{
		auto vs = (IDirect3DVertexShader9*)shader;
		if (FAILED(vs->GetFunction(nullptr, &size)) || !size || size > (1u << 20))
			return code;
		code.resize((size + 3) / 4);
		if (FAILED(vs->GetFunction(code.data(), &size)))
			code.clear();
	}
	else
	{
		{
			Lock l;
			auto it = g_gameCode.find(shader);
			if (it != g_gameCode.end())
				return it->second; // the game's original, not our rewrite
		}
		auto ps = (IDirect3DPixelShader9*)shader;
		if (FAILED(ps->GetFunction(nullptr, &size)) || !size || size > (1u << 20))
			return code;
		code.resize((size + 3) / 4);
		if (FAILED(ps->GetFunction(code.data(), &size)))
			code.clear();
	}
	return code;
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
		std::vector<DWORD> code = ShaderCode(sp.ps, false); // the game's original, not our rewrite
		UINT size = (UINT)code.size() * 4;
		if (code.empty() || !ShaderDeclaresSampler(code.data(), code.size(), sp.stage))
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

// Original device functions (filled in by HookDevice).
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
ORIG(GetRenderTargetData, HRESULT, IDirect3DSurface9*, IDirect3DSurface9*)
ORIG(SetSamplerState, HRESULT, DWORD, D3DSAMPLERSTATETYPE, DWORD)
ORIG(SetVertexShaderConstantF, HRESULT, UINT, const float*, UINT)
ORIG(DrawPrimitive, HRESULT, D3DPRIMITIVETYPE, UINT, UINT)
ORIG(DrawIndexedPrimitive, HRESULT, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT)
ORIG(DrawPrimitiveUP, HRESULT, D3DPRIMITIVETYPE, UINT, const void*, UINT)
ORIG(DrawIndexedPrimitiveUP, HRESULT, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT)
ORIG(CreateVertexShader, HRESULT, const DWORD*, IDirect3DVertexShader9**)
ORIG(SetVertexShader, HRESULT, IDirect3DVertexShader9*)
ORIG(CreatePixelShader, HRESULT, const DWORD*, IDirect3DPixelShader9**)
ORIG(SetPixelShader, HRESULT, IDirect3DPixelShader9*)

// Text overlays (menus, warnings) draw through the device vtable: pass their calls straight on.
#define FX_PASSTHROUGH(name, ...) \
	if (g_fxPassthrough) \
	return o_##name(dev, ##__VA_ARGS__)


// Reads level 0 of an R32F / R16F render-target texture into floats.
static bool ReadFloats(IDirect3DDevice9* dev, IDirect3DTexture9* tex, std::vector<float>& v, UINT& w, UINT& h, std::string& err)
{
	D3DSURFACE_DESC d = {};
	if (FAILED(tex->GetLevelDesc(0, &d)))
	{
		err = "GetLevelDesc failed";
		return false;
	}
	if (d.Format != D3DFMT_R32F && d.Format != D3DFMT_R16F)
	{
		err = "not R32F / R16F";
		return false;
	}
	IDirect3DSurface9 *src = nullptr, *sys = nullptr;
	if (FAILED(tex->GetSurfaceLevel(0, &src)) || !src)
	{
		err = "GetSurfaceLevel failed";
		return false;
	}
	bool ok = false;
	if (FAILED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL)) || !sys)
	{
		err = "CreateOffscreenPlainSurface failed";
	}
	else
	{
		D3DLOCKED_RECT lr;
		if (FAILED(dev->GetRenderTargetData(src, sys)) || FAILED(sys->LockRect(&lr, NULL, D3DLOCK_READONLY)))
		{
			err = "GetRenderTargetData/LockRect failed";
		}
		else
		{
			w = d.Width;
			h = d.Height;
			v.resize((size_t)w * h);
			for (UINT y = 0; y < h; y++)
			{
				const unsigned char* row = (const unsigned char*)lr.pBits + (size_t)y * lr.Pitch;
				for (UINT x = 0; x < w; x++)
					v[(size_t)y * w + x] = d.Format == D3DFMT_R32F ? ((const float*)row)[x] : HalfToFloat(((const uint16_t*)row)[x]);
			}
			sys->UnlockRect();
			ok = true;
		}
		sys->Release();
	}
	src->Release();
	return ok;
}

static const char* FileName(const char* path)
{
	const char* name = strrchr(path, '\\');
	return name ? name + 1 : path;
}

// Statistics of a depth-like float image (cleared to 1.0) plus a greyscale picture of it.
static std::string AnalyseFloats(const std::vector<float>& v, UINT w, UINT h, const char* label, const char* pngPath)
{
	const float clearValue = 1.0f; // the game clears its float targets and depth buffers to 1
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
	if (!samples)
		return "empty";
	std::sort(geo.begin(), geo.end());
	auto pct = [&](double p) { return geo.empty() ? 0.0f : geo[(size_t)(p * (geo.size() - 1))]; };
	float lo = pct(0.01), hi = pct(0.99);
	auto at = [&](double fx, double fy) { return v[(size_t)(fy * (h - 1)) * w + (size_t)(fx * (w - 1))]; };
	char buf[700];
	snprintf(buf, sizeof(buf),
		"%s %ux%u | cleared (==1.0) %.1f%% | >1.0 %.1f%% | <0 %.1f%% | geometry min %g p1 %g p25 %g p50 %g p75 %g p99 %g max %g | "
		"centre %g, lower-centre %g, left %g, right %g",
		label, w, h, 100.0 * cleared / samples, 100.0 * above1 / samples, 100.0 * negative / samples, geo.empty() ? 0.0f : geo.front(),
		lo, pct(0.25), pct(0.5), pct(0.75), hi, geo.empty() ? 0.0f : geo.back(), at(0.5, 0.5), at(0.5, 0.75), at(0.25, 0.5),
		at(0.75, 0.5));
	std::string result = buf;

	// Image: geometry from p1 (black) to p99 (light grey), cleared pixels white. The values are z/w,
	// which bunches everything past a few metres just below 1.0; log(1 - z/w) is log distance
	// (distance ~ near / (1 - z/w)), so equal grey steps are equal distance ratios.
	unsigned step = (w + 1279) / 1280;
	unsigned ow = w / step, oh = h / step;
	std::vector<unsigned char> px((size_t)ow * oh);
	auto logDist = [](float f) { return -logf(fmaxf(1.0f - f, 1e-7f)); };
	float logLo = logDist(lo), logHi = logDist(hi);
	float range = logHi > logLo ? logHi - logLo : 1.0f;
	for (unsigned y = 0; y < oh; y++)
		for (unsigned x = 0; x < ow; x++)
		{
			float f = v[(size_t)(y * step) * w + x * step];
			float n = (logDist(f) - logLo) / range;
			n = n < 0 ? 0 : (n > 1 ? 1 : n);
			px[(size_t)y * ow + x] = f == clearValue ? 255 : (unsigned char)(n * 230.0f);
		}
	if (WritePng(pngPath, px, ow, oh))
		result += std::string(" | image ") + FileName(pngPath);
	return result;
}

// ---- Depth buffer readback ---------------------------------------------------------------------
// With [Debug] FrameDumpDepth the game's depth buffers are INTZ textures. To read one, it is drawn
// into an R32F render target with a tiny pixel shader and that target is read back.

static IDirect3DPixelShader9* g_depthPs; // shaders survive Reset; kept for the device's lifetime

// Our own calls go straight to the original device functions so the dump's tracked bindings stay
// the game's; render targets, depth buffer and all other state are restored afterwards.
static bool ReadDepth(IDirect3DDevice9* dev, IDirect3DSurface9* depthSurf, std::vector<float>& v, UINT& w, UINT& h, std::string& err)
{
	IDirect3DTexture9* depthTex = nullptr;
	if (FAILED(depthSurf->GetContainer(kIID_Texture9, (void**)&depthTex)) || !depthTex)
	{
		err = "not a texture";
		return false;
	}
	D3DSURFACE_DESC d = {};
	depthTex->GetLevelDesc(0, &d);
	if (!g_depthPs && FAILED(o_CreatePixelShader(dev, kDepthToColourPs, &g_depthPs)))
		g_depthPs = nullptr;
	IDirect3DTexture9* rt = nullptr;
	IDirect3DSurface9* rtSurf = nullptr;
	if (!g_depthPs)
		err = "could not create the copy shader";
	else if (FAILED(o_CreateTexture(dev, d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &rt, NULL)) || !rt)
		err = "could not create an R32F render target";
	else if (FAILED(rt->GetSurfaceLevel(0, &rtSurf)))
		rtSurf = nullptr;

	bool ok = false;
	if (rtSurf)
	{
		IDirect3DStateBlock9* sb = nullptr;
		if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)))
			sb = nullptr;
		IDirect3DSurface9* oldRt[4] = {};
		IDirect3DSurface9* oldDs = nullptr;
		for (DWORD i = 0; i < 4; i++)
			if (FAILED(dev->GetRenderTarget(i, &oldRt[i])))
				oldRt[i] = nullptr;
		if (FAILED(dev->GetDepthStencilSurface(&oldDs)))
			oldDs = nullptr;

		o_SetRenderTarget(dev, 0, rtSurf);
		for (DWORD i = 1; i < 4; i++)
			o_SetRenderTarget(dev, i, nullptr);
		o_SetDepthStencilSurface(dev, nullptr);
		D3DVIEWPORT9 vp = { 0, 0, d.Width, d.Height, 0.0f, 1.0f };
		o_SetViewport(dev, &vp);
		static const D3DRENDERSTATETYPE kOff[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_STENCILENABLE, D3DRS_ALPHABLENDENABLE,
			D3DRS_ALPHATESTENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_CLIPPLANEENABLE,
			D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_TWOSIDEDSTENCILMODE };
		for (D3DRENDERSTATETYPE rs : kOff)
			dev->SetRenderState(rs, 0);
		dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
		dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
		o_SetVertexShader(dev, nullptr);
		dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		o_SetPixelShader(dev, g_depthPs);
		o_SetTexture(dev, 0, depthTex);
		dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
		dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
		dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, 0);
		struct V
		{
			float x, y, z, rhw, u, v;
		};
		float fw = (float)d.Width - 0.5f, fh = (float)d.Height - 0.5f; // D3D9 half-pixel offset
		V quad[4] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { fw, -0.5f, 0, 1, 1, 0 }, { -0.5f, fh, 0, 1, 0, 1 }, { fw, fh, 0, 1, 1, 1 } };
		bool began = SUCCEEDED(o_BeginScene(dev));
		HRESULT hr = o_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
		if (began)
			o_EndScene(dev);
		o_SetTexture(dev, 0, nullptr);

		for (DWORD i = 0; i < 4; i++)
			if (i > 0 || oldRt[i])
				o_SetRenderTarget(dev, i, oldRt[i]);
		o_SetDepthStencilSurface(dev, oldDs);
		if (sb)
		{
			sb->Apply();
			sb->Release();
		}
		for (IDirect3DSurface9* s : oldRt)
			if (s)
				s->Release();
		if (oldDs)
			oldDs->Release();

		if (FAILED(hr))
			err = "draw failed";
		else
			ok = ReadFloats(dev, rt, v, w, h, err);
		rtSurf->Release();
	}
	if (rt)
		rt->Release();
	depthTex->Release();
	return ok;
}

// Compares a depth buffer with the game's own z/w target (T6) pixel by pixel. The depth buffer
// holds minZ + z/w * (maxZ - minZ) for the viewport depth range it was drawn with. With n = near
// plane, 1 - z/w is roughly n / distance, so ratio = (1 - zDepth) / (1 - zT6) ~ distT6 / distDepth:
//   ~1  both see the same surface               (grey: brighter = nearer)
//   < 1 the depth buffer sees something farther (red: geometry missing from the depth buffer)
//   > 1 the depth buffer sees something nearer  (blue: e.g. occluders the final image doesn't have)
//   depth buffer has geometry where T6 is sky   (yellow), both sky (black)
static std::string CompareDepth(const std::vector<float>& dv, const DepthUse& use, const std::vector<float>& tv, UINT w, UINT h,
	const char* pngPath)
{
	float range = use.maxZ - use.minZ;
	if (range <= 0)
		range = 1;
	unsigned step = (w + 1279) / 1280;
	unsigned ow = w / step, oh = h / step;
	std::vector<unsigned char> px((size_t)ow * oh * 3);
	size_t bothSky = 0, onlyDepth = 0, onlyT = 0, same = 0, farther = 0, nearer = 0;
	std::vector<float> ratios;
	ratios.reserve((size_t)ow * oh);
	for (unsigned y = 0; y < oh; y++)
		for (unsigned x = 0; x < ow; x++)
		{
			size_t i = (size_t)(y * step) * w + x * step;
			float t = tv[i];
			float d = (dv[i] - use.minZ) / range;
			bool tSky = t >= 1.0f, dSky = d >= 0.99999f;
			unsigned char* c = &px[((size_t)y * ow + x) * 3];
			unsigned char r = 0, g = 0, b = 0;
			if (tSky && dSky)
			{
				bothSky++;
			}
			else if (tSky)
			{
				onlyDepth++;
				r = 230, g = 200, b = 40;
			}
			else if (dSky)
			{
				onlyT++;
				r = 255, g = 0, b = 0;
			}
			else
			{
				float ratio = (1.0f - d) / (1.0f - t);
				ratios.push_back(ratio);
				if (ratio < 0.97f)
				{
					farther++;
					r = 230, g = 40, b = 40;
				}
				else if (ratio > 1.03f)
				{
					nearer++;
					r = 60, g = 90, b = 255;
				}
				else
				{
					same++;
					float n = sqrtf((1.0f - t) / 0.06f);
					r = g = b = (unsigned char)(50 + 190 * (n > 1 ? 1 : n));
				}
			}
			c[0] = r, c[1] = g, c[2] = b;
		}
	size_t geo = (size_t)ow * oh - bothSky;
	if (!geo)
		return "no geometry in either";
	float median = 0;
	if (!ratios.empty())
	{
		std::nth_element(ratios.begin(), ratios.begin() + ratios.size() / 2, ratios.end());
		median = ratios[ratios.size() / 2];
	}
	char buf[600];
	snprintf(buf, sizeof(buf),
		"of the %.1f%% of pixels with geometry: same surface %.1f%% | MISSING from the depth buffer %.1f%% (it sees farther %.1f%%, "
		"sky %.1f%%) | depth buffer nearer %.1f%% | depth buffer has geometry where T6 is sky %.1f%% | median distance ratio %.4f "
		"(depth range %g..%g)",
		100.0 * geo / ((size_t)ow * oh), 100.0 * same / geo, 100.0 * (farther + onlyT) / geo, 100.0 * farther / geo, 100.0 * onlyT / geo,
		100.0 * nearer / geo, 100.0 * onlyDepth / geo, median, use.minZ, use.maxZ);
	std::string result = buf;
	if (WritePng(pngPath, px, ow, oh, 3))
		result += std::string(" | image ") + FileName(pngPath);
	return result;
}

// Writes every vertex / pixel shader drawn this capture: bytecode (<base>_allshaders\vs_ / ps_<hash>.cso)
// and, in <base>_vs.txt / <base>_ps.txt, the value of each named constant at the shader's first draw
// followed by its disassembly.
static void ExportShaderSnaps(const char* base)
{
	char dir[MAX_PATH];
	snprintf(dir, sizeof(dir), "%s_allshaders", base);
	CreateDirectoryA(dir, nullptr);
	static const char* kClass[] = { "scalar", "vector", "matrix (row-major: one register per row)",
		"matrix (column-major: one register per column)", "object", "struct" };
	static const char* kSet[] = { "bool", "int4", "float4", "sampler" };
	int counts[2] = {};
	for (int vertex = 1; vertex >= 0; vertex--)
	{
		std::vector<const ShaderSnap*> list;
		for (const ShaderSnap& s : g_snaps)
			if (s.vertex == (vertex != 0))
				list.push_back(&s);
		std::sort(list.begin(), list.end(), [](const ShaderSnap* a, const ShaderSnap* b) { return a->draws > b->draws; });
		std::string text;
		for (const ShaderSnap* s : list)
		{
			std::vector<DWORD> code = ShaderCode(s->shader, vertex != 0);
			if (code.empty())
				continue;
			uint32_t h = HashShader(code.data());
			char path[MAX_PATH];
			snprintf(path, sizeof(path), "%s\\%s_%08X.cso", dir, vertex ? "vs" : "ps", h);
			if (FILE* f = fopen(path, "wb"))
			{
				fwrite(code.data(), 4, code.size(), f);
				fclose(f);
			}
			char line[400];
			snprintf(line, sizeof(line), "==== %s %08X | %d draws | first drawn after event %d [%s] ====\n", vertex ? "VS" : "PS", h,
				s->draws, s->event, s->bindings.c_str());
			text += line;
			text += "Constants at the first draw:\n";
			int regs = vertex ? kVsConsts : kPsConsts;
			core::ConstantTable table;
			core::ReadConstantTable(code.data(), table, code.size());
			for (const core::ShaderConstant& ce : table.constants)
			{
				snprintf(line, sizeof(line), "  %s %s  %s%d..%d  %s%s\n", ce.set < 4 ? kSet[ce.set] : "?", ce.name.c_str(),
					ce.set == 3 ? "s" : ce.set == 2 ? "c" : ce.set == 1 ? "i" : "b", ce.reg, ce.reg + ce.count - 1,
					ce.cls < 6 ? kClass[ce.cls] : "?", ce.rows > 1 || ce.cols > 1 ? (" " + std::to_string(ce.rows) + "x" + std::to_string(ce.cols)).c_str() : "");
				text += line;
				if (ce.set != 2)
					continue;
				for (int r = ce.reg; r < ce.reg + ce.count && r < regs; r++)
				{
					const float* c = &s->consts[(size_t)r * 4];
					snprintf(line, sizeof(line), "      c%-3d = (%.7g, %.7g, %.7g, %.7g)\n", r, c[0], c[1], c[2], c[3]);
					text += line;
				}
			}
			text += DisassembleShader(code.data(), (UINT)code.size() * 4);
			text += "\n\n";
			counts[vertex]++;
		}
		char path[MAX_PATH];
		snprintf(path, sizeof(path), "%s_%s.txt", base, vertex ? "vs" : "ps");
		if (FILE* f = fopen(path, "w"))
		{
			fwrite(text.data(), 1, text.size(), f);
			fclose(f);
		}
	}
	Out("");
	Out("Shaders drawn: %d vertex, %d pixel (named constants at first draw + disassembly in %s_vs.txt / %s_ps.txt, bytecode in "
		"%s_allshaders)",
		counts[1], counts[0], FileName(base), FileName(base), FileName(base));
}

static void EndCapture(IDirect3DDevice9* dev)
{
	SYSTEMTIME t;
	GetLocalTime(&t);
	char base[MAX_PATH];
	snprintf(base, sizeof(base), "%sAC2ShadowOverhaul_FrameDump_%04d%02d%02d_%02d%02d%02d", ModuleDir(), t.wYear, t.wMonth, t.wDay,
		t.wHour, t.wMinute, t.wSecond);

	std::string text;
	{
		Lock l;
		FlushSegment();
		g_capturing = false;
		Out("");
		Out("Screen-sized float render targets sampled this frame (read back at Present):");
		int n = 0;
		struct FloatImage
		{
			std::string name;
			std::vector<float> v;
			UINT w, h;
		};
		std::vector<FloatImage> screenFloats; // R32F ones, compared with the depth buffers below
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
			FloatImage img = { it->second.name, {}, 0, 0 };
			std::string err;
			if (!ReadFloats(dev, t9, img.v, img.w, img.h, err))
				Out("  %s: %s", img.name.c_str(), err.c_str());
			else
			{
				Out("  %s: %s", img.name.c_str(), AnalyseFloats(img.v, img.w, img.h, FormatName(d.Format).c_str(), png).c_str());
				if (d.Format == D3DFMT_R32F)
				{
					// Raw copy ("AC2F", width, height, then floats row by row) for replaying the frame offline
					// (tests/ao_warp_test.cpp replay <file>).
					char raw[MAX_PATH];
					snprintf(raw, sizeof(raw), "%s_%s.f32", base, img.name.c_str());
					if (FILE* f = fopen(raw, "wb"))
					{
						const uint32_t hdr[3] = { 0x46324341u, img.w, img.h };
						fwrite(hdr, 4, 3, f);
						fwrite(img.v.data(), 4, img.v.size(), f);
						fclose(f);
						Out("    raw floats: %s", FileName(raw));
					}
					screenFloats.push_back(std::move(img));
				}
			}
			n++;
		}
		if (!n)
			Out("  (none)");
		Out("");
		Out("Ambient occlusion / contact shadow inputs (pixel shader constants c0-c7 of the last pass):");
		Out("  %s", AO_FrameInfo().c_str());

		Out("");
		Out("Depth buffers used this frame (draws / depth-writing / colour-off, viewport depth range of the depth writes):");
		for (void* s : g_capDepth)
		{
			auto it = g_res.find(s);
			std::string name = it != g_res.end() ? it->second.name : SurfName((IDirect3DSurface9*)s, false);
			const DepthUse& u = g_depthUse[s];
			Out("  %s: %d draws, %d depth-writing, %d colour-off, first after event %d, depth range %g..%g%s", name.c_str(), u.draws,
				u.zWrite, u.colourOff, u.firstEvent, u.minZ, u.maxZ, u.mixedRange ? " (MIXED ranges)" : "");
			if (!g_readableDepth)
				continue;
			if (it == g_res.end() || !it->second.intz)
			{
				Out("    not readable (not created as INTZ)");
				continue;
			}
			std::vector<float> v;
			UINT w = 0, h = 0;
			std::string err;
			if (!ReadDepth(dev, (IDirect3DSurface9*)s, v, w, h, err))
			{
				Out("    readback failed: %s", err.c_str());
				continue;
			}
			char png[MAX_PATH];
			snprintf(png, sizeof(png), "%s_%s.png", base, name.c_str());
			Out("    %s", AnalyseFloats(v, w, h, "INTZ", png).c_str());
			for (const FloatImage& img : screenFloats)
			{
				if (img.w != w || img.h != h)
					continue;
				snprintf(png, sizeof(png), "%s_%s_vs_%s.png", base, name.c_str(), img.name.c_str());
				Out("    vs %s: %s", img.name.c_str(), CompareDepth(v, u, img.v, w, h, png).c_str());
			}
		}
		if (g_capDepth.empty())
			Out("  (none)");
		ExportShadowShaders(base);
		ExportShaderSnaps(base);
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
	core::Scene_OnDeviceReady(bb);
}

// Ambient occlusion runs inside the hooks below (right after the opaque pass); dumps note where.
static void NoteAO(bool applied)
{
	if (applied && g_capturing)
	{
		Lock l;
		Event("Ambient occlusion applied (GTAO, blur, composite into the scene colour)", "AC2ShadowOverhaul");
	}
}

static void BeforeDraw(IDirect3DDevice9* dev)
{
	core::Scene_BeforeDraw(dev, (IDirect3DVertexShader9*)g_vs, (IDirect3DPixelShader9*)g_ps);
	NoteAO(AO_BeforeDraw(dev));
	Fx_BeforeDraw(dev);
	if (AO_InPass() && g_ps)
	{
		static int logged = 0;
		if (logged >= 10)
			return;
		Lock l;
		auto it = g_aoOutput.find(g_ps);
		if (it == g_aoOutput.end() || !it->second)
		{
			g_aoOutput[g_ps] = true; // log each shader once
			auto h = g_psHash.find(g_ps);
			logged++;
			LogWarn("Ambient occlusion: shader %08X is drawn in the opaque pass without the AO output (AO may be wrong on it).",
				h != g_psHash.end() ? h->second : 0u);
		}
	}
}

static HRESULT __stdcall h_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
	LogInfo("Device Reset: %s", DescribePresent(pp).c_str());
	core::Scene_OnLostDevice();
	core::Geo_OnLostDevice();
	AO_OnLostDevice();
	Fx_OnLostDevice();
	HRESULT hr = o_Reset(dev, pp);
	if (SUCCEEDED(hr))
	{
		AfterDeviceReady(dev);
		Fx_OnResetDevice(dev, pp);
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
		LogInfo("First Present seen%s.", g_dumpEnabled ? "; frame dump available from the menu" : "");
	}
	NoteAO(AO_BeforePresent(dev));
	Fx_BeforePresent(dev);
	core::Scene_EndFrame(); // after every effect used this frame's camera
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

	if (!g_capturing)
	{
		g_fxPassthrough = true;
		if (Tuning_Enabled())
			Tuning_OnPresent(dev);
		Fx_DrawOverlays(dev);
		g_fxPassthrough = false;
	}
	HRESULT hr = o_Present(dev, a, b, w, r);

	static bool keyWas = false;
	bool key = g_dumpKey > 0 && g_dumpKey < 256 && (GetAsyncKeyState(g_dumpKey) & 0x8000) != 0;
	if (key && !keyWas && GameHasFocus())
		g_dumpRequested = true;
	keyWas = key;
	if (g_dumpRequested && !g_capturing)
	{
		g_dumpRequested = false;
		BeginCapture();
	}
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

// [Debug] FrameDumpDepth: hand the game level 0 of an INTZ depth texture instead of a plain depth
// surface. The surface keeps the texture alive (its reference count is the texture's), so the
// texture goes away when the game releases the surface. Not for MSAA (INTZ can't be multisampled)
// or the shadow atlas depth buffer.
static bool CreateReadableDepth(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, IDirect3DSurface9** out,
	HANDLE* shared)
{
	if (!g_readableDepth || !g_intzSupported || ms != D3DMULTISAMPLE_NONE || shared || !out)
		return false;
	if (fmt != D3DFMT_D24S8 && fmt != D3DFMT_D24X8)
		return false;
	if (h >= 256 && w == 4 * h)
		return false;
	IDirect3DTexture9* tex = nullptr;
	if (FAILED(o_CreateTexture(dev, w, h, 1, D3DUSAGE_DEPTHSTENCIL, kFmtINTZ, D3DPOOL_DEFAULT, &tex, nullptr)) || !tex)
	{
		LogWarn("Readable depth: could not create a %ux%u INTZ texture; using a normal depth buffer.", w, h);
		return false;
	}
	IDirect3DSurface9* s = nullptr;
	HRESULT hr = tex->GetSurfaceLevel(0, &s);
	tex->Release();
	if (FAILED(hr) || !s)
		return false;
	*out = s;
	return true;
}

static HRESULT __stdcall h_CreateDepthStencilSurface(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms,
	DWORD q, BOOL discard, IDirect3DSurface9** out, HANDLE* shared)
{
	if (CreateReadableDepth(dev, w, h, fmt, ms, out, shared))
	{
		Lock l;
		Register(*out, 'D', w, h, fmt, kFmtINTZ, D3DUSAGE_DEPTHSTENCIL, ms, false, CALLER);
		g_res[*out].intz = true;
		return D3D_OK;
	}
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
	FX_PASSTHROUGH(StretchRect, src, sr, dst, dr, filter);
	if (g_capturing)
	{
		Lock l;
		Event("StretchRect " + SurfName(src, true) + " -> " + SurfName(dst, true), CALLER);
	}
	NoteAO(AO_BeforeOther(dev));
	Fx_BeforeCopyFrom(dev, src, true);
	return o_StretchRect(dev, src, sr, dst, dr, filter);
}

static HRESULT __stdcall h_ColorFill(IDirect3DDevice9* dev, IDirect3DSurface9* s, const RECT* r, D3DCOLOR c)
{
	FX_PASSTHROUGH(ColorFill, s, r, c);
	if (g_capturing)
	{
		Lock l;
		char buf[32];
		snprintf(buf, sizeof(buf), " colour %08lX", c);
		Event("ColorFill " + SurfName(s, true) + buf, CALLER);
	}
	NoteAO(AO_BeforeOther(dev));
	Fx_BeforeOther(dev);
	return o_ColorFill(dev, s, r, c);
}

static HRESULT __stdcall h_SetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* s)
{
	FX_PASSTHROUGH(SetRenderTarget, index, s);
	NoteAO(AO_BeforeSetRenderTarget(dev, index, s));
	Fx_BeforeSetRenderTarget(dev, index, s);
	HRESULT hr = o_SetRenderTarget(dev, index, s);
	if (SUCCEEDED(hr))
		core::Scene_AfterSetRenderTarget(index, s);
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
				g_vp.MinZ = 0.0f;
				g_vp.MaxZ = 1.0f;
			}
		}
		AO_AfterSetRenderTarget(dev, index, s, g_rt[0]);
	}
	if (SUCCEEDED(hr))
		Fx_AfterSetRenderTarget(dev, index, s, index < 4 ? g_rt[0] : nullptr);
	return hr;
}

static HRESULT __stdcall h_SetDepthStencilSurface(IDirect3DDevice9* dev, IDirect3DSurface9* s)
{
	FX_PASSTHROUGH(SetDepthStencilSurface, s);
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
	FX_PASSTHROUGH(BeginScene);
	if (g_capturing)
	{
		Lock l;
		Event("BeginScene", CALLER);
	}
	return o_BeginScene(dev);
}

static HRESULT __stdcall h_EndScene(IDirect3DDevice9* dev)
{
	FX_PASSTHROUGH(EndScene);
	if (g_capturing)
	{
		Lock l;
		Event("EndScene", CALLER);
	}
	NoteAO(AO_BeforeOther(dev));
	Fx_BeforeOther(dev);
	return o_EndScene(dev);
}

static HRESULT __stdcall h_Clear(IDirect3DDevice9* dev, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR c, float z, DWORD stencil)
{
	FX_PASSTHROUGH(Clear, count, rects, flags, c, z, stencil);
	if (g_capturing)
	{
		Lock l;
		char buf[160];
		snprintf(buf, sizeof(buf), "Clear%s%s%s colour %08lX z %g stencil %lu rects %lu | ", (flags & D3DCLEAR_TARGET) ? " COLOUR" : "",
			(flags & D3DCLEAR_ZBUFFER) ? " DEPTH" : "", (flags & D3DCLEAR_STENCIL) ? " STENCIL" : "", c, z, stencil, count);
		Event(buf + Bindings(), CALLER);
	}
	NoteAO(AO_BeforeOther(dev));
	Fx_BeforeOther(dev);
	return o_Clear(dev, count, rects, flags, c, z, stencil);
}

static HRESULT __stdcall h_SetViewport(IDirect3DDevice9* dev, const D3DVIEWPORT9* vp)
{
	FX_PASSTHROUGH(SetViewport, vp);
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
		core::Scene_AfterSetViewport(vp);
	}
	return hr;
}

static HRESULT __stdcall h_SetTexture(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* tex)
{
	FX_PASSTHROUGH(SetTexture, stage, tex);
	int slot = -1;
	if (stage < 16)
		slot = (int)stage;
	else if (stage >= D3DVERTEXTEXTURESAMPLER0 && stage <= D3DVERTEXTEXTURESAMPLER3)
		slot = 16 + (int)(stage - D3DVERTEXTEXTURESAMPLER0);
	if (slot >= 0)
		g_tex[slot] = tex;
	Fx_BeforeSetTexture(stage, tex);
	HRESULT hr = o_SetTexture(dev, stage, tex);
	if (SUCCEEDED(hr))
		Fx_AfterSetTexture(stage, tex);
	return hr;
}

static HRESULT __stdcall h_SetSamplerState(IDirect3DDevice9* dev, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value)
{
	FX_PASSTHROUGH(SetSamplerState, sampler, type, value);
	return o_SetSamplerState(dev, sampler, type, Fx_SamplerState(sampler, type, value));
}

static HRESULT __stdcall h_SetVertexShaderConstantF(IDirect3DDevice9* dev, UINT start, const float* v, UINT count)
{
	HRESULT hr = o_SetVertexShaderConstantF(dev, start, v, count);
	if (!g_fxPassthrough)
		Fx_AfterVertexConstants(dev, start, count);
	return hr;
}

static HRESULT __stdcall h_GetRenderTargetData(IDirect3DDevice9* dev, IDirect3DSurface9* src, IDirect3DSurface9* dst)
{
	FX_PASSTHROUGH(GetRenderTargetData, src, dst);
	Fx_BeforeCopyFrom(dev, src, false);
	return o_GetRenderTargetData(dev, src, dst);
}

static HRESULT __stdcall h_DrawPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT start, UINT count)
{
	FX_PASSTHROUGH(DrawPrimitive, t, start, count);
	BeforeDraw(dev);
	OnDraw(dev, count);
	return o_DrawPrimitive(dev, t, start, count);
}

static HRESULT __stdcall h_DrawIndexedPrimitive(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, INT base, UINT minIdx, UINT numV, UINT start, UINT count)
{
	FX_PASSTHROUGH(DrawIndexedPrimitive, t, base, minIdx, numV, start, count);
	BeforeDraw(dev);
	OnDraw(dev, count);
	return o_DrawIndexedPrimitive(dev, t, base, minIdx, numV, start, count);
}

static HRESULT __stdcall h_DrawPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT count, const void* data, UINT stride)
{
	FX_PASSTHROUGH(DrawPrimitiveUP, t, count, data, stride);
	BeforeDraw(dev);
	OnDraw(dev, count);
	return o_DrawPrimitiveUP(dev, t, count, data, stride);
}

static HRESULT __stdcall h_DrawIndexedPrimitiveUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE t, UINT minIdx, UINT numV, UINT count,
	const void* idx, D3DFORMAT idxFmt, const void* data, UINT stride)
{
	FX_PASSTHROUGH(DrawIndexedPrimitiveUP, t, minIdx, numV, count, idx, idxFmt, data, stride);
	BeforeDraw(dev);
	OnDraw(dev, count);
	return o_DrawIndexedPrimitiveUP(dev, t, minIdx, numV, count, idx, idxFmt, data, stride);
}

// Creates a pixel shader with what the effects need added to it when it is part of the main opaque
// pass (it writes the z/w target as oC1): the AO / contact shadow outputs and the SSGI bounce light,
// both from one rewrite (litshader.h); otherwise as given. kind: 0 no AO output, 1 AO output = 0,
// 2 AO output = indirect light. If the driver rejects the shader with both parts, each is tried on
// its own before the game's code.
static LONG g_aoMatched = 0, g_aoZero = 0, g_aoFailed = 0;
static HRESULT CreatePs(IDirect3DDevice9* dev, const DWORD* code, IDirect3DPixelShader9** out, int* kind, std::string* why)
{
	if (kind)
		*kind = 0;
	LitParts want;
	want.ao = AO_Outputs();
	want.gi = Fx_GiShaders();
	if (!want.ao && !want.gi)
		return o_CreatePixelShader(dev, code, out);
	const uint32_t hash = HashShader(code);
	LitParts tries[3] = { want, want, want };
	tries[1].gi = false; // AO only
	tries[2].ao = false; // SSGI only
	std::string giWhy;
	for (int t = 0; t < 3; t++)
	{
		std::vector<DWORD> lit;
		LitRewriteResult r;
		if (!Lit_RewritePS(code, tries[t], lit, r))
		{
			if (t == 0)
				giWhy = r.giWhy;
			break;
		}
		if (t == 0)
			giWhy = r.giWhy;
		HRESULT hr = o_CreatePixelShader(dev, lit.data(), out);
		if (SUCCEEDED(hr))
		{
			if (kind)
				*kind = r.ao ? (r.aoMatched ? 2 : 1) : 0;
			if (why)
				*why = r.aoWhy;
			if (want.gi)
				Fx_NoteGiRewrite(hash, r.gi, giWhy);
			return hr;
		}
		if (r.gi)
		{
			static LONG failed = 0;
			if (InterlockedIncrement(&failed) <= 10)
				LogWarn("SSGI: the driver rejected rewritten shader %08X (0x%08lX).", hash, hr);
		}
		else if (InterlockedIncrement(&g_aoFailed) <= 10)
			LogWarn("Ambient occlusion: the driver rejected the AO output for shader %08X (0x%08lX); using it without.", hash, hr);
		if (t == 0 && !(r.ao && r.gi))
			break; // only one part applied: nothing smaller to try
		if (t == 0)
			giWhy = "the driver rejected it";
	}
	if (want.gi)
		Fx_NoteGiRewrite(hash, false, giWhy);
	return o_CreatePixelShader(dev, code, out);
}

static HRESULT __stdcall h_CreatePixelShader(IDirect3DDevice9* dev, const DWORD* code, IDirect3DPixelShader9** out)
{
	FX_PASSTHROUGH(CreatePixelShader, code, out);
	SoftSet set;
	bool haveSet = false;
	HRESULT hr = E_FAIL;
	int aoKind = 0;
	std::string aoWhy;
	if (code && out)
	{
		const int width = SoftShadows_Width();
		std::vector<DWORD> soft;
		std::string reason;
		if (SoftShadows_Rewrite(code, width, soft, reason))
		{
			hr = CreatePs(dev, soft.data(), out, &aoKind, &aoWhy);
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
				// The game's filter (F6) and the debug menu's other widths. Each gets the AO output too,
				// as any of them may be drawn in the opaque pass.
				if (FAILED(CreatePs(dev, code, &set.original, nullptr, nullptr)))
					set.original = nullptr;
				for (int w = 1; w <= 4 && SoftShadows_BuildAllWidths(); w++)
				{
					if (w == width)
						continue;
					std::vector<DWORD> v;
					std::string why;
					if (SoftShadows_Rewrite(code, w, v, why) && FAILED(CreatePs(dev, v.data(), &set.byWidth[w], nullptr, nullptr)))
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
		hr = code && out ? CreatePs(dev, code, out, &aoKind, &aoWhy) : o_CreatePixelShader(dev, code, out);
	if (SUCCEEDED(hr) && out && *out)
	{
		uint32_t h = HashShader(code);
		if (aoKind == 2)
		{
			LONG n = InterlockedIncrement(&g_aoMatched);
			if (n == 1 || n % 50 == 0)
				LogInfo("Ambient occlusion: %ld opaque shaders given the indirect light output so far.", n);
		}
		else if (aoKind == 1)
		{
			LONG n = InterlockedIncrement(&g_aoZero);
			if (n <= 30)
				LogInfo("Ambient occlusion: shader %08X gets no AO (%s).", h, aoWhy.c_str());
		}
		Lock l;
		g_psHash[*out] = h;
		// The address may belong to a shader the game released earlier: drop its stale data.
		auto it = g_softOriginal.find(*out);
		if (it != g_softOriginal.end())
		{
			it->second.Release();
			g_softOriginal.erase(it);
		}
		g_gameCode.erase(*out);
		g_aoOutput[*out] = aoKind != 0;
		core::Scene_OnCreatePixelShader(*out, code);
		Fx_OnCreatePixelShader(*out, code);
		if (haveSet)
		{
			g_softOriginal[*out] = set;
			g_softAny = true;
		}
		if (g_dumpEnabled && (haveSet || aoKind))
		{
			std::vector<DWORD>& keep = g_gameCode[*out];
			for (size_t i = 0; i < (1u << 20); i++)
			{
				keep.push_back(code[i]);
				if (code[i] == 0x0000FFFF)
					break;
			}
		}
	}
	return hr;
}

static HRESULT __stdcall h_CreateVertexShader(IDirect3DDevice9* dev, const DWORD* code, IDirect3DVertexShader9** out)
{
	FX_PASSTHROUGH(CreateVertexShader, code, out);
	// TAA: shaders that transform geometry get the sub-pixel jitter added to their position.
	std::vector<DWORD> jittered;
	HRESULT hr = E_FAIL;
	if (Fx_RewriteVS(code, jittered))
	{
		hr = o_CreateVertexShader(dev, jittered.data(), out);
		static bool warned = false;
		if (FAILED(hr) && !warned)
		{
			warned = true;
			LogWarn("TAA: the driver rejected a jittered vertex shader (0x%08lX); using the original (logged once).", hr);
		}
	}
	if (FAILED(hr))
		hr = o_CreateVertexShader(dev, code, out);
	Fx_OnCreateVertexShader(SUCCEEDED(hr) && out ? *out : nullptr, code);
	if (SUCCEEDED(hr) && code && out && *out)
	{
		uint32_t h = HashShader(code);
		{
			Lock l;
			g_vsHash[*out] = h;
		}
		core::Scene_OnCreateVertexShader(*out, code);
	}
	return hr;
}

static HRESULT __stdcall h_SetVertexShader(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs)
{
	FX_PASSTHROUGH(SetVertexShader, vs);
	g_vs = vs;
	HRESULT hr = o_SetVertexShader(dev, vs);
	if (SUCCEEDED(hr))
		Fx_AfterSetVertexShader(vs);
	return hr;
}

static HRESULT __stdcall h_SetPixelShader(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps)
{
	FX_PASSTHROUGH(SetPixelShader, ps);
	g_ps = ps;
	Fx_AfterSetPixelShader(ps);
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
	H(GetRenderTargetData);
	H(SetSamplerState);
	H(SetVertexShaderConstantF);
	H(DrawPrimitive);
	H(DrawIndexedPrimitive);
	H(DrawPrimitiveUP);
	H(DrawIndexedPrimitiveUP);
	H(CreateVertexShader);
	H(SetVertexShader);
	H(CreatePixelShader);
	H(SetPixelShader);
#undef H
	// The effects draw their passes through these, below the hooks.
	core::DeviceFns fx;
	fx.Reset = o_Reset;
	fx.CreateTexture = o_CreateTexture;
	fx.StretchRect = o_StretchRect;
	fx.ColorFill = o_ColorFill;
	fx.SetRenderTarget = o_SetRenderTarget;
	fx.SetDepthStencilSurface = o_SetDepthStencilSurface;
	fx.SetViewport = o_SetViewport;
	fx.SetTexture = o_SetTexture;
	fx.SetSamplerState = o_SetSamplerState;
	fx.DrawPrimitiveUP = o_DrawPrimitiveUP;
	fx.CreateVertexShader = o_CreateVertexShader;
	fx.SetVertexShader = o_SetVertexShader;
	fx.SetVertexShaderConstantF = o_SetVertexShaderConstantF;
	fx.CreatePixelShader = o_CreatePixelShader;
	fx.SetPixelShader = o_SetPixelShader;
	core::SetDeviceFns(fx);
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

	// What the GPU offers for effects that need the scene depth (INTZ) and extra render targets (MRT).
	D3DADAPTER_IDENTIFIER9 id = {};
	d3d->GetAdapterIdentifier(adapter, 0, &id);
	D3DDISPLAYMODE mode = {};
	d3d->GetAdapterDisplayMode(adapter, &mode);
	D3DFORMAT adapterFmt = pp && !pp->Windowed && pp->BackBufferFormat != D3DFMT_UNKNOWN ? pp->BackBufferFormat : mode.Format;
	if (adapterFmt == D3DFMT_UNKNOWN)
		adapterFmt = D3DFMT_X8R8G8B8;
	g_intzSupported = SUCCEEDED(d3d->CheckDeviceFormat(adapter, type, adapterFmt, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, kFmtINTZ));
	D3DCAPS9 caps;
	memset(&caps, 0, sizeof(caps));
	(*out)->GetDeviceCaps(&caps);
	snprintf(buf, sizeof(buf),
		"GPU: %.200s (vendor 0x%04lX device 0x%04lX) | INTZ depth textures: %s | MRT: %lu targets, independent bit depths %s, "
		"post-blend %s | PS %lu.%lu, max PS instructions executed %lu | readable depth (FrameDumpDepth): %s",
		id.Description, id.VendorId, id.DeviceId, g_intzSupported ? "yes" : "NO", caps.NumSimultaneousRTs,
		(caps.PrimitiveMiscCaps & D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS) ? "yes" : "no",
		(caps.PrimitiveMiscCaps & D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING) ? "yes" : "no", (caps.PixelShaderVersion >> 8) & 0xFF,
		caps.PixelShaderVersion & 0xFF, caps.MaxPixelShader30InstructionSlots ? caps.MaxPShaderInstructionsExecuted : 0,
		!g_readableDepth ? "off" : g_intzSupported ? "on" : "unavailable");
	g_deviceInfo += "\n";
	g_deviceInfo += buf;
	LogInfo("%s", buf);
	HookDevice(*out);
	AfterDeviceReady(*out);
	Fx_OnDeviceCreated(d3d, adapter, *out, flags, pp);
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

bool FrameDump_Available() { return g_dumpEnabled && o_Direct3DCreate9; }
bool FrameDump_Busy() { return g_dumpRequested || g_capturing; }
void FrameDump_Request()
{
	if (FrameDump_Available())
		g_dumpRequested = true;
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
	g_dumpEnabled = dump;
	g_dumpKey = dump ? IniInt("Debug", "FrameDumpKey", 0) : 0;
	if (g_dumpKey > 0 && g_dumpKey < 256)
		LogInfo("Frame dump hotkey: 0x%02X.", g_dumpKey);
	g_readableDepth = dump && IniInt("Debug", "FrameDumpDepth", 1) != 0;
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
		LogInfo("Frame dump ready: choose \"Frame dump\" in the menu to write AC2ShadowOverhaul_FrameDump_*.txt (%d frame(s)).", g_framesToCapture);
	else
		LogInfo("Direct3D hooks installed (needed by soft shadows).");
}
