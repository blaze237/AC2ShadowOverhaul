// Tests the TAA in-game menu's input and saving (src/taa/menu.cpp) with a fake keyboard: F1 opens it,
// arrows select and change, values reach the TAA settings at once and land in the ini a second
// later (comments kept); keys do nothing while it's hidden.
#include <windows.h>
static bool g_keys[256];
static SHORT WINAPI FakeAsyncKeyState(int vk) { return vk >= 0 && vk < 256 && g_keys[vk] ? (SHORT)0x8000 : 0; }
static bool FakeFocus() { return true; }
#define GetAsyncKeyState FakeAsyncKeyState
#define GameHasFocus FakeFocus
#include "taa/menu.cpp"
#undef GetAsyncKeyState
using namespace taa;
#include <string>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); g_failures++; } } while (0)

static void Press(int vk)
{
	g_keys[vk] = true;
	Menu_OnPresent(nullptr);
	g_keys[vk] = false;
	Menu_OnPresent(nullptr);
}

static std::string ReadFile(const char* path)
{
	std::string s;
	if (FILE* f = fopen(path, "rb"))
	{
		char b[4096];
		size_t n;
		while ((n = fread(b, 1, sizeof(b), f)) > 0)
			s.append(b, n);
		fclose(f);
	}
	return s;
}

int main()
{
	char dir[MAX_PATH];
	GetFullPathNameA(".", MAX_PATH, dir, nullptr);
	std::string ini = std::string(dir) + "\\menu_test.ini";
	CHECK(CopyFileA("..\\..\\AC2ShadowOverhaul.ini", ini.c_str(), FALSE), "could not copy the ini");
	IniSetPath(ini.c_str());
	LogInit("menu_test.log", true);
	Taa_Init();
	Menu_Init();
	TaaSettings t0 = Taa_GetSettings();

	Press(VK_RIGHT); // hidden: ignored
	CHECK(Taa_GetSettings().on == t0.on, "a key acted while the menu was hidden");

	Press(VK_F1); // open
	Press('9'); // Debug view
	Press(VK_DOWN); // wraps to TAA
	Press(VK_DOWN); // Sharpness
	CHECK(g_selected == ITEM_SHARPNESS, "number key / arrows selected item %d", g_selected);
	Press(VK_RIGHT);
	Press(VK_RIGHT);
	float sharp = Taa_GetSettings().sharpness;
	printf("sharpness %.2f -> %.2f\n", t0.sharpness, sharp);
	CHECK(fabsf(sharp - (t0.sharpness + 0.10f)) < 1e-4f, "sharpness not changed live (%.3f)", sharp);
	Press(VK_UP); // TAA on/off
	Press(VK_LEFT);
	CHECK(Taa_GetSettings().on != t0.on, "on/off not switched");
	Press(VK_UP); // wraps to Debug view
	Press(VK_RIGHT);
	CHECK(Taa_GetSettings().debugView == 1, "debug view not changed");
	Press(VK_LEFT);

	CHECK(ReadFile(ini.c_str()).find("Sharpness=0.25") != std::string::npos, "saved before the delay");
	Sleep(1100);
	Menu_OnPresent(nullptr); // the delayed save happens on a poll
	std::string text = ReadFile(ini.c_str());
	CHECK(text.find("Sharpness=0.35") != std::string::npos, "Sharpness not saved");
	// The merged ini has StartEnabled / DebugView in several sections: read [TAA]'s.
	CHECK(GetPrivateProfileIntA("TAA", "StartEnabled", -1, ini.c_str()) == 0, "on/off not saved as [TAA] StartEnabled");
	CHECK(GetPrivateProfileIntA("TAA", "DebugView", -1, ini.c_str()) == 0, "DebugView not saved");
	CHECK(text.find("; Sharpening of the result") != std::string::npos, "comments lost");

	Press(VK_F1); // close
	Press(VK_RIGHT);
	CHECK(fabsf(Taa_GetSettings().sharpness - sharp) < 1e-4f && Taa_GetSettings().debugView == 0, "a key acted after closing");

	// A change right before exit is written by the flush.
	Press(VK_F1);
	Press(VK_DOWN); // from Debug view (last selected) wraps to TAA
	Press(VK_DOWN); // Sharpness
	Press(VK_LEFT);
	Menu_Flush();
	CHECK(ReadFile(ini.c_str()).find("Sharpness=0.30") != std::string::npos, "flush didn't save");
	printf(g_failures ? "FAILED (%d)\n" : "all passed\n", g_failures);
	DeleteFileA(ini.c_str());
	return g_failures ? 1 : 0;
}
