// Debug menu: live tuning of the shadow settings.
//
// When [DebugMenu] Enabled=1, a list of settings is drawn in the top-right corner of the screen.
// Number keys 1..7 select a setting (drawn in red), Up/Down change it: scales by 0.5, whole-number
// settings by 1, on/off settings switch. Holding an arrow repeats. Every change is written to
// AC2Shadows.log so the values can be copied into the ini. The menu key hides/shows the menu
// (and stops it from reacting to keys while hidden).
//
// Text is drawn with ID3DXFont from the d3dx9_42.dll the game already loads, just before Present.

#include "common.h"
#include "softshadows.h"
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

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
static volatile bool g_visible = true;
static int g_menuKey = VK_F9;
static int g_selected = 0;

enum
{
	ITEM_DISTANCE,
	ITEM_SPLIT,
	ITEM_CASTER,
	ITEM_DRAW,
	ITEM_SOFT_WIDTH,
	ITEM_SOFT_ON,
	ITEM_TWEAKS_ON,
	ITEM_COUNT
};
static const char* const kNames[ITEM_COUNT] = {
	"Shadow distance", "Cascade splits", "Caster distance", "Detail draw distance", "Soft shadow width", "Soft shadows",
	"Shadow tweaks (F7)",
};
static const char* const kIniKeys[ITEM_COUNT] = {
	"[Shadows] ShadowDistanceScale", "[Shadows] ShadowCascadeSplitScale", "[Shadows] ShadowCasterDistanceScale",
	"[Shadows] DetailDrawDistanceScale", "[SoftShadows] FilterWidth", "[SoftShadows] StartEnabled", "[Shadows] StartEnabled",
};

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
		if (!SoftShadows_Enabled())
			snprintf(buf, n, "off in ini");
		else
			snprintf(buf, n, "%d (%d taps)", SoftShadows_LiveWidth(), (SoftShadows_LiveWidth() + 1) * (SoftShadows_LiveWidth() + 1));
		break;
	case ITEM_SOFT_ON:
		snprintf(buf, n, "%s", !SoftShadows_Enabled() ? "off in ini" : SoftShadows_Active() ? "ON" : "OFF");
		break;
	default:
		snprintf(buf, n, "%s", !Shadows_Installed() ? "n/a" : Shadows_TweaksOn() ? "ON" : "OFF");
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
		if (!SoftShadows_Enabled())
			return;
		SoftShadows_SetLiveWidth(SoftShadows_LiveWidth() + dir); // clamped to 1..4
		break;
	case ITEM_SOFT_ON:
		if (!SoftShadows_Enabled())
			return;
		SoftShadows_SetActive(dir > 0);
		break;
	default:
		if (!Shadows_Installed())
			return;
		Shadows_SetTweaksOn(dir > 0);
		break;
	}
	char value[48];
	FormatValue(item, value, sizeof(value));
	LogInfo("Tuning: %s = %s   (ini: %s)", kNames[item], value, kIniKeys[item]);
}

// ---- Input -------------------------------------------------------------------------------------

static bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static void PollInput()
{
	static bool menuWas = false;
	static bool numWas[ITEM_COUNT] = {};
	static int heldDir = 0;
	static DWORD heldSince = 0, lastRepeat = 0;
	if (!GameHasFocus())
	{
		heldDir = 0;
		return;
	}
	bool menu = g_menuKey > 0 && KeyDown(g_menuKey);
	if (menu && !menuWas)
		g_visible = !g_visible;
	menuWas = menu;
	if (!g_visible)
	{
		heldDir = 0;
		return;
	}
	for (int i = 0; i < ITEM_COUNT; i++)
	{
		bool down = KeyDown('1' + i) || KeyDown(VK_NUMPAD1 + i);
		if (down && !numWas[i])
			g_selected = i;
		numWas[i] = down;
	}
	int dir = KeyDown(VK_UP) ? 1 : KeyDown(VK_DOWN) ? -1 : 0;
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
	HRESULT hr = g_createFont ? g_createFont(dev, (INT)height, 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
										ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas", &g_font)
							  : E_FAIL;
	if (FAILED(hr) || !g_font)
	{
		g_fontFailed = true;
		g_font = nullptr;
		LogWarn("Debug menu: could not create a D3DX font; the menu can't be drawn (keys still work, see the log).");
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
		DrawLine(y, right, "AC2Shadows tuning  (1-7 select, Up/Down change)", D3DCOLOR_XRGB(200, 200, 200));
		y += step;
		for (int i = 0; i < ITEM_COUNT; i++)
		{
			char value[48], line[128];
			FormatValue(i, value, sizeof(value));
			snprintf(line, sizeof(line), "%d  %s   %s", i + 1, kNames[i], value);
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

bool Tuning_Init()
{
	g_enabled = IniInt("DebugMenu", "Enabled", 0) != 0;
	if (!g_enabled)
		return false;
	g_menuKey = IniInt("DebugMenu", "ToggleKey", VK_F9);
	g_visible = IniInt("DebugMenu", "StartVisible", 1) != 0;
	SoftShadows_SetBuildAllWidths(true);
	LogInfo("Debug menu enabled (key 0x%02X shows/hides it; 1-7 select, Up/Down change).", g_menuKey);
	return true;
}
