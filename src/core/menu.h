// The in-game menus: one engine for the four (shadows & AO F9, SSGI F3, TAA F1, volumetrics F11),
// each an item table plus callbacks.
//
// A menu's key ([Menu] ...MenuKey) shows / hides it in the top-right corner of the screen. While
// shown: Up / Down or the number keys 1-9, 0 select an item (drawn in red), Left / Right change it
// (holding repeats after 400 ms, then every 120 ms). Changes apply at once, are logged, and are
// saved to the ini a second after the last change (so holding a key doesn't rewrite the file on
// every repeat) and at exit. While hidden a menu ignores every key, and keys held when it opens
// don't count until released. Only one menu is open at a time: one that opens claims ownership,
// the others see they lost it and hide.
//
// Text is drawn with ID3DXFont from the d3dx9_42.dll the game already loads, just before Present,
// on the back buffer through the device's vtable (the hooks pass those calls straight on while
// g_fxPassthrough is set).
#pragma once

#include <windows.h>
#include <d3d9.h>

namespace core
{

// ---- Which menu is open --------------------------------------------------------------------------

enum MenuId
{
	MENU_NONE = 0,
	MENU_SHADOWS,
	MENU_SSGI,
	MENU_TAA,
	MENU_VOLUMETRICS,
};
int MenuOwner();
void MenuClaim(int id);   // this menu was opened
void MenuRelease(int id); // this menu was closed (only clears ownership if it still has it)

// ---- Text ----------------------------------------------------------------------------------------

// A bold Consolas ID3DXFont (only the few methods used here are declared).
class TextFont
{
public:
	// Creates the font, or re-creates it when the height changes. False if D3DX can't make one;
	// then `failLog` (if any) is logged, once, and it isn't tried again.
	bool Ensure(IDirect3DDevice9* dev, UINT height, const char* failLog = nullptr);
	UINT Height() const { return m_height; }
	void Draw(const char* text, RECT r, DWORD format, D3DCOLOR color) const;
	void OnLostDevice();
	void OnResetDevice();

private:
	struct Font;
	Font* m_font = nullptr;
	UINT m_height = 0;
	bool m_failed = false;
};

// Draws on the back buffer with a full viewport, then puts the game's render target 0 and viewport
// back: construct, check Ok(), Begin() (binds it and begins a scene), draw; the destructor ends the
// scene and restores.
class BackBufferOverlay
{
public:
	explicit BackBufferOverlay(IDirect3DDevice9* dev);
	~BackBufferOverlay();
	bool Ok() const { return m_bb != nullptr; } // the back buffer and its size are known
	UINT Width() const { return m_w; }
	UINT Height() const { return m_h; }
	bool Begin(); // false if BeginScene failed (nothing to draw; still restored)

private:
	IDirect3DDevice9* m_dev;
	IDirect3DSurface9* m_bb = nullptr;
	IDirect3DSurface9* m_oldRt = nullptr;
	D3DVIEWPORT9 m_oldVp = {};
	UINT m_w = 0, m_h = 0;
	bool m_bound = false, m_inScene = false;
};

// ---- Menus ---------------------------------------------------------------------------------------

struct MenuItem
{
	const char* name;
	const char* section; // ini section its value is saved in; null = not saved
	const char* key;     // ini key
	bool repeat;         // holding Left / Right repeats the change
};

struct MenuDef
{
	int id;                 // MenuId
	const char* title;      // first line, "AC2 Shadow Overhaul - ..."
	const char* logName;    // logged as "<logName> shown." / "hidden."
	const char* keyIniKey;  // [Menu] key holding the virtual key code
	int defaultKey;
	const MenuItem* items;
	int itemCount;          // at most 32
	bool logIniOnChange;    // changes logged with their ini key: "Menu: X = v   (ini: [S] K)" / "(not saved)"
	bool logSaves;          // "Menu: saved [S] K=v" for each write
};

class Menu
{
public:
	explicit Menu(const MenuDef& def) : m_def(def) {}
	virtual ~Menu() {}

	bool Init(); // reads the menu key; false (logged) if it is 0 (no menu)
	bool Enabled() const { return m_enabled; }
	bool Visible() const { return m_visible; }
	int Selected() const { return m_selected; }
	void Show(); // shown at start ([Menu] StartVisible)
	// Polls the keys, then draws if shown. A null device: keys only (tests).
	void OnPresent(IDirect3DDevice9* dev);
	void Flush(); // writes unsaved changes now (at exit)

protected:
	virtual int Count() const { return m_def.itemCount; } // items listed now (a prefix of the table)
	virtual void Format(int item, char* buf, size_t n) = 0;   // the value as shown
	virtual void IniValue(int item, char* buf, size_t n) = 0; // the value as saved
	virtual bool Change(int item, int dir) = 0; // Left -1 / Right +1; false: nothing changed (not logged, not saved)
	virtual void AfterSave(int item) { (void)item; } // more ini writes that go with an item
	virtual bool Status(char* buf, size_t n) { (void)buf, (void)n; return false; } // "Status: ..." line, if any
	virtual void OnVisibleChanged(bool visible) { (void)visible; }

private:
	void PollInput();
	void Draw(IDirect3DDevice9* dev);
	void DoChange(int item, int dir);
	void SaveDirty();

	const MenuDef& m_def;
	bool m_enabled = false;
	bool m_visible = false;
	int m_key = 0;
	int m_selected = 0;
	unsigned m_dirty = 0; // bit per item
	DWORD m_lastChange = 0;
	// Input state between polls.
	bool m_menuWas = false, m_upWas = false, m_downWas = false, m_numWas[10] = {};
	int m_heldDir = 0;
	DWORD m_heldSince = 0, m_lastRepeat = 0;
};

// The menus' font (all four share it) across a device reset.
void Menu_OnLostDevice();
void Menu_OnResetDevice();

// Tests: replaces GetAsyncKeyState and the game-focus check (null = the real ones).
void Menu_SetInputForTests(SHORT(WINAPI* keyState)(int vk), bool (*hasFocus)());

} // namespace core
