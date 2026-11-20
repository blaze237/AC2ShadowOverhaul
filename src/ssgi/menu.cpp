// In-game menu: live tuning of the SSGI settings, drawn in the top-right corner of the screen.
//
// The menu key ([Menu] MenuKey, default F3) shows / hides it. While shown: Up / Down or the number
// keys 1-9, 0 select a setting (drawn in red), Left / Right change it (holding repeats). Changes are
// saved to the ini a second after the last key press (and at exit) and logged. While hidden it
// ignores every key. A status line shows whether the passes are running and what they cost on the GPU.
// (The same layout and keys as the other AC2 plugins' menus.)
//
// Text is drawn with ID3DXFont from the d3dx9_42.dll the game already loads, just before Present.

#include "common.h"
#include "gi.h"
#include "menu.h"
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

namespace ssgi
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
typedef HRESULT(WINAPI* PFN_D3DXCreateFontA)(IDirect3DDevice9*, INT, UINT, UINT, UINT, BOOL, DWORD, DWORD, DWORD, DWORD, LPCSTR,
	D3DXFont**);

static PFN_D3DXCreateFontA g_createFont;
static D3DXFont* g_font;
static UINT g_fontHeight;
static bool g_fontFailed;

// ---- Items ---------------------------------------------------------------------------------------

static bool g_enabled = false;
static bool g_visible = false;
static int g_menuKey = VK_F3;
static int g_selected = 0;
static unsigned g_dirty; // bit per setting
static DWORD g_lastChange;

struct Item
{
	const char* name;
	const char* iniKey; // in [SSGI]
	const char* unit;
	int decimals;
};
static const Item kItems[GS_COUNT] = {
	{ "SSGI", "StartEnabled", "", 0 },
	{ "Debug view", "DebugView", "", 0 },
	{ "Intensity", "Intensity", "", 2 },
	{ "Radius", "Radius", " m", 1 },
	{ "Max screen radius", "MaxScreenRadius", " x height", 2 },
	{ "Thickness", "Thickness", " m", 2 },
	{ "Slices", "Slices", "", 0 },
	{ "Steps", "Steps", "", 0 },
	{ "Temporal blend", "TemporalBlend", "", 2 },
	{ "Radiance clamp", "RadianceClamp", "", 1 },
};
static const char* const kViews[] = { "off", "GI light", "raw trace", "normals" };

// Step for a change from value v in direction dir (finer steps at the low end of each range).
static float Step(int id, float v, int dir)
{
	bool up = dir > 0;
	switch (id)
	{
	case GS_INTENSITY: return (up ? v < 0.999f : v < 1.001f) ? 0.05f : 0.25f;
	case GS_RADIUS: return (up ? v < 3.999f : v < 4.001f) ? 0.5f : 1.0f;
	case GS_MAX_SCREEN: return 0.05f;
	case GS_THICKNESS: return (up ? v < 0.499f : v < 0.501f) ? 0.05f : 0.25f;
	case GS_BLEND: return 0.02f;
	case GS_CLAMP: return (up ? v < 7.999f : v < 8.001f) ? 0.5f : 4.0f;
	default: return 1.0f;
	}
}

static void FormatValue(int id, char* buf, size_t n)
{
	float v = GI_GetSetting(id);
	if (id == GS_ON)
		snprintf(buf, n, "%s", v > 0.5f ? "ON" : "OFF");
	else if (id == GS_VIEW)
		snprintf(buf, n, "%s", kViews[(int)v & 3]);
	else
		snprintf(buf, n, "%.*f%s", kItems[id].decimals, (double)v, kItems[id].unit);
}

static void SaveDirty()
{
	for (int i = 0; i < GS_COUNT; i++)
	{
		if (!(g_dirty & (1u << i)))
			continue;
		char v[32];
		snprintf(v, sizeof(v), "%.*f", kItems[i].decimals, (double)GI_GetSetting(i));
		if (IniWrite("SSGI", kItems[i].iniKey, v))
			LogInfo("Menu: saved [SSGI] %s=%s", kItems[i].iniKey, v);
		else
			LogWarn("Menu: could not write [SSGI] %s=%s to %s (error %lu)", kItems[i].iniKey, v, IniPath(), GetLastError());
	}
	g_dirty = 0;
}

static void Change(int id, int dir)
{
	float v = GI_GetSetting(id);
	if (id == GS_ON)
		v = dir > 0 ? 1.0f : 0.0f;
	else if (id == GS_VIEW)
		v = (float)(((int)v + dir + 4) % 4);
	else
	{
		// Round to the step so repeated changes land on tidy values.
		float step = Step(id, v, dir);
		v = floorf((v + step * dir) / step + 0.5f) * step;
	}
	GI_SetSetting(id, v);
	char value[48];
	FormatValue(id, value, sizeof(value));
	LogInfo("Menu: %s = %s", kItems[id].name, value);
	g_dirty |= 1u << id;
	g_lastChange = GetTickCount();
}

// ---- Input -------------------------------------------------------------------------------------

static bool KeyDown(int vk) { return vk > 0 && vk < 256 && (GetAsyncKeyState(vk) & 0x8000) != 0; }

