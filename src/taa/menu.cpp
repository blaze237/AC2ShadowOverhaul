// In-game settings menu. The menu key ([Menu] MenuKey, default F1) shows / hides it in the top-right
// corner; Up / Down or the number keys 1-9, 0 select a setting, Left / Right change it (holding
// repeats). Changes apply at once and are written to the ini a second after the last key press (and
// at exit). Keys only act while the menu is shown. (The same layout and keys as the other AC2
// plugins' menus.)
//
// Text is drawn with ID3DXFont from the d3dx9_42.dll the game already loads, just before Present.
#include "menu.h"
#include "common.h"
#include "taa.h"
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

namespace taa
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

// ---- Items ---------------------------------------------------------------------------------------

enum
{
	ITEM_ON,
	ITEM_SHARPNESS,
	ITEM_FEEDBACK,
	ITEM_MOTION_FEEDBACK,
	ITEM_MIP_BIAS,
	ITEM_JITTER,
	ITEM_CLIP,
	ITEM_DEPTH_TOL,
	ITEM_DEBUG,
	ITEM_COUNT
};
static const struct
{
	const char* name;
	const char* key; // in [TAA]
	float step;
} kItems[ITEM_COUNT] = {
	{ "TAA", "StartEnabled", 0 },
	{ "Sharpness", "Sharpness", 0.05f },
	{ "Feedback (smoothness)", "Feedback", 0.01f },
	{ "Feedback in motion", "MotionFeedback", 0.01f },
	{ "Texture mip bias", "MipBias", 0.1f },
	{ "Jitter size", "JitterScale", 0.1f },
	{ "Clip box", "ClipBox", 0.05f },
	{ "Depth tolerance", "DepthTolerance", 0.005f },
	{ "Debug view", "DebugView", 0 },
};
static const char* const kDebugViews[] = { "off", "history source", "motion" };

static float* FloatOf(TaaSettings& t, int item)
{
	switch (item)
	{
	case ITEM_SHARPNESS: return &t.sharpness;
	case ITEM_FEEDBACK: return &t.feedback;
	case ITEM_MOTION_FEEDBACK: return &t.motionFeedback;
	case ITEM_MIP_BIAS: return &t.mipBias;
	case ITEM_JITTER: return &t.jitterScale;
	case ITEM_CLIP: return &t.clipBox;
	case ITEM_DEPTH_TOL: return &t.depthTolerance;
	}
	return nullptr;
}

static void FormatValue(const TaaSettings& t, int item, char* buf, size_t n)
{
	if (item == ITEM_ON)
		snprintf(buf, n, "%s", t.on ? "ON" : "OFF");
	else if (item == ITEM_DEBUG)
		snprintf(buf, n, "%s", kDebugViews[t.debugView]);
	else
	{
		TaaSettings c = t;
		float v = *FloatOf(c, item);
		snprintf(buf, n, item == ITEM_DEPTH_TOL ? "%.3f" : "%.2f", v);
	}
}

// Ini text for an item (what FormatValue shows, but plain numbers).
static void IniValue(const TaaSettings& t, int item, char* buf, size_t n)
{
	if (item == ITEM_ON)
		snprintf(buf, n, "%d", t.on ? 1 : 0);
	else if (item == ITEM_DEBUG)
		snprintf(buf, n, "%d", t.debugView);
	else
	{
		TaaSettings c = t;
		snprintf(buf, n, item == ITEM_DEPTH_TOL ? "%.3f" : "%.2f", *FloatOf(c, item));
	}
}

// ---- State and saving ----------------------------------------------------------------------------

static bool g_enabled = false, g_visible = false;
static int g_menuKey = VK_F1, g_selected = 0;
static unsigned g_dirty;  // bit per item
static DWORD g_lastChange;

static void SaveDirty()
{
	TaaSettings t = Taa_GetSettings();
	for (int i = 0; i < ITEM_COUNT; i++)
		if (g_dirty & (1u << i))
		{
			char v[32];
			IniValue(t, i, v, sizeof(v));
			if (!IniWrite("TAA", kItems[i].key, v))
				LogWarn("Menu: could not write %s to the ini.", kItems[i].key);
		}
	g_dirty = 0;
}

