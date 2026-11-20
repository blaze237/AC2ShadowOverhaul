// In-game menu: live tuning of the shadow settings, drawn in the top-right corner of the screen.
//
// The menu key ([Menu] MenuKey, default F9) shows / hides it. While shown: Up / Down or the number
// keys 1-9, 0 select a setting (drawn in red), Left / Right change it (scales by 0.5, whole-number
// settings by 1, on/off settings switch; holding repeats). Changes are saved to the ini a second
// after the last key press (and at exit) and logged. While hidden it ignores every key.
// (The same layout and keys as the other AC2 plugins' menus.)
//
// Text is drawn with ID3DXFont from the d3dx9_42.dll the game already loads, just before Present.

#include "common.h"
#include "softshadows.h"
#include "ao.h"
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <string>

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
typedef HRESULT(WINAPI* PFN_D3DXCreateFontA)(IDirect3DDevice9*, INT, UINT, UINT, UINT, BOOL, DWORD, DWORD, DWORD, DWORD, LPCSTR,
	D3DXFont**);

static PFN_D3DXCreateFontA g_createFont;
static D3DXFont* g_font;
static UINT g_fontHeight;
static bool g_fontFailed;

// ---- Settings ------------------------------------------------------------------------------------

static bool g_enabled = false;
static volatile bool g_visible = false;
static int g_menuKey = VK_F9;
static int g_selected = 0;

enum
{
	ITEM_DISTANCE,
	ITEM_SPLIT,
	ITEM_CASTER,
	ITEM_DRAW,
	ITEM_SOFT_WIDTH,
	ITEM_AO_MODE,
	ITEM_AO_RADIUS,
	ITEM_AO_STRENGTH,
	ITEM_CS_ON,
	ITEM_CS_LENGTH,
	ITEM_ALL_ON,     // master switch for every effect; not saved
	ITEM_FRAME_DUMP, // writes a frame dump ([Debug] FrameDump=1 only); not a setting
	ITEM_COUNT
};
static const char* const kNames[ITEM_COUNT] = {
	"Shadow distance", "Cascade splits", "Caster distance", "Detail draw distance", "Soft shadow width",
	"Ambient occlusion", "AO radius", "AO strength", "Contact shadows", "Contact shadow length", "All effects", "Frame dump",
};
static const char* const kIniSections[ITEM_COUNT] = {
	"Shadows", "Shadows", "Shadows", "Shadows", "SoftShadows", "AmbientOcclusion", "AmbientOcclusion", "AmbientOcclusion",
	"ContactShadows", "ContactShadows", nullptr, nullptr,
};
static const char* const kIniKeys[ITEM_COUNT] = {
	"ShadowDistanceScale", "ShadowCascadeSplitScale", "ShadowCasterDistanceScale", "DetailDrawDistanceScale", "FilterWidth",
	"DebugView", "Radius", "Strength", "StartEnabled", "Length", nullptr, nullptr,
};

// Items in the menu: the frame dump entry only when [Debug] FrameDump=1.
static int ItemCount() { return FrameDump_Available() ? ITEM_COUNT : ITEM_COUNT - 1; }

// Soft shadow width as the menu shows it: 0 = off (the game's own filter), else 1..4.
static int SoftWidth() { return SoftShadows_Active() ? SoftShadows_LiveWidth() : 0; }

// Ambient occlusion menu modes: off, on, then the debug views (AO, the indirect light buffer, contact
// shadows, the sun light buffer).
static const char* const kAoModes[] = { "OFF", "ON", "show AO only", "show indirect light", "show contact shadows only", "show sun light" };
static int AoMode() { return AO_View() ? AO_View() + 1 : AO_Active() ? 1 : 0; }

// "All effects": off switches the distance tweaks, soft shadows, AO (and its debug view) and contact
// shadows off together, for before/after comparisons; on puts back whatever was on before.
static bool g_allOn = true;
static struct
{
	bool tweaks, soft, ao, cs;
	int view;
} g_allSaved;

