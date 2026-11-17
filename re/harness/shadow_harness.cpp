// Offline test: maps the real AssassinsCreedIIGame.exe (UPlay) into this 32-bit process without
// running it, installs the shadow hooks via the real signature scanner, and calls the game's own
// cascade builder / visibility-distance setter with synthetic inputs, patched vs. unpatched.
#include "../../AC2Shadows/src/common.cpp"
#include "../../AC2Shadows/src/shadows.cpp"
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <math.h>

static uintptr_t g_delta = 0; // actual base - preferred base
#define GAME(a) ((uintptr_t)(a) + g_delta)

static LONG CALLBACK Veh(EXCEPTION_POINTERS* e)
{
	if (e->ExceptionRecord->ExceptionCode < 0xC0000000) // e.g. OutputDebugString
		return EXCEPTION_CONTINUE_SEARCH;
	printf("EXCEPTION 0x%08lX at %p (game va %p)\n", e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress,
		(void*)((uintptr_t)e->ExceptionRecord->ExceptionAddress - g_delta));
	fflush(stdout);
	ExitProcess(2);
	return EXCEPTION_CONTINUE_SEARCH;
}

static unsigned char* Alloc(size_t n)
{
	unsigned char* p = (unsigned char*)_aligned_malloc(n, 16);
	memset(p, 0, n);
	return p;
}

struct CascadeResult
{
	float rmin[4], rmax[4];
	float nearFar[2];
};

static CascadeResult RunCascades(const float splits[4])
{
	unsigned char* P = Alloc(0x800);   // light component; settings object R = P + 0x24
	unsigned char* E = Alloc(0x100);   // light entity, [E+0x40] = P
	unsigned char* V = Alloc(0x200);   // view
	unsigned char* O = Alloc(0x800);   // output (ranges / matrices)
	unsigned char* Z = Alloc(0x40);    // empty handle
	unsigned char* R = P + 0x24;

	*(unsigned char**)(E + 0x40) = P;
	*(unsigned char**)(R + 0x8) = Z;
	P[0x630] = 1; // use P+0x10 as light direction
	float dir[4] = { 0.3f, -0.8f, 0.5f, 0.0f };
	memcpy(P + 0x10, dir, 16);
	float vdir[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
	memcpy(V + 0x10, vdir, 16);
	float pos[4] = { 10.0f, 2.0f, 20.0f, 1.0f };
	memcpy(V + 0x30, pos, 16);
	*(float*)(V + 0x48) = 1000.0f;
	for (int i = 0; i < 4; i++)
		*(float*)(R + 0x51C + 4 * i) = splits[i];
	*(float*)(R + 0x530) = splits[3] - 10.0f;
	*(float*)(R + 0x534) = splits[3] - 1.0f;

	auto fn = (void(__cdecl*)(void*, void*, void*, void*))GAME(0x158C5D0);
	fn(E, V, nullptr, O);

	CascadeResult r;
	memcpy(r.rmin, O + 0xD0, 16);
	memcpy(r.rmax, O + 0xE0, 16);
	memcpy(r.nearFar, O + 0x80, 8);
	return r;
}

static void Print(const char* tag, const CascadeResult& r)
{
	printf("%s  RangesMin = %.2f %.2f %.2f %.2f | RangesMax = %.2f %.2f %.2f %.2f | NearFar = %g %g\n", tag,
		r.rmin[0], r.rmin[1], r.rmin[2], r.rmin[3], r.rmax[0], r.rmax[1], r.rmax[2], r.rmax[3], r.nearFar[0], r.nearFar[1]);
}

static void RunVis(const char* tag)
{
	unsigned char* vis = Alloc(0x400);
	unsigned char* s = Alloc(0x100);
	s[0x98] = 1;
	*(float*)(s + 0x88) = 50.0f;
	*(float*)(s + 0x90) = 70.0f;
	*(float*)(s + 0x8C) = 15.0f;
	*(float*)(s + 0x94) = 30.0f;
	auto fn = (void(__fastcall*)(void*, void*, void*))GAME(0x166D060); // thiscall-compatible
	fn(vis, nullptr, s);
	float* o = (float*)(vis + 0x2F4);
	printf("%s  vis draw small/medium = %g %g | shadow small/medium = %g %g\n", tag, o[0], o[1], o[2], o[3]);
}

// Calls the game's render list append (0x15A7FC0) 5 times into each of four lists of capacity 2.
// Returns true when every list stopped at its capacity and the canary past the buffer is intact.
static bool RunListAppend(const char* tag)
{
	// Each case targets two lists of the same group (capacity 2 each), so a skipped entry in the
	// first list must still leave the loop in a state that appends correctly to the second.
	unsigned char* C = Alloc(0x800);
	const unsigned offs[4][2] = { { 0x0, 0x18 }, { 0x240, 0x258 }, { 0x2D0, 0x360 }, { 0x510, 0x5A0 } };
	const char* names[4] = { "main", "+0x240", "sun cascade", "cascade set 2" };
	unsigned char* bufs[4][2];
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 2; j++)
		{
			unsigned char* b = bufs[i][j] = Alloc(12 * 8);
			memset(b + 24, 0xCC, 12 * 6);
			*(unsigned char**)(C + offs[i][j]) = b;
			*(uint32_t*)(C + offs[i][j] + 4) = 0;
			*(uint32_t*)(C + offs[i][j] + 8) = 2 | 0x80000000;
		}
	// flags in obj+0x20, pass mask, a9 ([ebp+28h]), a10 ([ebp+2Ch]); obj+0x94 = sun cascade mask
	struct { uint32_t flags, passMask, a9, a10; } cases[4] = { { 1, 3, 0, 0 }, { 4, 3, 1, 0 }, { 2, 0x40, 0, 1 }, { 8 | 16, 0, 0, 1 } };
	auto fn = (void(__fastcall*)(void*, void*, uint32_t, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
		uint32_t, uint32_t, uint32_t))GAME(0x15A7FC0);
	bool ok = true;
	for (int i = 0; i < 4; i++)
	{
		unsigned char* obj = Alloc(0x100);
		*(uint32_t*)(obj + 0x20) = cases[i].flags;
		obj[0x94] = 3;
		for (int n = 0; n < 5; n++)
			fn(C, nullptr, 0x1234, obj, 7, cases[i].passMask, 0, 0, 0, 0, cases[i].a9, cases[i].a10, 0, 0);
		for (int j = 0; j < 2; j++)
		{
			uint32_t count = *(uint32_t*)(C + offs[i][j] + 4);
			bool canary = true;
			for (int k = 24; k < 24 + 12 * 6; k++)
				canary &= bufs[i][j][k] == 0xCC;
			bool entries = *(unsigned char**)bufs[i][j] == obj && *(unsigned char**)(bufs[i][j] + 12) == obj;
			printf("%s  list %-13s #%d count %u, canary %s, entries %s\n", tag, names[i], j, count, canary ? "intact" : "OVERWRITTEN",
				entries ? "ok" : "MISSING");
			ok &= count == 2 && canary && entries;
		}
	}
	return ok;
}

