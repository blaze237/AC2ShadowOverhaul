// See menu.h.
#include "core/menu.h"
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <initializer_list>

namespace core
{

// ---- Which menu is open --------------------------------------------------------------------------

static volatile LONG g_menuOwner = MENU_NONE;
int MenuOwner() { return (int)g_menuOwner; }
void MenuClaim(int id) { InterlockedExchange(&g_menuOwner, id); }
void MenuRelease(int id) { InterlockedCompareExchange(&g_menuOwner, MENU_NONE, id); }

// ---- Minimal ID3DXFont ---------------------------------------------------------------------------

struct TextFont::Font
{
	struct Vtbl
	{
		void* QueryInterface;
		void* AddRef;
		ULONG(__stdcall* Release)(Font*);
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
		INT(__stdcall* DrawTextA)(Font*, void* sprite, LPCSTR text, INT count, RECT* rect, DWORD format, D3DCOLOR color);
		void* DrawTextW;
		HRESULT(__stdcall* OnLostDevice)(Font*);
		HRESULT(__stdcall* OnResetDevice)(Font*);
	};
	Vtbl* vt;
};
typedef HRESULT(WINAPI* PFN_D3DXCreateFontA)(IDirect3DDevice9*, INT, UINT, UINT, UINT, BOOL, DWORD, DWORD, DWORD, DWORD, LPCSTR, void**);

bool TextFont::Ensure(IDirect3DDevice9* dev, UINT height, const char* failLog)
{
	if (m_font && height == m_height)
		return true;
	if (m_font)
		m_font->vt->Release(m_font);
	m_font = nullptr;
	if (m_failed)
		return false;
	// D3DX: the version the game loads, else the last one.
	static PFN_D3DXCreateFontA create = nullptr;
	if (!create)
		for (const char* d : { "d3dx9_42.dll", "d3dx9_43.dll" })
		{
			HMODULE m = GetModuleHandleA(d);
			if (!m)
				m = LoadLibraryA(d);
			if (m && (create = (PFN_D3DXCreateFontA)GetProcAddress(m, "D3DXCreateFontA")) != nullptr)
				break;
		}
	void* font = nullptr;
	HRESULT hr = create ? create(dev, (INT)height, 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
							  DEFAULT_PITCH | FF_DONTCARE, "Consolas", &font)
						: E_FAIL;
	if (FAILED(hr) || !font)
	{
		m_failed = true;
		if (failLog)
			LogWarn("%s", failLog);
		return false;
	}
	m_font = (Font*)font;
	m_height = height;
	return true;
}

void TextFont::Draw(const char* text, RECT r, DWORD format, D3DCOLOR color) const
{
	if (m_font)
		m_font->vt->DrawTextA(m_font, nullptr, text, -1, &r, format, color);
}

void TextFont::OnLostDevice()
{
	if (m_font)
		m_font->vt->OnLostDevice(m_font);
}

void TextFont::OnResetDevice()
{
	if (m_font)
		m_font->vt->OnResetDevice(m_font);
}

// ---- Drawing on the back buffer ----------------------------------------------------------------------

BackBufferOverlay::BackBufferOverlay(IDirect3DDevice9* dev) : m_dev(dev)
{
	if (!dev || FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &m_bb)) || !m_bb)
	{
		m_bb = nullptr;
		return;
	}
	D3DSURFACE_DESC d;
	if (FAILED(m_bb->GetDesc(&d)) || !d.Width || !d.Height)
	{
		m_bb->Release();
		m_bb = nullptr;
		return;
	}
	m_w = d.Width;
	m_h = d.Height;
}

bool BackBufferOverlay::Begin()
{
	if (!m_bb || m_bound)
		return false;
	m_dev->GetRenderTarget(0, &m_oldRt);
	m_dev->GetViewport(&m_oldVp);
	m_dev->SetRenderTarget(0, m_bb);
	D3DVIEWPORT9 vp = { 0, 0, m_w, m_h, 0.0f, 1.0f };
	m_dev->SetViewport(&vp);
	m_bound = true;
	m_inScene = SUCCEEDED(m_dev->BeginScene());
	return m_inScene;
}