static void Change(int item, int dir)
{
	TaaSettings t = Taa_GetSettings();
	if (item == ITEM_ON)
		t.on = dir > 0;
	else if (item == ITEM_DEBUG)
		t.debugView = (t.debugView + dir + 3) % 3;
	else
	{
		float* v = FloatOf(t, item);
		*v = roundf((*v + dir * kItems[item].step) / kItems[item].step) * kItems[item].step; // stay on the step grid
	}
	Taa_SetSettings(t);
	t = Taa_GetSettings(); // clamped
	char v[32];
	FormatValue(t, item, v, sizeof(v));
	LogInfo("Menu: %s = %s", kItems[item].name, v);
	g_dirty |= 1u << item;
	g_lastChange = GetTickCount();
}

// ---- Input ---------------------------------------------------------------------------------------

static bool KeyDown(int vk) { return vk > 0 && vk < 256 && (GetAsyncKeyState(vk) & 0x8000) != 0; }

// "F1" for the function keys, else the virtual-key code.
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
			MenuClaim(MENU_TAA);
		else
			MenuRelease(MENU_TAA);
		LogInfo("TAA menu %s.", g_visible ? "shown" : "hidden");
	}
	menuWas = menu;
	if (g_visible && MenuOwner() != MENU_TAA)
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

// ---- Drawing -------------------------------------------------------------------------------------

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
		for (const char* d : { "d3dx9_42.dll", "d3dx9_43.dll" })
		{
			HMODULE m = GetModuleHandleA(d);
			if (!m)
				m = LoadLibraryA(d);
			if (m && (g_createFont = (PFN_D3DXCreateFontA)GetProcAddress(m, "D3DXCreateFontA")) != nullptr)
				break;
		}
	HRESULT hr = g_createFont ? g_createFont(dev, (INT)height, 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
										DEFAULT_PITCH | FF_DONTCARE, "Consolas", &g_font)
							  : E_FAIL;
	if (FAILED(hr) || !g_font)
	{
		g_fontFailed = true;
		g_font = nullptr;
		LogWarn("Menu: could not create a D3DX font, so the menu can't be drawn (its keys still work; changes are logged).");
		return false;
	}
	g_fontHeight = height;
	return true;
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
	if (!g_enabled)
		return;
	PollInput();
	if (!g_visible || !dev) // no device: input only (tests)
		return;
	IDirect3DSurface9 *bb = nullptr, *oldRt = nullptr;
	if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
		return;
	D3DSURFACE_DESC d = {};
	bb->GetDesc(&d);
	if (!EnsureFont(dev, d.Height))
	{
		bb->Release();
		return;
	}
	D3DVIEWPORT9 oldVp;
	dev->GetRenderTarget(0, &oldRt);
	dev->GetViewport(&oldVp);
	dev->SetRenderTarget(0, bb);
	D3DVIEWPORT9 vp = { 0, 0, d.Width, d.Height, 0.0f, 1.0f };
	dev->SetViewport(&vp);
	if (SUCCEEDED(dev->BeginScene()))
	{
		TaaSettings t = Taa_GetSettings();
		int right = (int)d.Width - (int)g_fontHeight, y = (int)g_fontHeight / 2, step = (int)(g_fontHeight * 1.25f);
		char line[160], key[16];
		MenuKeyName(g_menuKey, key, sizeof(key));
		DrawLine(y, right, "AC2 Shadow Overhaul - TAA", D3DCOLOR_XRGB(200, 200, 200));
		y += step;
		snprintf(line, sizeof(line), "Up/Down or 1-0 select, Left/Right change, %s close", key);
		DrawLine(y, right, line, D3DCOLOR_XRGB(160, 160, 160));
		y += step;
		snprintf(line, sizeof(line), "Status: %s", Taa_Status());
		DrawLine(y, right, line, D3DCOLOR_XRGB(255, 220, 120));
		y += step;
		for (int i = 0; i < ITEM_COUNT; i++)
		{
			char value[48], num[2] = { i < 9 ? (char)('1' + i) : i == 9 ? '0' : ' ', 0 };
			FormatValue(t, i, value, sizeof(value));
			snprintf(line, sizeof(line), "%s  %s   %s", num, kItems[i].name, value);
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

void Menu_Flush()
{
	if (g_dirty)
		SaveDirty();
}

void Menu_Init()
{
	g_menuKey = IniInt("Menu", "TAAMenuKey", VK_F1);
	g_enabled = g_menuKey > 0 && g_menuKey < 256;
	g_visible = false; // only the shadow menu can start shown ([Menu] StartVisible)
	char key[16];
	MenuKeyName(g_menuKey, key, sizeof(key));
	if (g_enabled)
		LogInfo("Menu: %s shows/hides it; Up/Down or 1-0 select, Left/Right change; changes are saved to the ini.", key);
	else
		LogInfo("Menu: off ([Menu] MenuKey=0).");
}

} // namespace taa
