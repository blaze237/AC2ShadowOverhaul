// Shadow distance
//
// AC2 limits shadows near the camera in two independent ways:
//
// 1) Sun shadow cascades. Each frame the renderer builds 4 cascades from the sun light's
//    shadow settings object (data driven, engine defaults 4/16/64/400 m):
//      +0x51C..+0x528  cascade split distances, nearest to farthest
//      +0x52C          shadow depth range (re-copied from the far split every frame)
//      +0x530/+0x534   fade-out start / end
//      +0x550..+0x58C  per-cascade biases; the depth bias is normalised by +0x52C
//    The cascade builder (0x158C5D0 on UPlay) copies the splits and depth range into stack
//    locals before building the ortho projections and the cascade range shader constants, so
//    we scale those locals rather than the data (which would compound every frame). The fade
//    and bias normalisation are read separately in the caller (0x15EE0D0) and get the same
//    scale, so the fade moves out with the far cascade and world-space biases are preserved.
//
// 2) Detail object culling. The visibility test (0x15B1710) drops the shadow-caster flag of
//    objects tagged as small/medium detail beyond vis+0x2FC / +0x300 (15/30 m at max
//    ShadowQuality), and culls them entirely beyond vis+0x2F4 / +0x2F8 (50/70 m draw distance).
//    Both are copied in by 0x166D060 every frame. This is what makes parts of buildings start
//    casting shadows along a line that sweeps across the screen as the camera moves.
//
// 3) Shadow caster list capacity. Each frame the visibility jobs append every shadow caster to
//    a per-cascade render list (12-byte entries). The lists are allocated once with a fixed
//    capacity (2500 / 2049 / 1536 / 1537 entries, in the render list container constructor
//    0x16B6560) and the append (0x15A7FC0) does an atomic count++ with no bounds check. Pushing
//    the cascades out collects more casters than that, the writes run off the end of the buffer
//    and the shadow render jobs crash (seen at 0x1602B43). We enlarge the four cascade lists and
//    make the append loops in 0x15A7FC0 drop entries instead of overflowing.
//
// All sites are located by byte signature so a mismatched executable is skipped safely.

#include "common.h"
#include <float.h>
#include <string.h>

static float g_ShadowDistanceScale = 1.0f; // far cascade end, depth range and fade-out
static float g_ShadowSplitScale = 1.0f;    // three inner cascade boundaries
static float g_ShadowCasterScale = 1.0f;   // small/medium object shadow caster cut-off
static float g_DetailDrawScale = 1.0f;     // small/medium object draw distance (also caps their shadows)
static uintptr_t g_SunShadowCascades_Return = 0;
static void ReportDroppedListEntries();

// Configured scales, so the hotkey can switch between them and vanilla (1.0). The hooks read the
// globals above every frame, so a change takes effect on the next frame.
static float g_cfgDistance = 1.0f, g_cfgSplit = 1.0f, g_cfgCaster = 1.0f, g_cfgDraw = 1.0f;
static volatile bool g_shadowTweaksOn = true;

static void ApplyShadowTweaks(bool on)
{
	g_shadowTweaksOn = on;
	g_ShadowDistanceScale = on ? g_cfgDistance : 1.0f;
	g_ShadowSplitScale = on ? g_cfgSplit : 1.0f;
	g_ShadowCasterScale = on ? g_cfgCaster : 1.0f;
	g_DetailDrawScale = on ? g_cfgDraw : 1.0f;
}

static int g_toggleKey = 0;

static DWORD WINAPI ToggleThread(void*)
{
	bool wasDown = false;
	for (;;)
	{
		Sleep(30);
		bool down = (GetAsyncKeyState(g_toggleKey) & 0x8000) != 0;
		if (down && !wasDown && GameHasFocus())
		{
			bool on = !g_shadowTweaksOn;
			ApplyShadowTweaks(on);
			LogInfo("Shadow tweaks toggled %s.", on ? "ON" : "OFF (vanilla)");
			Beep(on ? 880 : 440, 120); // high = on, low = vanilla
		}
		wasDown = down;
	}
	return 0;
}

static float ClampScale(float scale, float fallback)
{
	if (!(scale > 0.0f)) // also catches NaN
		return fallback;
	if (scale < 0.25f)
		return 0.25f;
	if (scale > 20.0f)
		return 20.0f;
	return scale;
}