BackBufferOverlay::~BackBufferOverlay()
{
	if (!m_bb)
		return;
	if (m_bound)
	{
		if (m_inScene)
			m_dev->EndScene();
		m_dev->SetRenderTarget(0, m_oldRt ? m_oldRt : m_bb);
		m_dev->SetViewport(&m_oldVp);
		if (m_oldRt)
			m_oldRt->Release();
	}
	m_bb->Release();
}

// ---- Menus ---------------------------------------------------------------------------------------

static TextFont g_font; // all menus (one is shown at a time)
static SHORT(WINAPI* g_keyStateForTests)(int) = nullptr;
static bool (*g_focusForTests)() = nullptr;

void Menu_OnLostDevice() { g_font.OnLostDevice(); }
void Menu_OnResetDevice() { g_font.OnResetDevice(); }

void Menu_SetInputForTests(SHORT(WINAPI* keyState)(int), bool (*hasFocus)())
{
	g_keyStateForTests = keyState;
	g_focusForTests = hasFocus;
}

// GetAsyncKeyState is called through the import table each time (the end-to-end tests replace it there).
static bool KeyDown(int vk)
{
	if (vk <= 0 || vk >= 256)
		return false;
	SHORT s = g_keyStateForTests ? g_keyStateForTests(vk) : GetAsyncKeyState(vk);
	return (s & 0x8000) != 0;
}

static bool HasFocus() { return g_focusForTests ? g_focusForTests() : GameHasFocus(); }

// "F9" for the function keys, else the virtual-key code.
static void KeyName(int vk, char* buf, size_t n)
{
	if (vk >= VK_F1 && vk <= VK_F24)
		snprintf(buf, n, "F%d", vk - VK_F1 + 1);
	else
		snprintf(buf, n, "key 0x%02X", vk);
}

bool Menu::Init()
{
	m_key = IniInt("Menu", m_def.keyIniKey, m_def.defaultKey);
	m_enabled = m_key > 0 && m_key < 256;
	m_visible = false; // only the shadow menu can start shown ([Menu] StartVisible)
	if (!m_enabled)
	{
		LogInfo("Menu: off ([Menu] MenuKey=0).");
		return false;
	}
	char key[16];
	KeyName(m_key, key, sizeof(key));
	LogInfo("Menu: %s shows/hides it; Up/Down or 1-0 select, Left/Right change; changes are saved to the ini.", key);
	return true;
}

void Menu::Show()
{
	m_visible = true;
	MenuClaim(m_def.id);
}

void Menu::Flush()
{
	if (m_dirty)
		SaveDirty();
}

void Menu::SaveDirty()
{
	for (int i = 0; i < m_def.itemCount; i++)
	{
		const MenuItem& it = m_def.items[i];
		if (!(m_dirty & (1u << i)) || !it.section)
			continue;
		char v[32];
		IniValue(i, v, sizeof(v));
		if (IniWrite(it.section, it.key, v))
		{
			if (m_def.logSaves)
				LogInfo("Menu: saved [%s] %s=%s", it.section, it.key, v);
		}
		else
			LogWarn("Menu: could not write [%s] %s=%s to %s (error %lu)", it.section, it.key, v, IniPath(), GetLastError());
		AfterSave(i);
	}
	m_dirty = 0;
}

void Menu::DoChange(int item, int dir)
{
	if (!Change(item, dir))
		return;
	const MenuItem& it = m_def.items[item];
	char value[64];
	Format(item, value, sizeof(value));
	if (!m_def.logIniOnChange)
		LogInfo("Menu: %s = %s", it.name, value);
	else if (!it.section)
		LogInfo("Menu: %s = %s   (not saved)", it.name, value);
	else
		LogInfo("Menu: %s = %s   (ini: [%s] %s)", it.name, value, it.section, it.key);
	if (it.section)
	{
		m_dirty |= 1u << item;
		m_lastChange = GetTickCount();
	}
}

