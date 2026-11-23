// Tests the shared menu engine (src/core/menu.cpp) with a fake keyboard and two small menus:
//  - hidden, a menu ignores every key; its key shows / hides it;
//  - Up / Down wrap, 1-9 and 0 select (only items that are listed), Left / Right change;
//  - holding Left / Right repeats after 400 ms (not for items that don't repeat);
//  - keys held while a menu opens don't count until released; nothing happens without focus;
//  - opening one menu hides the other (it is told), and the arrows then reach only the open one;
//  - changes are logged and saved to the ini a second after the last one (plus AfterSave's
//    writes), items without an ini key are not saved, Flush saves at once.
#include "core/menu.h"
#include "common.h"
#include <windows.h>
#include <stdio.h>
#include <string>

static int g_fail = 0;
#define CHECK(cond, ...)                                                                                                              \
	do                                                                                                                                \
	{                                                                                                                                 \
		if (!(cond))                                                                                                                  \
		{                                                                                                                             \
			g_fail++;                                                                                                                 \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                               \
			printf(__VA_ARGS__);                                                                                                      \
			printf("\n");                                                                                                             \
		}                                                                                                                             \
	} while (0)

static bool g_keys[256];
static bool g_focus = true;
static SHORT WINAPI FakeKeyState(int vk) { return vk >= 0 && vk < 256 && g_keys[vk] ? (SHORT)0x8000 : 0; }
static bool FakeFocus() { return g_focus; }

// ---- A test menu: integer values, item 3 doesn't repeat, item 4 isn't saved ----------------------

static const core::MenuItem kItems[] = {
	{ "Alpha", "Test", "Alpha", true },
	{ "Beta", "Test", "Beta", true },
	{ "Gamma", "Test", "Gamma", true },
	{ "Once", "Test", "Once", false },
	{ "Unsaved", nullptr, nullptr, true },
	{ "Six", "Test", "Six", true },
	{ "Seven", "Test", "Seven", true },
	{ "Eight", "Test", "Eight", true },
	{ "Nine", "Test", "Nine", true },
	{ "Ten", "Test", "Ten", true },
	{ "Eleven", "Test", "Eleven", true },
};
static const int kCount = sizeof(kItems) / sizeof(kItems[0]);
static const core::MenuDef kDefA = { core::MENU_SSGI, "Test A", "Menu A", "TestMenuKeyA", 'A', kItems, kCount, false, true };
static const core::MenuDef kDefB = { core::MENU_TAA, "Test B", "Menu B", "TestMenuKeyB", 'B', kItems, 3, false, true };

class TestMenu : public core::Menu
{
public:
	explicit TestMenu(const core::MenuDef& d) : Menu(d) {}
	int value[16] = {};
	int changes = 0, afterSaves = 0, shown = 0, hidden = 0;
	int listed = kCount;

protected:
	int Count() const override { return listed; }
	void Format(int item, char* buf, size_t n) override { snprintf(buf, n, "%d", value[item]); }
	void IniValue(int item, char* buf, size_t n) override { snprintf(buf, n, "%d", value[item]); }
	bool Change(int item, int dir) override
	{
		if (item == 2 && value[2] + dir < 0)
			return false; // a value that can't change: not logged or saved
		value[item] += dir;
		changes++;
		return true;
	}
	void AfterSave(int item) override
	{
		afterSaves++;
		if (item == 0)
			IniWrite("Test", "AlphaExtra", "1");
	}
	void OnVisibleChanged(bool visible) override { (visible ? shown : hidden)++; }
};

static TestMenu A(kDefA), B(kDefB);
static std::string g_ini;

static void Poll() // one Present
{
	A.OnPresent(nullptr);
	B.OnPresent(nullptr);
}

static void Press(int vk)
{
	g_keys[vk] = true;
	Poll();
	g_keys[vk] = false;
	Poll();
}

static int IniGet(const char* key) { return GetPrivateProfileIntA("Test", key, -999, g_ini.c_str()); }

