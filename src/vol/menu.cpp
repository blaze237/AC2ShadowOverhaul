// In-game menu: live tuning of the fog and the shafts, drawn in the top-right corner of the screen.
//
// The menu key ([Menu] MenuKey, default F11) shows / hides it. While shown: Up / Down or the number
// keys 1-9, 0 select a setting (drawn in red), Left / Right change it (holding repeats). Changes are
// saved to the ini a second after the last key press (and at exit) and logged. The keys only do
// anything while the menu is shown. (The same layout and keys as the other AC2 plugins' menus.)
//
// Text is drawn with ID3DXFont from the d3dx9_42.dll the game already loads, just before Present.

#include "common.h"
#include "volumetrics.h"
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>

namespace vol
{

// ---- Minimal ID3DXFont ---------------------------------------------------------------------------

struct D3DXFont;
struct D3DXFontVtbl
{
	void* QueryInterface;
	void* AddRef;
	ULONG(__stdcall* Release)(D3DXFont*);
	void* GetDevice;
	void* GetDescA;
	void* GetDescW;
	void* GetTextMetricsA;
	void* GetTextMetricsW;
	void* GetDC;
	void* GetGlyphData;
	void* PreloadCharacters;
	void* PreloadGlyphs;
	void* PreloadTextA;
	void* PreloadTextW;
	INT(__stdcall* DrawTextA)(D3DXFont*, void* sprite, LPCSTR text, INT count, RECT* rect, DWORD format, D3DCOLOR color);
	void* DrawTextW;
	HRESULT(__stdcall* OnLostDevice)(D3DXFont*);
	HRESULT(__stdcall* OnResetDevice)(D3DXFont*);
};
struct D3DXFont
{
	D3DXFontVtbl* vt;
};
typedef HRESULT(WINAPI* PFN_D3DXCreateFontA)(IDirect3DDevice9*, INT, UINT, UINT, UINT, BOOL, DWORD, DWORD, DWORD, DWORD, LPCSTR, D3DXFont**);

static PFN_D3DXCreateFontA g_createFont;
static D3DXFont* g_font;
static UINT g_fontHeight;
static bool g_fontFailed;

// ---- Items -------------------------------------------------------------------------------------

static bool g_enabled = false;
static volatile bool g_visible = false;
static int g_menuKey = VK_F11;
static int g_selected = 0;

enum
{
	ITEM_FOG,
	ITEM_DENSITY,
	ITEM_FALLOFF,
	ITEM_BASE,
	ITEM_SUN,
	ITEM_ANISO,
	ITEM_AMBIENT,
	ITEM_DISTANCE,
	ITEM_STEPS,
	ITEM_SHAFTS,
	ITEM_SHAFT_INTENSITY,
	ITEM_SHAFT_LENGTH,
	ITEM_SHAFT_THRESHOLD,
	ITEM_ALL, // master switch for comparisons; not saved
	ITEM_COUNT
};
static const char* const kNames[ITEM_COUNT] = {
	"Volumetric fog", "Fog density", "Height falloff", "Base height", "Sun scattering (god rays)", "Sun focus",
	"Sky light scattering", "Fog distance", "Ray march steps", "Sun shafts", "Shaft intensity", "Shaft length",
	"Shaft sky threshold", "All volumetric effects",
};
static const char* const kSections[ITEM_COUNT] = {
	"VolumetricFog", "VolumetricFog", "VolumetricFog", "VolumetricFog", "VolumetricFog", "VolumetricFog", "VolumetricFog",
	"VolumetricFog", "VolumetricFog", "SunShafts", "SunShafts", "SunShafts", "SunShafts", nullptr,
};
static const char* const kKeys[ITEM_COUNT] = {
	"StartEnabled", "Density", "HeightFalloff", "BaseHeight", "SunIntensity", "Anisotropy", "AmbientIntensity", "MaxDistance", "Steps",
	"StartEnabled", "Intensity", "Length", "Threshold", nullptr,
};

// Fog modes: off, on, then the debug views. Shaft modes: off, on, shafts only.
static int FogMode() { return g_set.view == 1 ? 2 : g_set.view == 2 ? 3 : g_set.fogOn ? 1 : 0; }
static int ShaftMode() { return g_set.view == 3 ? 2 : g_set.shaftsOn ? 1 : 0; }
static const char* const kFogModes[] = { "OFF", "ON", "show fog only", "show fog transmittance" };
static const char* const kShaftModes[] = { "OFF", "ON", "show shafts only" };

static bool g_allOn = true;
static struct
{
	bool fog, shafts;
	int view;
} g_allSaved;

static void SetAllOn(bool on)
{
	if (on == g_allOn)
		return;
	g_allOn = on;
	if (!on)
	{
		g_allSaved = { g_set.fogOn, g_set.shaftsOn, g_set.view };
		g_set.fogOn = g_set.shaftsOn = false;
		g_set.view = 0;
	}
	else
	{
		g_set.fogOn = g_allSaved.fog;
		g_set.shaftsOn = g_allSaved.shafts;
		g_set.view = g_allSaved.view;
	}
}

// ---- Saving ------------------------------------------------------------------------------------

static unsigned g_dirty;
static DWORD g_lastChange;

static void IniValue(int item, char* buf, size_t n)
{
	switch (item)
	{
	case ITEM_FOG: snprintf(buf, n, "%d", g_set.fogOn ? 1 : 0); break;
	case ITEM_DENSITY: snprintf(buf, n, "%.5f", g_set.density); break;
	case ITEM_FALLOFF: snprintf(buf, n, "%.4f", g_set.heightFalloff); break;
	case ITEM_BASE: snprintf(buf, n, "%.1f", g_set.baseHeight); break;
	case ITEM_SUN: snprintf(buf, n, "%.2f", g_set.sunIntensity); break;
	case ITEM_ANISO: snprintf(buf, n, "%.2f", g_set.anisotropy); break;
	case ITEM_AMBIENT: snprintf(buf, n, "%.2f", g_set.ambientIntensity); break;
	case ITEM_DISTANCE: snprintf(buf, n, "%.0f", g_set.maxDistance); break;
	case ITEM_STEPS: snprintf(buf, n, "%d", g_set.steps); break;
	case ITEM_SHAFTS: snprintf(buf, n, "%d", g_set.shaftsOn ? 1 : 0); break;
	case ITEM_SHAFT_INTENSITY: snprintf(buf, n, "%.2f", g_set.shaftsIntensity); break;
	case ITEM_SHAFT_LENGTH: snprintf(buf, n, "%.2f", g_set.shaftsLength); break;
	case ITEM_SHAFT_THRESHOLD: snprintf(buf, n, "%.2f", g_set.shaftsThreshold); break;
	default: buf[0] = 0; break;
	}
}

static void SaveDirty()
{
	for (int i = 0; i < ITEM_COUNT; i++)
	{
		if (!(g_dirty & (1u << i)) || !kSections[i])
			continue;
		char v[32];
		IniValue(i, v, sizeof(v));
		if (IniWrite(kSections[i], kKeys[i], v))
			LogInfo("Menu: saved [%s] %s=%s", kSections[i], kKeys[i], v);
		else
			LogWarn("Menu: could not write [%s] %s=%s to %s (error %lu)", kSections[i], kKeys[i], v, IniPath(), GetLastError());
		if (i == ITEM_FOG || i == ITEM_SHAFTS)
		{
			char view[8];
			snprintf(view, sizeof(view), "%d", g_set.view);
			IniWrite("VolumetricFog", "DebugView", view);
		}
	}
	g_dirty = 0;
}

static void FormatValue(int item, char* buf, size_t n)
{
	switch (item)
	{
	case ITEM_FOG: snprintf(buf, n, "%s", !g_set.fogConfigured ? "n/a (Enabled=0)" : kFogModes[FogMode()]); break;
	case ITEM_DENSITY: snprintf(buf, n, "%.4f /m", g_set.density); break;
	case ITEM_FALLOFF: snprintf(buf, n, "%.4f /m (halves every %.0f m)", g_set.heightFalloff, g_set.heightFalloff > 0 ? 0.693f / g_set.heightFalloff : 0.0f); break;
	case ITEM_BASE: snprintf(buf, n, "%.0f m", g_set.baseHeight); break;
	case ITEM_SUN: snprintf(buf, n, "%.2f", g_set.sunIntensity); break;
	case ITEM_ANISO: snprintf(buf, n, "%.2f", g_set.anisotropy); break;
	case ITEM_AMBIENT: snprintf(buf, n, "%.2f", g_set.ambientIntensity); break;
	case ITEM_DISTANCE: snprintf(buf, n, "%.0f m", g_set.maxDistance); break;
	case ITEM_STEPS: snprintf(buf, n, "%d", g_set.steps); break;
	case ITEM_SHAFTS: snprintf(buf, n, "%s", !g_set.shaftsConfigured ? "n/a (Enabled=0)" : kShaftModes[ShaftMode()]); break;
	case ITEM_SHAFT_INTENSITY: snprintf(buf, n, "%.2f", g_set.shaftsIntensity); break;
	case ITEM_SHAFT_LENGTH: snprintf(buf, n, "%.2f", g_set.shaftsLength); break;
	case ITEM_SHAFT_THRESHOLD: snprintf(buf, n, "%.2f", g_set.shaftsThreshold); break;
	default: snprintf(buf, n, "%s", g_allOn ? "ON" : "OFF (vanilla)"); break;
	}
}

static void Change(int item, int dir)
{
	switch (item)
	{
	case ITEM_FOG:
	{
		if (!g_set.fogConfigured)
			return;
		int mode = FogMode() + dir;
		mode = mode < 0 ? 0 : mode > 3 ? 3 : mode;
		g_set.fogOn = mode >= 1;
		g_set.view = mode == 2 ? 1 : mode == 3 ? 2 : g_set.view == 3 ? 3 : 0;
		break;
	}
	case ITEM_DENSITY: g_set.density = dir > 0 ? (g_set.density < 1e-4f ? 1e-4f : g_set.density * 1.25f) : g_set.density / 1.25f; break;
	case ITEM_FALLOFF:
		g_set.heightFalloff = dir > 0 ? (g_set.heightFalloff < 1e-3f ? 1e-3f : g_set.heightFalloff * 1.25f)
									  : (g_set.heightFalloff <= 1e-3f ? 0.0f : g_set.heightFalloff / 1.25f);
		break;
	case ITEM_BASE: g_set.baseHeight = g_set.baseHeight + 5.0f * dir; break;
	case ITEM_SUN: g_set.sunIntensity = g_set.sunIntensity + 0.1f * dir; break;
	case ITEM_ANISO: g_set.anisotropy = g_set.anisotropy + 0.05f * dir; break;
	case ITEM_AMBIENT: g_set.ambientIntensity = g_set.ambientIntensity + 0.05f * dir; break;
	case ITEM_DISTANCE: // 25 m steps up to 500 m, 100 m above
		g_set.maxDistance = g_set.maxDistance + ((dir > 0 ? g_set.maxDistance < 499.0f : g_set.maxDistance < 501.0f) ? 25.0f : 100.0f) * dir;
		break;
	case ITEM_STEPS: g_set.steps = g_set.steps + 8 * dir; break;
	case ITEM_SHAFTS:
	{
		if (!g_set.shaftsConfigured)
			return;
		int mode = ShaftMode() + dir;
		mode = mode < 0 ? 0 : mode > 2 ? 2 : mode;
		g_set.shaftsOn = mode >= 1;
		g_set.view = mode == 2 ? 3 : g_set.view == 3 ? 0 : g_set.view;
		break;
	}
	case ITEM_SHAFT_INTENSITY: g_set.shaftsIntensity = g_set.shaftsIntensity + 0.1f * dir; break;
	case ITEM_SHAFT_LENGTH: g_set.shaftsLength = g_set.shaftsLength + 0.05f * dir; break;
	case ITEM_SHAFT_THRESHOLD: g_set.shaftsThreshold = g_set.shaftsThreshold + 0.05f * dir; break;
	default: SetAllOn(dir > 0); break;
	}
	Vol_ClampSettings();
	char value[64];
	FormatValue(item, value, sizeof(value));
	LogInfo("Menu: %s = %s", kNames[item], value);
	if (kSections[item])
	{
		g_dirty |= 1u << item;
		g_lastChange = GetTickCount();
	}
}

// ---- Input -------------------------------------------------------------------------------------

static bool KeyDown(int vk) { return vk > 0 && vk < 256 && (GetAsyncKeyState(vk) & 0x8000) != 0; }

// "F11" for the function keys, else the virtual-key code.
static void MenuKeyName(int vk, char* buf, size_t n)
{
	if (vk >= VK_F1 && vk <= VK_F24)
		snprintf(buf, n, "F%d", vk - VK_F1 + 1);
	else
		snprintf(buf, n, "key 0x%02X", vk);
}

// Menu key; while shown, Up / Down and 1-9, 0 select, Left / Right change (holding repeats).
static void PollInput()
{
	static bool menuWas = false, upWas = false, downWas = false, numWas[10] = {};
	static int heldDir = 0;
	static DWORD heldSince = 0, lastRepeat = 0;
	if (g_dirty && GetTickCount() - g_lastChange > 1000)
		SaveDirty();
	if (!GameHasFocus())
	{
		heldDir = 0;
		return;
	}
	bool menu = KeyDown(g_menuKey);
	if (menu && !menuWas)
	{
		g_visible = !g_visible;
		if (g_visible)
			MenuClaim(MENU_VOLUMETRICS);
		else
			MenuRelease(MENU_VOLUMETRICS);
		LogInfo("Volumetrics menu %s.", g_visible ? "shown" : "hidden");
	}
	menuWas = menu;
	if (g_visible && MenuOwner() != MENU_VOLUMETRICS)
		g_visible = false; // another menu was opened
	if (!g_visible)
	{
		// Keys held while the menu opens don't count until released.
		heldDir = 0;
		upWas = downWas = true;
		for (bool& w : numWas)
			w = true;
		return;
	}
	for (int i = 0; i < 10; i++)
	{
		bool down = KeyDown(i < 9 ? '1' + i : '0') || KeyDown(i < 9 ? VK_NUMPAD1 + i : VK_NUMPAD0);
		if (down && !numWas[i] && i < ITEM_COUNT)
			g_selected = i;
		numWas[i] = down;
	}
	bool up = KeyDown(VK_UP), dn = KeyDown(VK_DOWN);
	if (up && !upWas)
		g_selected = (g_selected + ITEM_COUNT - 1) % ITEM_COUNT;
	if (dn && !downWas)
		g_selected = (g_selected + 1) % ITEM_COUNT;
	upWas = up;
	downWas = dn;
	int dir = KeyDown(VK_RIGHT) ? 1 : KeyDown(VK_LEFT) ? -1 : 0;
	DWORD now = GetTickCount();
	if (dir != heldDir)
	{
		heldDir = dir;
		heldSince = lastRepeat = now;
		if (dir)
			Change(g_selected, dir);
	}
	else if (dir && now - heldSince > 400 && now - lastRepeat > 120)
	{
		lastRepeat = now;
		Change(g_selected, dir);
	}
}

// ---- Drawing -----------------------------------------------------------------------------------

static void ReleaseFont()
{
	if (g_font)
		g_font->vt->Release(g_font);
	g_font = nullptr;
}

static bool EnsureFont(IDirect3DDevice9* dev, UINT bbHeight)
{
	UINT height = bbHeight / 45;
	if (height < 14)
		height = 14;
	if (g_font && height == g_fontHeight)
		return true;
	ReleaseFont();
	if (g_fontFailed)
		return false;
	if (!g_createFont)
	{
		for (const char* d : { "d3dx9_42.dll", "d3dx9_43.dll" })
		{
			HMODULE m = GetModuleHandleA(d);
			if (!m)
				m = LoadLibraryA(d);
			if (m && (g_createFont = (PFN_D3DXCreateFontA)GetProcAddress(m, "D3DXCreateFontA")) != nullptr)
				break;
		}
	}
	HRESULT hr = g_createFont ? g_createFont(dev, (INT)height, 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
									DEFAULT_PITCH | FF_DONTCARE, "Consolas", &g_font)
							  : E_FAIL;
	if (FAILED(hr) || !g_font)
	{
		g_fontFailed = true;
		g_font = nullptr;
		LogWarn("Menu: could not create a D3DX font; the menu can't be drawn (keys still work, see the log).");
		return false;
	}
	g_fontHeight = height;
	return true;
}

void Menu_OnLostDevice()
{
	if (g_font)
		g_font->vt->OnLostDevice(g_font);
}

void Menu_OnResetDevice()
{
	if (g_font)
		g_font->vt->OnResetDevice(g_font);
}

// Right-aligned at `right`.
static void DrawLine(int y, int right, const char* text, D3DCOLOR color)
{
	int shadow = (int)g_fontHeight / 12 + 1;
	RECT r = { 0, y + shadow, right + shadow, y + (int)g_fontHeight * 2 };
	g_font->vt->DrawTextA(g_font, nullptr, text, -1, &r, DT_RIGHT | DT_TOP | DT_NOCLIP | DT_SINGLELINE, D3DCOLOR_ARGB(200, 0, 0, 0));
	r = { 0, y, right, y + (int)g_fontHeight * 2 };
	g_font->vt->DrawTextA(g_font, nullptr, text, -1, &r, DT_RIGHT | DT_TOP | DT_NOCLIP | DT_SINGLELINE, color);
}

void Menu_OnPresent(IDirect3DDevice9* dev)
{
	PollInput();
	if (!g_visible)
		return;
	IDirect3DSurface9 *bb = nullptr, *oldRt = nullptr;
	if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
		return;
	D3DSURFACE_DESC d;
	if (FAILED(bb->GetDesc(&d)) || !EnsureFont(dev, d.Height))
	{
		bb->Release();
		return;
	}
	// Draw on the back buffer with a full viewport; restore the game's target and viewport after.
	D3DVIEWPORT9 oldVp;
	dev->GetRenderTarget(0, &oldRt);
	dev->GetViewport(&oldVp);
	dev->SetRenderTarget(0, bb);
	D3DVIEWPORT9 vp = { 0, 0, d.Width, d.Height, 0.0f, 1.0f };
	dev->SetViewport(&vp);
	if (SUCCEEDED(dev->BeginScene()))
	{
		int right = (int)d.Width - (int)g_fontHeight, y = (int)g_fontHeight / 2;
		int step = (int)(g_fontHeight * 1.25f);
		char line[160], value[64], key[16];
		MenuKeyName(g_menuKey, key, sizeof(key));
		DrawLine(y, right, "AC2 Shadow Overhaul - Volumetrics", D3DCOLOR_XRGB(200, 200, 200));
		y += step;
		snprintf(line, sizeof(line), "Up/Down or 1-0 select, Left/Right change, %s close", key);
		DrawLine(y, right, line, D3DCOLOR_XRGB(160, 160, 160));
		y += step;
		snprintf(line, sizeof(line), "Status: %s", Vol_Status());
		DrawLine(y, right, line, D3DCOLOR_XRGB(255, 220, 120));
		y += step;
		for (int i = 0; i < ITEM_COUNT; i++)
		{
			char num[2] = { i < 9 ? (char)('1' + i) : i == 9 ? '0' : ' ', 0 };
			FormatValue(i, value, sizeof(value));
			snprintf(line, sizeof(line), "%s  %s   %s", num, kNames[i], value);
			DrawLine(y, right, line, i == g_selected ? D3DCOLOR_XRGB(255, 50, 50) : D3DCOLOR_XRGB(255, 255, 255));
			y += step;
		}
		dev->EndScene();
	}
	dev->SetRenderTarget(0, oldRt ? oldRt : bb);
	dev->SetViewport(&oldVp);
	if (oldRt)
		oldRt->Release();
	bb->Release();
}

// ---- Init --------------------------------------------------------------------------------------

void Menu_Flush()
{
	if (g_dirty)
		SaveDirty();
}

bool Menu_Init()
{
	g_menuKey = IniInt("Menu", "VolumetricsMenuKey", VK_F11);
	g_enabled = g_menuKey > 0 && g_menuKey < 256;
	if (!g_enabled)
	{
		LogInfo("Menu: off ([Menu] MenuKey=0).");
		return false;
	}
	g_visible = false; // only the shadow menu can start shown ([Menu] StartVisible)
	char key[16];
	MenuKeyName(g_menuKey, key, sizeof(key));
	LogInfo("Menu: %s shows/hides it; Up/Down or 1-0 select, Left/Right change; changes are saved to the ini.", key);
	return true;
}

} // namespace vol