// frame = ebp of the cascade builder. Its locals:
//   [ebp-0x60] far split (end of cascade 0), [ebp-0x5C], [ebp-0x58], [ebp-0x54] inner splits
//   (descending), [ebp-0x34] near plane of the shadow depth range (-0.5 * depth range).
static void __cdecl ScaleSunShadowCascades(uintptr_t frame)
{
	float* splits = (float*)(frame - 0x60);
	float* depthNear = (float*)(frame - 0x34);

	ReportDroppedListEntries();
	float original[4] = { splits[0], splits[1], splits[2], splits[3] };

	splits[0] *= g_ShadowDistanceScale;
	for (int i = 1; i < 4; i++)
	{
		float v = splits[i] * g_ShadowSplitScale;
		if (v > splits[i - 1]) // keep cascades ordered if the inner scale exceeds the outer one
			v = splits[i - 1];
		splits[i] = v;
	}
	*depthNear *= g_ShadowDistanceScale;

	// Log the data-driven values whenever they change (they differ per area / time of day).
	static float lastLoggedFar = -1.0f;
	static int logCount = 0;
	if (original[0] != lastLoggedFar && logCount < 32 && LogEnabled())
	{
		lastLoggedFar = original[0];
		logCount++;
		LogInfo("Sun shadow cascades: %.1f / %.1f / %.1f / %.1f m -> %.1f / %.1f / %.1f / %.1f m",
			original[3], original[2], original[1], original[0], splits[3], splits[2], splits[1], splits[0]);
	}
}

// Replaces "mov esi, [ebx+14h]; mov eax, [ebx+10h]" right after the builder has loaded
// its split / depth locals. xmm0 holds the depth range about to be stored to [ebp-0x2C].
ASM(SunShadowCascades_Hook)
{
	__asm
	{
		test esi, esi // shadow settings object (always present in practice)
		jz _noSettings

		sub esp, 0x80
		movups [esp], xmm0
		movups [esp + 0x10], xmm1
		movups [esp + 0x20], xmm2
		movups [esp + 0x30], xmm3
		movups [esp + 0x40], xmm4
		movups [esp + 0x50], xmm5
		movups [esp + 0x60], xmm6
		movups [esp + 0x70], xmm7
		pushad
		push ebp
		call ScaleSunShadowCascades
		add esp, 4
		popad
		movups xmm0, [esp]
		movups xmm1, [esp + 0x10]
		movups xmm2, [esp + 0x20]
		movups xmm3, [esp + 0x30]
		movups xmm4, [esp + 0x40]
		movups xmm5, [esp + 0x50]
		movups xmm6, [esp + 0x60]
		movups xmm7, [esp + 0x70]
		add esp, 0x80

		mulss xmm0, dword ptr [g_ShadowDistanceScale]

	_noSettings:
		mov esi, [ebx + 0x14]
		mov eax, [ebx + 0x10]
		jmp dword ptr [g_SunShadowCascades_Return]
	}
}

// Each replaces a 6-byte "fld dword ptr [esi+xxx]" (esi = sun shadow settings) with a call.
ASM(SunShadowFadeStart_Hook)
{
	__asm
	{
		fld dword ptr [esi + 0x530]
		fmul dword ptr [g_ShadowDistanceScale]
		ret
	}
}

ASM(SunShadowFadeEnd_Hook)
{
	__asm
	{
		fld dword ptr [esi + 0x534]
		fmul dword ptr [g_ShadowDistanceScale]
		ret
	}
}

ASM(SunShadowDepthRange_Hook)
{
	__asm
	{
		fld dword ptr [esi + 0x52C]
		fmul dword ptr [g_ShadowDistanceScale]
		ret
	}
}

