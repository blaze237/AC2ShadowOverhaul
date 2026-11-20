// Shared by the end-to-end tests (tests/<feature>/e2e_test.cpp and tests/combined_test.cpp). Each is
// built as AssassinsCreedIIGame.exe next to the mock d3d9.dll (tests/mockd3d) and loads
// scripts\AC2ShadowOverhaul.asi, built by the top-level build.sh, with an ini written here.
//
//  - E2E_WriteIni: the shipped AC2ShadowOverhaul.ini with logging on, every menu hidden at start, the
//    frame dump off, and each effect group switched on or off ([...] Enabled).
//  - E2E_LoadPlugin: loads the .asi like Ultimate ASI Loader and fakes its keyboard and focus (the
//    menus poll GetAsyncKeyState at Present; set g_keys) and hides D3DXCreateFontA from it (the mock
//    device can't back a D3DX font, so the menus and the MSAA warning draw nothing but still log).
//  - E2E_Effect: which effect drew a recorded draw, from its pixel shader's bytecode.
//  - E2E_Log / E2E_LogCount: the plugin's log, AC2ShadowOverhaul.log.
#pragma once
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "mockd3d/mock_api.h"
#include "shaders_gen.h"      // ambient occlusion + contact shadows (src/shaders_gen.h)
#include "ssgi/shaders_gen.h"
#include "vol/shaders_gen.h"
#include "taa/shaders_gen.h"

// ---- The ini -------------------------------------------------------------------------------------

#define E2E_INI ".\\scripts\\AC2ShadowOverhaul.ini"
#define E2E_LOG "scripts\\AC2ShadowOverhaul.log"
#define E2E_ASI "scripts\\AC2ShadowOverhaul.asi"

struct E2EEffects
{
	bool ao = false;   // [AmbientOcclusion] and [ContactShadows]
	bool ssgi = false; // [SSGI]
	bool vol = false;  // [VolumetricFog] and [SunShafts]
	bool taa = false;  // [TAA]
	static E2EEffects All()
	{
		E2EEffects e;
		e.ao = e.ssgi = e.vol = e.taa = true;
		return e;
	}
};

// Copies the shipped ini (the repo root is two or three folders up from the test's folder) to
// scripts\ and switches the effect groups. Also deletes the last run's log.
static bool E2E_WriteIni(const E2EEffects& on)
{
	CreateDirectoryA("scripts", nullptr);
	bool copied = false;
	for (const char* src : { "..\\..\\..\\AC2ShadowOverhaul.ini", "..\\..\\AC2ShadowOverhaul.ini" })
		if (!copied && GetFileAttributesA(src) != INVALID_FILE_ATTRIBUTES)
			copied = CopyFileA(src, E2E_INI, FALSE) != 0;
	if (!copied)
	{
		printf("FAIL  could not find the shipped AC2ShadowOverhaul.ini\n");
		return false;
	}
	auto set = [](const char* section, const char* key, const char* v) { WritePrivateProfileStringA(section, key, v, E2E_INI); };
	auto onOff = [](bool b) { return b ? "1" : "0"; };
	set("AmbientOcclusion", "Enabled", onOff(on.ao));
	set("ContactShadows", "Enabled", onOff(on.ao));
	set("SSGI", "Enabled", onOff(on.ssgi));
	set("VolumetricFog", "Enabled", onOff(on.vol));
	set("SunShafts", "Enabled", onOff(on.vol));
	set("TAA", "Enabled", onOff(on.taa));
	set("Debug", "EnableLogging", "1");
	set("Debug", "FrameDump", "0");
	set("Debug", "FrameDumpKey", "0");
	set("Menu", "StartVisible", "0");
	DeleteFileA(E2E_LOG);
	return true;
}

static void E2E_SetIni(const char* section, const char* key, const char* value) { WritePrivateProfileStringA(section, key, value, E2E_INI); }

// ---- Faked input ---------------------------------------------------------------------------------

static volatile bool g_keys[256];
static HWND g_wnd;
static SHORT WINAPI E2E_GetAsyncKeyState(int vk) { return vk > 0 && vk < 256 && g_keys[vk] ? (SHORT)0x8000 : 0; }
static HWND WINAPI E2E_GetForegroundWindow() { return g_wnd; }
static FARPROC WINAPI E2E_GetProcAddress(HMODULE m, LPCSTR name)
{
	if ((uintptr_t)name > 0xFFFF && strcmp(name, "D3DXCreateFontA") == 0)
		return nullptr;
	return GetProcAddress(m, name);
}

// The import address table slot of dll!func in `mod`, or nullptr.
static void** E2E_ImportSlot(HMODULE mod, const char* dll, const char* func)
{
	unsigned char* base = (unsigned char*)mod;
	auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
	auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir.VirtualAddress)
		return nullptr;
	for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++)
	{
		if (dll && _stricmp((const char*)(base + imp->Name), dll) != 0)
			continue;
		auto names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
		auto iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
		for (; names->u1.AddressOfData; names++, iat++)
		{
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
				continue;
			if (strcmp((const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name, func) == 0)
				return (void**)&iat->u1.Function;
		}
	}
	return nullptr;
}