static float CallFadeStub(void* stub, unsigned char* settings)
{
	float result;
	__asm
	{
		push esi
		mov esi, settings
		call stub
		fstp result
		pop esi
	}
	return result;
}

int main(int argc, char** argv)
{
	const char* exe = argc > 1 ? argv[1] : "AssassinsCreedIIGame.exe";
	AddVectoredExceptionHandler(1, Veh);
	LogInit("harness.log", true);
	IniSetPath(argc > 2 ? argv[2] : "Z:\nonexistent\none.ini");

	HMODULE game = LoadLibraryExA(exe, NULL, DONT_RESOLVE_DLL_REFERENCES);
	if (!game)
	{
		printf("LoadLibraryEx failed: %lu\n", GetLastError());
		return 1;
	}
	g_delta = (uintptr_t)game - 0x400000;
	printf("Game image mapped at %p (delta 0x%X)\n", (void*)game, (unsigned)g_delta);
	{
		// The loader skips relocations for an EXE mapped this way; apply them by hand.
		auto dos = (IMAGE_DOS_HEADER*)game;
		auto nt = (IMAGE_NT_HEADERS*)((unsigned char*)game + dos->e_lfanew);
		DWORD old;
		VirtualProtect(game, nt->OptionalHeader.SizeOfImage, PAGE_EXECUTE_READWRITE, &old);
		if (g_delta && nt->OptionalHeader.ImageBase == 0x400000)
		{
			auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
			unsigned char* p = (unsigned char*)game + dir.VirtualAddress;
			unsigned char* end = p + dir.Size;
			int n = 0;
			while (p < end)
			{
				auto blk = (IMAGE_BASE_RELOCATION*)p;
				if (!blk->SizeOfBlock)
					break;
				WORD* e = (WORD*)(blk + 1);
				int cnt = (blk->SizeOfBlock - sizeof(*blk)) / 2;
				for (int i = 0; i < cnt; i++)
					if ((e[i] >> 12) == IMAGE_REL_BASED_HIGHLOW)
					{
						*(uint32_t*)((unsigned char*)game + blk->VirtualAddress + (e[i] & 0xFFF)) += (uint32_t)g_delta;
						n++;
					}
				p += blk->SizeOfBlock;
			}
			printf("Applied %d relocations\n", n);
		}
	}
	if (*(uint32_t*)GAME(0x00414A54 + 1) != 0x01CA6FC8 + g_delta)
		printf("WARNING: UPlay version check bytes differ\n");

	const float splits[4] = { 5.0f, 15.0f, 40.0f, 80.0f };
	CascadeResult before = RunCascades(splits);
	Print("vanilla ", before);
	RunVis("vanilla ");
	RunListAppend("vanilla ");

	Shadows_Init(game);

	CascadeResult after = RunCascades(splits);
	Print("patched ", after);
	RunVis("patched ");
	bool listsOk = RunListAppend("patched ");
	printf("render list guard: %s (dropped main %ld, +0x240 %ld, sun %ld, set2 %ld; expect 6 each)\n", listsOk ? "PASS" : "FAIL", g_droppedEntries[0],
		g_droppedEntries[1], g_droppedEntries[2], g_droppedEntries[3]);
	{
		const uintptr_t caps[8] = { 0x16B67F9, 0x16B6806, 0x16B6813, 0x16B6820, 0x16B687B, 0x16B6888, 0x16B6895, 0x16B68A2 };
		bool capsOk = true;
		for (uintptr_t c : caps)
			capsOk &= *(uint32_t*)GAME(c) == 0x3FFF;
		printf("shadow list capacities 0x3FFF: %s\n", capsOk ? "PASS" : "FAIL");
		listsOk &= capsOk;
	}

	unsigned char* s = Alloc(0x600);
	*(float*)(s + 0x530) = 70.0f;
	*(float*)(s + 0x534) = 79.0f;
	*(float*)(s + 0x52C) = 80.0f;
	printf("stubs    fadeStart %g fadeEnd %g depthRange %g\n", CallFadeStub((void*)SunShadowFadeStart_Hook, s),
		CallFadeStub((void*)SunShadowFadeEnd_Hook, s), CallFadeStub((void*)SunShadowDepthRange_Hook, s));

	// Confirm the call sites in the caller now point at the stubs.
	auto target = [](uintptr_t site) { return site + 5 + *(int32_t*)(site + 1); };
	printf("site 0x15EE16C -> %s\n", target(GAME(0x15EE16C)) == (uintptr_t)SunShadowFadeStart_Hook ? "FadeStart stub OK" : "WRONG");
	printf("site 0x15EE17D -> %s\n", target(GAME(0x15EE17D)) == (uintptr_t)SunShadowFadeEnd_Hook ? "FadeEnd stub OK" : "WRONG");
	printf("site 0x15EE1D5 -> %s\n", target(GAME(0x15EE1D5)) == (uintptr_t)SunShadowDepthRange_Hook ? "DepthRange stub OK" : "WRONG");

	ApplyShadowTweaks(false);
	CascadeResult off = RunCascades(splits);
	Print("toggled off", off);
	RunVis("toggled off");
	ApplyShadowTweaks(true);
	CascadeResult on = RunCascades(splits);
	Print("toggled on ", on);
	bool toggleOk = memcmp(&off, &before, sizeof(off)) == 0 && memcmp(&on, &after, sizeof(on)) == 0;
	printf("toggle off == vanilla, on == patched: %s\n", toggleOk ? "PASS" : "FAIL");

	bool ok = toggleOk && listsOk;
	// Defaults (no ini): far cascade edge x3 (ShadowDistanceScale), inner splits x1.5
	// (ShadowCascadeSplitScale). RangesMax = far, s1, s2, s3; RangesMin = s1, s2, s3, 0.
	for (int i = 0; i < 4; i++)
	{
		float maxScale = i == 0 ? 3.0f : 1.5f;
		if (fabsf(after.rmax[i] - before.rmax[i] * maxScale) > 0.01f || fabsf(after.rmin[i] - before.rmin[i] * 1.5f) > 0.01f)
			ok = false;
	}
	printf("cascade ranges scaled (far x3, inner splits x1.5): %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 3;
}