// Full replacement for the thiscall that copies the per-category visibility distances from
// the render settings into a visibility query (0x166D060 on UPlay):
//   vis+0x2F4 / +0x2F8  small / medium object draw distance   (x g_DetailDrawScale)
//   vis+0x2FC / +0x300  small / medium object shadow distance (x g_ShadowCasterScale)
// The visibility test culls an object entirely (including its shadow) beyond its draw distance
// before the shadow distance is even checked, so the draw distance also caps how far the shadow
// cut-off can usefully be pushed (vanilla max quality: 50/70 m draw, 15/30 m shadow).
static void __fastcall SetVisibilityDistances_Hook(uint8_t* vis, void* /*edx*/, const uint8_t* settings)
{
	float* out = (float*)(vis + 0x2F4);
	if (settings[0x98])
	{
		float drawSmall = *(const float*)(settings + 0x88);
		float drawMedium = *(const float*)(settings + 0x90);
		float shadowSmall = *(const float*)(settings + 0x8C);
		float shadowMedium = *(const float*)(settings + 0x94);
		out[0] = drawSmall * g_DetailDrawScale;
		out[1] = drawMedium * g_DetailDrawScale;
		out[2] = shadowSmall * g_ShadowCasterScale;
		out[3] = shadowMedium * g_ShadowCasterScale;

		static float lastDrawSmall = -1.0f, lastShadowSmall = -1.0f;
		static int logCount = 0;
		if ((drawSmall != lastDrawSmall || shadowSmall != lastShadowSmall) && logCount < 16 && LogEnabled())
		{
			lastDrawSmall = drawSmall;
			lastShadowSmall = shadowSmall;
			logCount++;
			LogInfo("Detail object distances: draw %.0f / %.0f m -> %.0f / %.0f m, shadow %.0f / %.0f m -> %.0f / %.0f m",
				drawSmall, drawMedium, out[0], out[1], shadowSmall, shadowMedium, out[2], out[3]);
		}
	}
	else
	{
		out[0] = out[1] = out[2] = out[3] = FLT_MAX;
	}
}

// ---- Render list overflow guard ---------------------------------------------------------------
// Render list: +0 entries, +4 count (atomically incremented by appenders), +8 capacity | 0x80000000.
// The append function 0x15A7FC0 (UPlay) has four append loops, one per list group:
//   main pass lists (container + (layer*6 + pass) * 0x18), lists at +0x240 (6 passes),
//   sun shadow cascade lists at +0x2D0 + i*0x90 and a second cascade set at +0x510 + i*0x90.
static const unsigned kShadowListCapacity = 0x3FFF; // stays clear of any 14/16-bit packing
static volatile LONG g_droppedEntries[4] = {};
static uintptr_t g_guardContinue[4] = {}, g_guardSkip[4] = {};

// Main pass loop. Replaces "lea eax,[edx+4]; mov ecx,eax; mov eax,1; lock xadd [ecx],eax; inc eax"
// (edx = list). On overflow it skips the entry write and resumes at the loop tail, which expects
// edx = [ebp-0x10] (the container) and reloads eax itself.
ASM(ListGuardMain_Hook)
{
	__asm
	{
		mov eax, 1
		lock xadd dword ptr [edx + 4], eax
		inc eax
		mov ecx, dword ptr [edx + 8]
		and ecx, 0x7FFFFFFF
		cmp eax, ecx
		ja _overflow
		jmp dword ptr [g_guardContinue]
	_overflow:
		lock dec dword ptr [edx + 4]
		lock inc dword ptr [g_droppedEntries]
		mov edx, dword ptr [ebp - 0x10]
		jmp dword ptr [g_guardSkip]
	}
}

// The other three loops. Each replaces "lea ecx,[edx+4]; mov eax,1; lock xadd [ecx],eax; inc eax"
// (edx = list, kept across the loop); the skip target is the loop tail, which reloads eax/ecx.
#define LIST_GUARD(name, i)                                    \
	ASM(name)                                                  \
	{                                                          \
		__asm mov eax, 1                                       \
		__asm lock xadd dword ptr [edx + 4], eax               \
		__asm inc eax                                          \
		__asm mov ecx, dword ptr [edx + 8]                     \
		__asm and ecx, 0x7FFFFFFF                              \
		__asm cmp eax, ecx                                     \
		__asm ja name##_overflow                               \
		__asm jmp dword ptr [g_guardContinue + 4 * i]          \
		__asm name##_overflow:                                 \
		__asm lock dec dword ptr [edx + 4]                     \
		__asm lock inc dword ptr [g_droppedEntries + 4 * i]    \
		__asm jmp dword ptr [g_guardSkip + 4 * i]              \
	}
LIST_GUARD(ListGuard1_Hook, 1)
LIST_GUARD(ListGuard2_Hook, 2)