// Menu key; while shown, Up / Down and 1-9, 0 select, Left / Right change (holding repeats).
void Menu::PollInput()
{
	if (m_dirty && GetTickCount() - m_lastChange > 1000)
		SaveDirty();
	if (!HasFocus())
	{
		m_heldDir = 0;
		return;
	}
	bool menu = KeyDown(m_key);
	if (menu && !m_menuWas)
	{
		m_visible = !m_visible;
		if (m_visible)
			MenuClaim(m_def.id);
		else
			MenuRelease(m_def.id);
		OnVisibleChanged(m_visible);
		LogInfo("%s %s.", m_def.logName, m_visible ? "shown" : "hidden");
	}
	m_menuWas = menu;
	if (m_visible && MenuOwner() != m_def.id)
	{
		m_visible = false; // another menu was opened
		OnVisibleChanged(false);
	}
	const int count = Count();
	if (m_selected >= count)
		m_selected = 0;
	if (!m_visible)
	{
		// Keys held while the menu opens don't count until released.
		m_heldDir = 0;
		m_upWas = m_downWas = true;
		for (bool& w : m_numWas)
			w = true;
		return;
	}
	for (int i = 0; i < 10; i++)
	{
		bool down = KeyDown(i < 9 ? '1' + i : '0') || KeyDown(i < 9 ? VK_NUMPAD1 + i : VK_NUMPAD0);
		if (down && !m_numWas[i] && i < count)
			m_selected = i;
		m_numWas[i] = down;
	}
	bool up = KeyDown(VK_UP), dn = KeyDown(VK_DOWN);
	if (up && !m_upWas)
		m_selected = (m_selected + count - 1) % count;
	if (dn && !m_downWas)
		m_selected = (m_selected + 1) % count;
	m_upWas = up;
	m_downWas = dn;
	int dir = KeyDown(VK_RIGHT) ? 1 : KeyDown(VK_LEFT) ? -1 : 0;
	DWORD now = GetTickCount();
	if (dir != m_heldDir)
	{
		m_heldDir = dir;
		m_heldSince = m_lastRepeat = now;
		if (dir)
			DoChange(m_selected, dir);
	}
	else if (dir && m_def.items[m_selected].repeat && now - m_heldSince > 400 && now - m_lastRepeat > 120)
	{
		m_lastRepeat = now;
		DoChange(m_selected, dir);
	}
}

// Top right: title, help, status (if any), then the items, each "<number>  <name>   <value>",
// right-aligned with a drop shadow; the selected item in red.
void Menu::Draw(IDirect3DDevice9* dev)
{
	BackBufferOverlay o(dev);
	if (!o.Ok())
		return;
	UINT height = o.Height() / 45;
	if (height < 14)
		height = 14;
	if (!g_font.Ensure(dev, height, "Menu: could not create a D3DX font; the menu can't be drawn (keys still work, see the log)."))
		return;
	if (!o.Begin())
		return;
	const int h = (int)g_font.Height();
	const int right = (int)o.Width() - h, step = (int)(h * 1.25f), shadow = h / 12 + 1;
	int y = h / 2;
	auto line = [&](const char* text, D3DCOLOR color) {
		const DWORD fmt = DT_RIGHT | DT_TOP | DT_NOCLIP | DT_SINGLELINE;
		g_font.Draw(text, { 0, y + shadow, right + shadow, y + h * 2 }, fmt, D3DCOLOR_ARGB(200, 0, 0, 0));
		g_font.Draw(text, { 0, y, right, y + h * 2 }, fmt, color);
		y += step;
	};
	char text[160], key[16], value[64], status[128];
	KeyName(m_key, key, sizeof(key));
	line(m_def.title, D3DCOLOR_XRGB(200, 200, 200));
	snprintf(text, sizeof(text), "Up/Down or 1-0 select, Left/Right change, %s close", key);
	line(text, D3DCOLOR_XRGB(160, 160, 160));
	if (Status(status, sizeof(status)))
	{
		snprintf(text, sizeof(text), "Status: %s", status);
		line(text, D3DCOLOR_XRGB(255, 220, 120));
	}
	for (int i = 0; i < Count(); i++)
	{
		const char num[2] = { i < 9 ? (char)('1' + i) : i == 9 ? '0' : ' ', 0 };
		Format(i, value, sizeof(value));
		snprintf(text, sizeof(text), "%s  %s   %s", num, m_def.items[i].name, value);
		line(text, i == m_selected ? D3DCOLOR_XRGB(255, 50, 50) : D3DCOLOR_XRGB(255, 255, 255));
	}
}

void Menu::OnPresent(IDirect3DDevice9* dev)
{
	if (!m_enabled)
		return;
	PollInput();
	if (m_visible && dev)
		Draw(dev);
}

} // namespace core

namespace core
{
bool MenuKeyHeld(int vk) { return HasFocus() && KeyDown(vk); }
}