static void SetAllOn(bool on)
{
	if (on == g_allOn)
		return;
	g_allOn = on;
	if (!on)
	{
		g_allSaved = { Shadows_TweaksOn(), SoftShadows_Active(), AO_Active(), CS_Active(), AO_View() };
		if (Shadows_Installed())
			Shadows_SetTweaksOn(false);
		SoftShadows_SetActive(false);
		if (AO_Enabled())
		{
			AO_SetView(0);
			AO_SetOn(false);
			CS_SetOn(false);
		}
	}
	else
	{
		if (Shadows_Installed())
			Shadows_SetTweaksOn(g_allSaved.tweaks);
		SoftShadows_SetActive(g_allSaved.soft);
		if (AO_Enabled())
		{
			AO_SetOn(g_allSaved.ao);
			AO_SetView(g_allSaved.view);
			CS_SetOn(g_allSaved.cs);
		}
	}
}

// Saving: changes are written to the ini a second after the last key press, so holding a key
// doesn't rewrite the file on every repeat.
static unsigned g_dirty; // bit per item
static DWORD g_lastChange;

static void IniValue(int item, char* buf, size_t n)
{
	switch (item)
	{
	case ITEM_DISTANCE:
	case ITEM_SPLIT:
	case ITEM_CASTER:
	case ITEM_DRAW:
		snprintf(buf, n, "%.1f", Shadows_GetSetting(item));
		break;
	case ITEM_SOFT_WIDTH:
		snprintf(buf, n, "%d", SoftWidth());
		break;
	case ITEM_AO_MODE:
		snprintf(buf, n, "%d", AO_View());
		break;
	case ITEM_AO_RADIUS:
		snprintf(buf, n, "%.2f", AO_Radius());
		break;
	case ITEM_AO_STRENGTH:
		snprintf(buf, n, "%.2f", AO_Strength());
		break;
	case ITEM_CS_ON:
		snprintf(buf, n, "%d", CS_Active() ? 1 : 0);
		break;
	case ITEM_CS_LENGTH:
		snprintf(buf, n, "%.2f", CS_Length());
		break;
	default:
		buf[0] = 0;
		break;
	}
}

static void SaveDirty()
{
	for (int i = 0; i < ITEM_COUNT; i++)
	{
		if (!(g_dirty & (1u << i)) || !kIniSections[i])
			continue;
		char v[32];
		IniValue(i, v, sizeof(v));
		if (IniWrite(kIniSections[i], kIniKeys[i], v))
			LogInfo("Menu: saved [%s] %s=%s", kIniSections[i], kIniKeys[i], v);
		else
			LogWarn("Menu: could not write [%s] %s=%s to %s (error %lu)", kIniSections[i], kIniKeys[i], v, IniPath(),
				GetLastError());
		if (i == ITEM_AO_MODE)
			IniWrite("AmbientOcclusion", "StartEnabled", AO_Active() ? "1" : "0");
	}
	g_dirty = 0;
}

static void FormatValue(int item, char* buf, size_t n)
{
	switch (item)
	{
	case ITEM_DISTANCE:
	case ITEM_SPLIT:
	case ITEM_CASTER:
	case ITEM_DRAW:
		if (!Shadows_Installed())
			snprintf(buf, n, "n/a");
		else
			snprintf(buf, n, "x%.1f", Shadows_GetSetting(item));
		break;
	case ITEM_SOFT_WIDTH:
		if (SoftWidth())
			snprintf(buf, n, "%d (%d taps)", SoftWidth(), (SoftWidth() + 1) * (SoftWidth() + 1));
		else
			snprintf(buf, n, "OFF (game's filter)");
		break;
	case ITEM_AO_MODE:
		snprintf(buf, n, "%s", !AO_Enabled() ? "n/a (Enabled=0)" : kAoModes[AoMode()]);
		break;
	case ITEM_AO_RADIUS:
		snprintf(buf, n, "%.2f m", AO_Radius());
		break;
	case ITEM_AO_STRENGTH:
		snprintf(buf, n, "%.2f", AO_Strength());
		break;
	case ITEM_CS_ON:
		snprintf(buf, n, "%s", !AO_Enabled() ? "n/a (Enabled=0)" : CS_Active() ? "ON" : "OFF");
		break;
	case ITEM_CS_LENGTH:
		snprintf(buf, n, "%.2f m", CS_Length());
		break;
	case ITEM_ALL_ON:
		snprintf(buf, n, "%s", g_allOn ? "ON" : "OFF (vanilla)");
		break;
	default:
		snprintf(buf, n, "%s", FrameDump_Busy() ? "writing..." : "Right writes one");
		break;
	}
}