int main()
{
	char dir[MAX_PATH];
	GetFullPathNameA(".", MAX_PATH, dir, nullptr);
	g_ini = std::string(dir) + "\\menu_test.ini";
	DeleteFileA(g_ini.c_str());
	WritePrivateProfileStringA("Menu", "TestMenuKeyA", "0x41", g_ini.c_str());
	WritePrivateProfileStringA("Menu", "TestMenuKeyB", "66", g_ini.c_str());
	IniSetPath(g_ini.c_str());
	LogInit("menu_test.log", true);
	core::Menu_SetInputForTests(FakeKeyState, FakeFocus);
	CHECK(A.Init() && B.Init() && A.Enabled() && B.Enabled(), "menus not enabled");

	// Hidden: nothing reacts.
	Press(VK_RIGHT);
	Press(VK_DOWN);
	CHECK(A.changes == 0 && A.Selected() == 0 && !A.Visible(), "a hidden menu reacted");

	// Open A; select and change.
	Press('A');
	CHECK(A.Visible() && core::MenuOwner() == core::MENU_SSGI && A.shown == 1, "A not shown");
	Press(VK_UP); // wraps to the last item
	CHECK(A.Selected() == kCount - 1, "Up from the first item selected %d", A.Selected());
	Press(VK_DOWN); // wraps back
	CHECK(A.Selected() == 0, "Down from the last item selected %d", A.Selected());
	Press('0');
	CHECK(A.Selected() == 9, "0 selected %d", A.Selected());
	Press(VK_NUMPAD2);
	CHECK(A.Selected() == 1, "numpad 2 selected %d", A.Selected());
	Press(VK_RIGHT);
	Press(VK_RIGHT);
	Press(VK_LEFT);
	CHECK(A.value[1] == 1 && A.changes == 3, "Beta %d after Right, Right, Left (%d changes)", A.value[1], A.changes);
	CHECK(IniGet("Beta") == -999, "saved before the delay");

	// A change that doesn't happen isn't counted (and later not saved).
	Press('3');
	Press(VK_LEFT);
	CHECK(A.value[2] == 0 && A.changes == 3, "Gamma changed below 0");

	// Fewer items listed: number keys past them do nothing, the selection stays in range.
	A.listed = 4;
	Press('6');
	CHECK(A.Selected() == 2, "6 selected item %d with 4 listed", A.Selected());
	Press(VK_DOWN);
	Press(VK_DOWN);
	CHECK(A.Selected() == 0, "Down past the 4 listed items selected %d", A.Selected());
	A.listed = kCount;

	// Holding: one change at once, repeats only after 400 ms, then about every 120 ms.
	Press('1');
	int before = A.value[0];
	g_keys[VK_RIGHT] = true;
	DWORD start = GetTickCount();
	Poll();
	while (GetTickCount() - start < 300)
	{
		Sleep(10);
		Poll();
	}
	CHECK(A.value[0] == before + 1, "held 300 ms: %d changes (expected 1)", A.value[0] - before);
	while (GetTickCount() - start < 1000)
	{
		Sleep(10);
		Poll();
	}
	g_keys[VK_RIGHT] = false;
	Poll();
	int held = A.value[0] - before;
	CHECK(held >= 4 && held <= 7, "held 1 s: %d changes (expected about 1 + 600 / 120)", held);
	// An item that doesn't repeat.
	Press('4');
	g_keys[VK_RIGHT] = true;
	start = GetTickCount();
	while (GetTickCount() - start < 700)
	{
		Poll();
		Sleep(10);
	}
	g_keys[VK_RIGHT] = false;
	Poll();
	CHECK(A.value[3] == 1, "a non-repeating item changed %d times while held", A.value[3]);

	// The unsaved item.
	Press('5');
	Press(VK_RIGHT);
	CHECK(A.value[4] == 1, "Unsaved not changed");

	// Saving: a second after the last change, on a poll.
	Sleep(1100);
	Poll();
	CHECK(IniGet("Beta") == 1 && IniGet("Alpha") == A.value[0] && IniGet("Once") == 1, "not saved after the delay (Beta %d, Alpha %d, Once %d)",
		IniGet("Beta"), IniGet("Alpha"), IniGet("Once"));
	CHECK(IniGet("Unsaved") == -999 && IniGet("Gamma") == -999, "an unsaved or unchanged item was written");
	CHECK(IniGet("AlphaExtra") == 1 && A.afterSaves == 3, "AfterSave not called for each saved item (%d)", A.afterSaves);

	// No focus: keys ignored.
	g_focus = false;
	Press(VK_RIGHT);
	CHECK(A.value[4] == 1, "a key acted without focus");
	g_focus = true;

	// Opening B hides A (A is told); the arrows reach only B. Keys held when B opens don't count.
	int aChanges = A.changes;
	g_keys[VK_DOWN] = true;
	Press('B');
	Poll();
	CHECK(B.Visible() && !A.Visible() && A.hidden == 1 && core::MenuOwner() == core::MENU_TAA, "B didn't take over from A");
	CHECK(B.Selected() == 0, "Down held while B opened moved its selection (%d)", B.Selected());
	g_keys[VK_DOWN] = false;
	Poll();
	Press(VK_DOWN);
	Press(VK_RIGHT);
	CHECK(B.Selected() == 1 && B.value[1] == 1 && A.changes == aChanges, "the arrows didn't go to B only");

	// Flush writes at once; closing B releases ownership.
	B.Flush();
	CHECK(IniGet("Beta") == 1, "flush didn't save");
	Press('B');
	CHECK(!B.Visible() && core::MenuOwner() == core::MENU_NONE, "B not closed");
	Press(VK_RIGHT);
	CHECK(B.value[1] == 1 && A.changes == aChanges, "a key acted with every menu closed");

	// A menu key of 0 means no menu.
	WritePrivateProfileStringA("Menu", "TestMenuKeyA", "0", g_ini.c_str());
	TestMenu off(kDefA);
	CHECK(!off.Init() && !off.Enabled(), "menu key 0 still enabled the menu");

	printf(g_fail ? "%d FAILURES\n" : "all menu engine checks passed\n", g_fail);
	DeleteFileA(g_ini.c_str());
	return g_fail ? 1 : 0;
}