// "F3" for the function keys, else the virtual-key code.
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
			MenuClaim(MENU_SSGI);
		else
			MenuRelease(MENU_SSGI);
		GI_SetTiming(g_visible);
		LogInfo("SSGI menu %s.", g_visible ? "shown" : "hidden");
	}
	menuWas = menu;
	if (g_visible && MenuOwner() != MENU_SSGI)
	{
		g_visible = false; // another menu was opened
		GI_SetTiming(false);
	}
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
		if (down && !numWas[i] && i < GS_COUNT)
			g_selected = i;
		numWas[i] = down;
	}
	bool up = KeyDown(VK_UP), dn = KeyDown(VK_DOWN);
	if (up && !upWas)
		g_selected = (g_selected + GS_COUNT - 1) % GS_COUNT;
	if (dn && !downWas)
		g_selected = (g_selected + 1) % GS_COUNT;
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

// "tracing 960x540, 0.42 ms" / why not.
static void StatusText(char* buf, size_t n)
{
	static unsigned lastFrames = 0;
	static DWORD lastSeen = 0;
	GiStatus st = GI_GetStatus();
	DWORD now = GetTickCount();
	if (st.framesTraced != lastFrames)
	{
		lastFrames = st.framesTraced;
		lastSeen = now;
	}
	bool running = lastSeen && now - lastSeen < 500;
	if (GI_GetSetting(GS_ON) < 0.5f)
		snprintf(buf, n, "off (the first setting turns it on)");
	else if (st.failed)
		snprintf(buf, n, "FAILED: could not create its textures (see AC2SSGI.log)");
	else if (!running)
		snprintf(buf, n, "not running: no opaque pass seen (in-game anti-aliasing on? menus?)");
	else if (st.gpuMs >= 0)
		snprintf(buf, n, "tracing %ux%u, GPU %.2f ms", st.traceW, st.traceH, (double)st.gpuMs);
	else
		snprintf(buf, n, "tracing %ux%u", st.traceW, st.traceH);
}

void Menu_OnPresent(IDirect3DDevice9* dev, const GiDeviceFns& D)
{
	if (!g_enabled)
		return;
	PollInput();
	if (!g_visible)
		return;

	IDirect3DSurface9 *bb = nullptr, *oldRt = nullptr;
	if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
		return;
	D3DSURFACE_DESC desc;
	if (FAILED(bb->GetDesc(&desc)) || !EnsureFont(dev, desc.Height))
	{
		bb->Release();
		return;
	}
	// Draw on the back buffer with a full viewport; restore the game's target and viewport after.
	D3DVIEWPORT9 oldVp;
	dev->GetRenderTarget(0, &oldRt);
	dev->GetViewport(&oldVp);
	D.SetRenderTarget(dev, 0, bb);
	D3DVIEWPORT9 vp = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
	dev->SetViewport(&vp);
	if (SUCCEEDED(dev->BeginScene()))
	{
		int right = (int)desc.Width - (int)g_fontHeight, y = (int)g_fontHeight / 2;
		int step = (int)(g_fontHeight * 1.25f);
		char key[16], line[160], status[128];
		MenuKeyName(g_menuKey, key, sizeof(key));
		DrawLine(y, right, "AC2 Shadow Overhaul - SSGI", D3DCOLOR_XRGB(200, 200, 200));
		y += step;
		snprintf(line, sizeof(line), "Up/Down or 1-0 select, Left/Right change, %s close", key);
		DrawLine(y, right, line, D3DCOLOR_XRGB(160, 160, 160));
		y += step;
		StatusText(status, sizeof(status));
		snprintf(line, sizeof(line), "Status: %s", status);
		DrawLine(y, right, line, D3DCOLOR_XRGB(255, 220, 120));
		y += step;
		for (int i = 0; i < GS_COUNT; i++)
		{
			char value[48], num[2] = { i < 9 ? (char)('1' + i) : i == 9 ? '0' : ' ', 0 };
			FormatValue(i, value, sizeof(value));
			snprintf(line, sizeof(line), "%s  %s   %s", num, kItems[i].name, value);
			DrawLine(y, right, line, i == g_selected ? D3DCOLOR_XRGB(255, 50, 50) : D3DCOLOR_XRGB(255, 255, 255));
			y += step;
		}
		dev->EndScene();
	}
	D.SetRenderTarget(dev, 0, oldRt ? oldRt : bb);
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
	g_menuKey = IniInt("Menu", "SSGIMenuKey", VK_F3);
	g_enabled = g_menuKey > 0 && g_menuKey < 256;
	if (!g_enabled)
	{
		LogInfo("Menu: off ([Menu] MenuKey=0).");
		return false;
	}
	g_visible = false; // only the shadow menu can start shown ([Menu] StartVisible)
	GI_SetTiming(false);
	char key[16];
	MenuKeyName(g_menuKey, key, sizeof(key));
	LogInfo("Menu: %s shows/hides it; Up/Down or 1-0 select, Left/Right change; changes are saved to the ini.", key);
	return true;
}

} // namespace ssgi