static void Change(int item, int dir)
{
	switch (item)
	{
	case ITEM_DISTANCE:
	case ITEM_SPLIT:
	case ITEM_CASTER:
	case ITEM_DRAW:
	{
		if (!Shadows_Installed())
			return;
		float v = Shadows_GetSetting(item) + 0.5f * dir;
		v = v < 0.5f ? 0.5f : v > 20.0f ? 20.0f : v;
		Shadows_SetSetting(item, v);
		break;
	}
	case ITEM_SOFT_WIDTH:
	{
		int w = SoftWidth() + dir;
		w = w < 0 ? 0 : w > 4 ? 4 : w;
		SoftShadows_SetActive(w > 0);
		if (w > 0)
			SoftShadows_SetLiveWidth(w);
		break;
	}
	case ITEM_AO_MODE:
	{
		if (!AO_Enabled())
			return;
		int mode = AoMode() + dir;
		mode = mode < 0 ? 0 : mode > 5 ? 5 : mode;
		AO_SetOn(mode >= 1);
		AO_SetView(mode >= 2 ? mode - 1 : 0);
		break;
	}
	case ITEM_AO_RADIUS:
		AO_SetRadius(AO_Radius() + 0.25f * dir);
		break;
	case ITEM_AO_STRENGTH:
		AO_SetStrength(AO_Strength() + 0.25f * dir);
		break;
	case ITEM_CS_ON:
		if (!AO_Enabled())
			return;
		CS_SetOn(dir > 0);
		break;
	case ITEM_CS_LENGTH:
		// 0.05 m steps up to half a metre, 0.25 m above
		CS_SetLength(CS_Length() + ((dir > 0 ? CS_Length() < 0.499f : CS_Length() < 0.501f) ? 0.05f : 0.25f) * dir);
		break;
	case ITEM_ALL_ON:
		SetAllOn(dir > 0);
		break;
	default:
		if (dir > 0)
		{
			LogInfo("Menu: frame dump requested.");
			FrameDump_Request();
		}
		return;
	}
	char value[48];
	FormatValue(item, value, sizeof(value));
	if (!kIniSections[item])
	{
		LogInfo("Menu: %s = %s   (not saved)", kNames[item], value);
		return;
	}
	LogInfo("Menu: %s = %s   (ini: [%s] %s)", kNames[item], value, kIniSections[item], kIniKeys[item]);
	g_dirty |= 1u << item;
	g_lastChange = GetTickCount();
}

// ---- Input -------------------------------------------------------------------------------------

static bool KeyDown(int vk) { return vk > 0 && vk < 256 && (GetAsyncKeyState(vk) & 0x8000) != 0; }