// Cascade set 2: the loop tail starts with the last two entry writes, so skip past them and
// load the loop counter the tail expects in ecx.
ASM(ListGuard3_Hook)
{
	__asm
	{
		mov eax, 1
		lock xadd dword ptr [edx + 4], eax
		inc eax
		mov ecx, dword ptr [edx + 8]
		and ecx, 0x7FFFFFFF
		cmp eax, ecx
		ja _overflow
		jmp dword ptr [g_guardContinue + 12]
	_overflow:
		lock dec dword ptr [edx + 4]
		lock inc dword ptr [g_droppedEntries + 12]
		mov ecx, dword ptr [ebp + 0x20]
		jmp dword ptr [g_guardSkip + 12]
	}
}

static void ReportDroppedListEntries()
{
	static DWORD lastTick = 0;
	static int reports = 0;
	if (reports >= 20 || !LogEnabled())
		return;
	DWORD now = GetTickCount();
	if (now - lastTick < 5000)
		return;
	if (!g_droppedEntries[0] && !g_droppedEntries[1] && !g_droppedEntries[2] && !g_droppedEntries[3])
		return;
	lastTick = now;
	reports++;
	LONG d[4];
	for (int i = 0; i < 4; i++)
		d[i] = InterlockedExchange(&g_droppedEntries[i], 0);
	LogWarn("Render lists full, entries skipped since last report: sun shadow %ld, shadow set 2 %ld, main %ld, other %ld. "
		"Some distant objects/shadows were not drawn; lower the distance scales if this keeps appearing.", d[2], d[3], d[0], d[1]);
}

static void PatchRenderLists(HMODULE patternModule)
{
	uintptr_t a = FindUniquePattern("render list append (main)",
		"8D 04 40 8D 04 47 8D 0C 40 8D 14 CA 8D 42 04 8B C8 B8 01 00 00 00 F0 0F C1 01 40 8B 0A", patternModule);
	uintptr_t b = FindUniquePattern("render list append (shadow)",
		"C7 45 24 00 00 00 00 81 C2 D0 02 00 00 83 7D 34 00 74 6C F6 45 34 01 74 4F 8D 4A 04 B8 01 00 00 00 F0 0F C1 01 40 8B 0A",
		patternModule);

	// Sites relative to the signatures (UPlay: a = 0x15A8044, b = 0x15A8104), each checked
	// against the bytes it was written for before anything is patched.
	static const uint8_t kAppend[] = { 0x8D, 0x4A, 0x04, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xF0, 0x0F, 0xC1, 0x01, 0x40 };
	struct Site
	{
		uintptr_t site, cont, skip;
		uint8_t skipBytes[3];
		const void* hook;
	} sites[4] = {
		{ a + 0x0C, a + 0x1B, a + 0x67, { 0x8B, 0x45, 0x18 }, (const void*)ListGuardMain_Hook }, // main passes
		{ b + 0xD4, b + 0xE1, b + 0x120, { 0x8B, 0x45, 0x34 }, (const void*)ListGuard1_Hook },  // +0x240 lists
		{ b + 0x19, b + 0x26, b + 0x65, { 0x8B, 0x45, 0x18 }, (const void*)ListGuard2_Hook },   // sun cascades
		{ b + 0x18A, b + 0x197, b + 0x1D9, { 0x83, 0xC1, 0x01 }, (const void*)ListGuard3_Hook }, // cascade set 2
	};
	bool ok = a && b;
	for (int i = 0; ok && i < 4; i++)
	{
		const Site& st = sites[i];
		ok = memcmp((void*)st.skip, st.skipBytes, 3) == 0 &&
			(i == 0 ? memcmp((void*)st.site, "\x8D\x42\x04\x8B\xC8\xB8\x01\x00\x00\x00\xF0\x0F\xC1\x01\x40", 15) == 0
					: memcmp((void*)st.site, kAppend, sizeof(kAppend)) == 0);
	}
	if (ok)
	{
		for (int i = 0; i < 4; i++)
		{
			g_guardContinue[i] = sites[i].cont;
			g_guardSkip[i] = sites[i].skip;
			InjectHook(sites[i].site, sites[i].hook, PATCH_JUMP);
			Nop(sites[i].site + 5, sites[i].cont - sites[i].site - 5);
		}
		LogInfo("Render list overflow guard installed.");
	}
	else
	{
		LogWarn("Render list overflow guard skipped (signatures not found). Large distance scales may crash.");
	}

	// Capacities pushed by the list container constructor (0x16B6560): sun cascades
	// 2500 / 2049 / 1536 / 1537, second cascade set 1097 x4.
	uintptr_t caps1 = FindUniquePattern("shadow list capacities",
		"8B 4D C8 68 C4 09 00 00 E8 ?? ?? ?? ?? 8B 4D C4 68 01 08 00 00 E8 ?? ?? ?? ?? 8B 4D C0 68 00 06 00 00 E8 ?? ?? ?? ?? "
		"8B 4D BC 68 01 06 00 00 E8", patternModule);
	uintptr_t caps2 = FindUniquePattern("shadow list capacities 2",
		"8B 4D A0 68 49 04 00 00 E8 ?? ?? ?? ?? 8B 4D 9C 68 49 04 00 00 E8 ?? ?? ?? ?? 8B 4D 98 68 49 04 00 00 E8 ?? ?? ?? ?? "
		"8B 4D 94 68 49 04 00 00 E8", patternModule);
	uint32_t v = kShadowListCapacity;
	for (int i = 0; i < 4; i++)
	{
		if (caps1)
			WriteMemory(caps1 + 4 + 13 * i, &v, sizeof(v));
		if (caps2)
			WriteMemory(caps2 + 4 + 13 * i, &v, sizeof(v));
	}
	if (caps1 && caps2)
		LogInfo("Shadow caster lists enlarged to %u entries per cascade.", kShadowListCapacity);
	else
		LogWarn("Shadow caster list enlargement skipped (signature not found).");
}

