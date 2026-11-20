// AC2ShadowOverhaul.asi - graphics overhaul for Assassin's Creed II (loaded by Ultimate ASI Loader):
// longer and softer shadows, ambient occlusion, contact shadows, screen-space global illumination,
// volumetric fog and sun shafts, and temporal anti-aliasing. Works alongside EaglePatch+; does not
// depend on it.
#include "common.h"
#include "softshadows.h"
#include "ao.h"
#include "features.h"
#include <stdio.h>
#include <string.h>
#include <string>

// Shows the reason the plugin isn't running. Logging is off by default, so a message box makes sure
// the player sees it; it runs on its own thread so the game isn't held up (and not under the loader lock).
static DWORD WINAPI ShowConflict(LPVOID text)
{
	MessageBoxA(NULL, (const char*)text, "AC2 Shadow Overhaul", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
	free(text);
	return 0;
}

// Plugins that are now part of this one (or older names of it). With any of them next to us the
// game would be patched twice, so we do nothing until they are deleted.
static bool OldPluginsPresent()
{
	static const char* const kOld[] = { "AC2Graphics", "AC2Shadows", "AC2SSGI", "AC2Volumetrics", "AC2TAA" };
	// Ultimate ASI Loader loads plugins from the game folder and its scripts and plugins folders.
	char exeDir[MAX_PATH];
	GetModuleFileNameA(NULL, exeDir, MAX_PATH);
	if (char* slash = strrchr(exeDir, '\\'))
		slash[1] = 0;
	std::string dirs[] = { ModuleDir(), exeDir, std::string(exeDir) + "scripts\\", std::string(exeDir) + "plugins\\" };
	std::string found;
	for (const char* oldName : kOld)
	{
		bool there = GetModuleHandleA((std::string(oldName) + ".asi").c_str()) != NULL;
		for (const std::string& d : dirs)
			there = there || GetFileAttributesA((d + oldName + ".asi").c_str()) != INVALID_FILE_ATTRIBUTES;
		if (!there)
			continue;
		if (!found.empty())
			found += ", ";
		found += oldName;
		found += ".asi";
	}
	if (found.empty())
		return false;
	LogWarn("Found %s next to AC2ShadowOverhaul.asi. These are now part of AC2ShadowOverhaul: delete them (and their .ini files). "
			"AC2ShadowOverhaul is doing nothing until then.",
		found.c_str());
	std::string msg = "AC2 Shadow Overhaul now includes SSGI, volumetrics and TAA, so the separate plugins must be removed.\n\n"
					  "Please delete these files from the game's scripts folder (and their .ini files):\n    " +
		found + "\n\nAC2 Shadow Overhaul is switched off until then.";
	char* text = _strdup(msg.c_str());
	HANDLE t = text ? CreateThread(NULL, 0, ShowConflict, text, 0, NULL) : NULL;
	if (t)
		CloseHandle(t);
	else
		free(text);
	return true;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
	static bool running = false;
	if (reason == DLL_PROCESS_DETACH && running)
	{
		Tuning_Flush();
		Fx_Flush();
		CrashLog_Detach(reserved != NULL);
	}
	if (reason != DLL_PROCESS_ATTACH)
		return TRUE;
	DisableThreadLibraryCalls(self);

	IniInit(self);
	char logPath[MAX_PATH];
	snprintf(logPath, sizeof(logPath), "%sAC2ShadowOverhaul.log", ModuleDir());
	LogInit(logPath, IniInt("Debug", "EnableLogging", 1) != 0);

	char exe[MAX_PATH];
	GetModuleFileNameA(NULL, exe, MAX_PATH);
	LogInfo("AC2ShadowOverhaul loaded into %s (ini: %s)", exe, IniPath());
	const char* name = strrchr(exe, '\\');
	name = name ? name + 1 : exe;
	if (_stricmp(name, "AssassinsCreedIIGame.exe") != 0)
	{
		LogWarn("Not AssassinsCreedIIGame.exe; doing nothing.");
		return TRUE;
	}
	if (OldPluginsPresent())
		return TRUE;

	running = true;
	CrashLog_Init();
	bool tuning = Tuning_Init();
	Shadows_Init(NULL, tuning);
	bool soft = SoftShadows_Init();
	bool ao = AO_Init();
	ao = CS_Init() || ao;
	bool fx = Fx_Init();
	FrameDump_Init(soft || tuning || ao || fx);
	return TRUE;
}