// "F9" for the function keys, else the virtual-key code.
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
		LogInfo("Menu %s.", g_visible ? "shown" : "hidden");
	}
	menuWas = menu;
	const int count = ItemCount();
	if (g_selected >= count)
		g_selected = 0;
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
		if (down && !numWas[i] && i < count)
			g_selected = i;
		numWas[i] = down;
	}
	bool up = KeyDown(VK_UP), dn = KeyDown(VK_DOWN);
	if (up && !upWas)
		g_selected = (g_selected + count - 1) % count;
	if (dn && !downWas)
		g_selected = (g_selected + 1) % count;
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
	else if (dir && g_selected != ITEM_FRAME_DUMP && now - heldSince > 400 && now - lastRepeat > 120)
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
	HRESULT hr = g_createFont ? g_createFont(dev, (INT)height, 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
										ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas", &g_font)
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

void Tuning_OnLostDevice()
{
	if (g_font)
		g_font->vt->OnLostDevice(g_font);
}

void Tuning_OnResetDevice()
{
	if (g_font)
		g_font->vt->OnResetDevice(g_font);
}

static void DrawLine(int y, int right, const char* text, D3DCOLOR color)
{
	int shadow = (int)g_fontHeight / 12 + 1;
	RECT r = { 0, y + shadow, right + shadow, y + (int)g_fontHeight * 2 };
	g_font->vt->DrawTextA(g_font, nullptr, text, -1, &r, DT_RIGHT | DT_TOP | DT_NOCLIP | DT_SINGLELINE, D3DCOLOR_ARGB(200, 0, 0, 0));
	r = { 0, y, right, y + (int)g_fontHeight * 2 };
	g_font->vt->DrawTextA(g_font, nullptr, text, -1, &r, DT_RIGHT | DT_TOP | DT_NOCLIP | DT_SINGLELINE, color);
}

void Tuning_OnPresent(IDirect3DDevice9* dev, unsigned bbWidth, unsigned bbHeight)
{
	PollInput();
	if (!g_visible || !bbWidth || !bbHeight || !EnsureFont(dev, bbHeight))
		return;

	// Draw on the back buffer with a full viewport; restore the game's target and viewport after.
	IDirect3DSurface9 *bb = nullptr, *oldRt = nullptr;
	D3DVIEWPORT9 oldVp;
	if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
		return;
	dev->GetRenderTarget(0, &oldRt);
	dev->GetViewport(&oldVp);
	dev->SetRenderTarget(0, bb);
	D3DVIEWPORT9 vp = { 0, 0, bbWidth, bbHeight, 0.0f, 1.0f };
	dev->SetViewport(&vp);
	if (SUCCEEDED(dev->BeginScene()))
	{
		int right = (int)bbWidth - (int)g_fontHeight;
		int y = (int)g_fontHeight / 2;
		int step = (int)(g_fontHeight * 1.25f);
		char line[128], key[16];
		MenuKeyName(g_menuKey, key, sizeof(key));
		DrawLine(y, right, "AC2 Shadow Overhaul", D3DCOLOR_XRGB(200, 200, 200));
		y += step;
		snprintf(line, sizeof(line), "Up/Down or 1-0 select, Left/Right change, %s close", key);
		DrawLine(y, right, line, D3DCOLOR_XRGB(160, 160, 160));
		y += step;
		for (int i = 0; i < ItemCount(); i++)
		{
			char value[48];
			FormatValue(i, value, sizeof(value));
			snprintf(line, sizeof(line), "%s  %s   %s", i < 9 ? std::to_string(i + 1).c_str() : i == 9 ? "0" : " ", kNames[i], value);
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

bool Tuning_Enabled() { return g_enabled; }

void Tuning_Flush()
{
	if (g_dirty)
		SaveDirty();
}

bool Tuning_Init()
{
	g_menuKey = IniInt("Menu", "MenuKey", VK_F9);
	g_enabled = g_menuKey > 0 && g_menuKey < 256;
	if (!g_enabled)
	{
		LogInfo("Menu: off ([Menu] MenuKey=0).");
		return false;
	}
	g_visible = IniInt("Menu", "StartVisible", 0) != 0;
	SoftShadows_SetBuildAllWidths(true);
	char key[16];
	MenuKeyName(g_menuKey, key, sizeof(key));
	LogInfo("Menu: %s shows/hides it; Up/Down or 1-0 select, Left/Right change; changes are saved to the ini.", key);
	return true;
}