// ---- Live tuning (debug menu) ------------------------------------------------------------------

static bool g_shadowHooksInstalled = false;

bool Shadows_Installed() { return g_shadowHooksInstalled; }
bool Shadows_TweaksOn() { return g_shadowTweaksOn; }
void Shadows_SetTweaksOn(bool on) { ApplyShadowTweaks(on); }

float Shadows_GetSetting(int which)
{
	switch (which)
	{
	case 0: return g_cfgDistance;
	case 1: return g_cfgSplit;
	case 2: return g_cfgCaster;
	default: return g_cfgDraw;
	}
}

void Shadows_SetSetting(int which, float value)
{
	value = ClampScale(value, 1.0f);
	switch (which)
	{
	case 0: g_cfgDistance = value; break;
	case 1: g_cfgSplit = value; break;
	case 2: g_cfgCaster = value; break;
	default: g_cfgDraw = value; break;
	}
	ApplyShadowTweaks(g_shadowTweaksOn); // the hooks read the globals every frame
}

void Shadows_Init(HMODULE patternModule, bool forceInstall)
{
	const char* S = "Shadows";
	float master = ClampScale(IniFloat(S, "ShadowDistanceScale", 3.0f), 1.0f);
	float split = IniFloat(S, "ShadowCascadeSplitScale", 0.0f);
	float caster = IniFloat(S, "ShadowCasterDistanceScale", 0.0f);
	g_DetailDrawScale = ClampScale(IniFloat(S, "DetailDrawDistanceScale", 2.0f), 1.0f);
	g_ShadowDistanceScale = master;
	g_ShadowSplitScale = (split > 0.0f) ? ClampScale(split, master) : master;
	g_ShadowCasterScale = (caster > 0.0f) ? ClampScale(caster, master) : master;

	if (!forceInstall && g_ShadowDistanceScale == 1.0f && g_ShadowSplitScale == 1.0f && g_ShadowCasterScale == 1.0f &&
		g_DetailDrawScale == 1.0f)
	{
		LogInfo("Shadow distance patches disabled (all scales are 1.0).");
		return;
	}

	PatchRenderLists(patternModule);

	// Cascade builder: the four split loads, the depth range loads, then the two movs we replace.
	uintptr_t cascades = FindUniquePattern("cascade splits",
		"F3 0F 10 8E 1C 05 00 00 EB 08 F3 0F 10 0D ?? ?? ?? ?? 85 F6 F3 0F 11 4D AC 74 0A "
		"F3 0F 10 8E 20 05 00 00 EB 08 F3 0F 10 0D ?? ?? ?? ?? 85 F6 F3 0F 11 4D A8 74 0A "
		"F3 0F 10 8E 24 05 00 00 EB 08 F3 0F 10 0D ?? ?? ?? ?? 85 F6 F3 0F 11 4D A4 74 08 "
		"F3 0F 10 86 28 05 00 00 85 F6 F3 0F 11 45 A0 74 10 D9 86 2C 05 00 00 D9 E0 DC 0D ?? ?? ?? ?? "
		"EB 06 DD 05 ?? ?? ?? ?? 85 F6 D9 5D CC 74 0A F3 0F 10 86 2C 05 00 00 EB 08 F3 0F 10 05 ?? ?? ?? ?? "
		"8B 73 14 8B 43 10", patternModule);
	// Caller: clamp call, then fld fade start / fld fade end.
	uintptr_t fade = FindUniquePattern("shadow fade",
		"8B CE E8 ?? ?? ?? ?? D9 86 30 05 00 00 D9 5D F4 F3 0F 10 05 ?? ?? ?? ?? D9 86 34 05 00 00", patternModule);
	// Caller: bias normalisation by the depth range.
	uintptr_t bias = FindUniquePattern("shadow bias",
		"D9 86 2C 05 00 00 DC 0D ?? ?? ?? ?? DC 3D ?? ?? ?? ?? D9 5D F4", patternModule);
	// Visibility distance copy (whole function).
	uintptr_t visDist = FindUniquePattern("detail object distances",
		"55 8B EC 8B 45 08 80 B8 98 00 00 00 00 74 34 D9 80 88 00 00 00 D9 99 F4 02 00 00 "
		"D9 80 90 00 00 00 D9 99 F8 02 00 00 D9 80 8C 00 00 00 D9 99 FC 02 00 00 D9 80 94 00 00 00 D9 99 00 03 00 00",
		patternModule);

	// The cascade, fade and bias hooks must go in together or the fade/bias would no longer
	// match the cascades.
	if (cascades && fade && bias)
	{
		uintptr_t site = cascades + 0x91;
		g_SunShadowCascades_Return = site + 6;
		InjectHook(site, (const void*)SunShadowCascades_Hook, PATCH_JUMP);
		Nop(site + 5, 1);

		InjectHook(fade + 0x07, (const void*)SunShadowFadeStart_Hook, PATCH_CALL);
		Nop(fade + 0x07 + 5, 1);
		InjectHook(fade + 0x18, (const void*)SunShadowFadeEnd_Hook, PATCH_CALL);
		Nop(fade + 0x18 + 5, 1);

		InjectHook(bias, (const void*)SunShadowDepthRange_Hook, PATCH_CALL);
		Nop(bias + 5, 1);

		LogInfo("Sun shadow cascades patched: far/fade x%.2f, inner splits x%.2f.", g_ShadowDistanceScale, g_ShadowSplitScale);
	}
	else
	{
		LogWarn("Sun shadow cascade patch skipped (signatures not found - already patched by another mod?).");
	}

	if (visDist)
	{
		InjectHook(visDist, (const void*)SetVisibilityDistances_Hook, PATCH_JUMP);
		LogInfo("Detail object distances patched: shadow x%.2f, draw x%.2f.", g_ShadowCasterScale, g_DetailDrawScale);
	}
	else
	{
		LogWarn("Detail object distance patch skipped (signature not found - already patched by another mod?).");
	}

	g_shadowHooksInstalled = (cascades && fade && bias) || visDist;
	g_cfgDistance = g_ShadowDistanceScale;
	g_cfgSplit = g_ShadowSplitScale;
	g_cfgCaster = g_ShadowCasterScale;
	g_cfgDraw = g_DetailDrawScale;
	g_toggleKey = IniInt(S, "ToggleKey", VK_F7);
	if (g_toggleKey > 0 && g_toggleKey < 256 && (cascades || visDist))
	{
		if (!IniInt(S, "StartEnabled", 1))
			ApplyShadowTweaks(false);
		HANDLE t = CreateThread(NULL, 0, ToggleThread, NULL, 0, NULL);
		if (t)
			CloseHandle(t);
		LogInfo("Shadow tweak toggle key: 0x%02X (starting %s).", g_toggleKey, g_shadowTweaksOn ? "on" : "off");
	}
}