static bool E2E_PatchImport(HMODULE mod, const char* func, void* repl)
{
	void** slot = E2E_ImportSlot(mod, nullptr, func);
	if (!slot)
		return false;
	DWORD old;
	VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
	*slot = repl;
	VirtualProtect(slot, sizeof(void*), old, &old);
	return true;
}

// Loads the plugin and fakes its input. Creates the test's "game window" (the fake foreground window).
static HMODULE E2E_LoadPlugin()
{
	HMODULE m = LoadLibraryA(E2E_ASI);
	if (!m)
	{
		printf("FAIL  could not load %s\n", E2E_ASI);
		return nullptr;
	}
	E2E_PatchImport(m, "GetAsyncKeyState", (void*)E2E_GetAsyncKeyState);
	E2E_PatchImport(m, "GetForegroundWindow", (void*)E2E_GetForegroundWindow);
	E2E_PatchImport(m, "GetProcAddress", (void*)E2E_GetProcAddress);
	if (!g_wnd)
		g_wnd = CreateWindowA("STATIC", "e2e", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, NULL, NULL, NULL, NULL);
	return m;
}

// ---- The plugin's log ----------------------------------------------------------------------------

static std::string E2E_Log()
{
	std::string text;
	if (FILE* f = fopen(E2E_LOG, "rb"))
	{
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
			text.append(buf, n);
		fclose(f);
	}
	return text;
}

// Lines in the log containing `what` (occurrences).
static int E2E_LogCount(const char* what)
{
	std::string text = E2E_Log();
	int count = 0;
	for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1))
		count++;
	return count;
}

// ---- Which effect drew -----------------------------------------------------------------------------

enum E2EEffect
{
	FX_NONE, // the game (or anything else)
	FX_AO,   // ambient occlusion + contact shadows
	FX_SSGI,
	FX_VOL,
	FX_TAA,
	FX_COUNT
};
static const char* const kE2EEffectNames[FX_COUNT] = { "game", "AO", "SSGI", "volumetrics", "TAA" };

struct E2EShader
{
	const DWORD* code;
	size_t size;
	E2EEffect effect;
	const char* name;
};
static const E2EShader kE2EShaders[] = {
	{ kAoGtaoPs, sizeof(kAoGtaoPs), FX_AO, "gtao" },
	{ kAoBlurPs, sizeof(kAoBlurPs), FX_AO, "ao-blur" },
	{ kAoCompositePs, sizeof(kAoCompositePs), FX_AO, "ao-composite" },
	{ ssgi::kGiPrepPs, sizeof(ssgi::kGiPrepPs), FX_SSGI, "gi-prep" },
	{ ssgi::kGiTracePs, sizeof(ssgi::kGiTracePs), FX_SSGI, "gi-trace" },
	{ ssgi::kGiTemporalPs, sizeof(ssgi::kGiTemporalPs), FX_SSGI, "gi-temporal" },
	{ ssgi::kGiBlurPs, sizeof(ssgi::kGiBlurPs), FX_SSGI, "gi-blur" },
	{ ssgi::kGiDebugPs, sizeof(ssgi::kGiDebugPs), FX_SSGI, "gi-debug" },
	{ vol::kVolFogMarchPs, sizeof(vol::kVolFogMarchPs), FX_VOL, "march" },
	{ vol::kVolFogBlurPs, sizeof(vol::kVolFogBlurPs), FX_VOL, "blur" },
	{ vol::kVolShaftsMaskPs, sizeof(vol::kVolShaftsMaskPs), FX_VOL, "mask" },
	{ vol::kVolShaftsBlurPs, sizeof(vol::kVolShaftsBlurPs), FX_VOL, "radial" },
	{ vol::kVolCompositePs, sizeof(vol::kVolCompositePs), FX_VOL, "composite" },
	{ taa::kTaaResolvePs, sizeof(taa::kTaaResolvePs), FX_TAA, "taa-resolve" },
	{ taa::kTaaOutputPs, sizeof(taa::kTaaOutputPs), FX_TAA, "taa-output" },
};

static const E2EShader* E2E_Shader(const MockDraw& d)
{
	if (!d.psCode)
		return nullptr;
	for (const E2EShader& s : kE2EShaders)
		if (!memcmp(d.psCode, s.code, s.size))
			return &s;
	return nullptr;
}
static E2EEffect E2E_Effect(const MockDraw& d)
{
	const E2EShader* s = E2E_Shader(d);
	return s ? s->effect : FX_NONE;
}
static const char* E2E_PassName(const MockDraw& d)
{
	const E2EShader* s = E2E_Shader(d);
	return s ? s->name : nullptr;
}

// Effect passes among draws [from, to) of the log, per effect.
struct E2EPassCount
{
	int n[FX_COUNT] = {};
	int Effects() const { return n[FX_AO] + n[FX_SSGI] + n[FX_VOL] + n[FX_TAA]; }
};
static E2EPassCount E2E_CountPasses(const MockLog* log, size_t from, size_t to)
{
	E2EPassCount c;
	for (size_t i = from; i < to && i < log->draws.size(); i++)
		c.n[E2E_Effect(log->draws[i])]++;
	return c;
}
