// AC2Shadows.asi - graphics tweaks for Assassin's Creed II (loaded by Ultimate ASI Loader).
// Works alongside EaglePatch+; does not depend on it.
#include "common.h"
#include "softshadows.h"
#include <stdio.h>
#include <string.h>

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_DETACH)
	{
		Tuning_Flush();
		CrashLog_Detach(reserved != NULL);
	}
	if (reason != DLL_PROCESS_ATTACH)
		return TRUE;
	DisableThreadLibraryCalls(self);

	IniInit(self);
	char logPath[MAX_PATH];
	snprintf(logPath, sizeof(logPath), "%sAC2Shadows.log", ModuleDir());
	LogInit(logPath, IniInt("Debug", "EnableLogging", 1) != 0);

	char exe[MAX_PATH];
	GetModuleFileNameA(NULL, exe, MAX_PATH);
	LogInfo("AC2Shadows loaded into %s (ini: %s)", exe, IniPath());
	const char* name = strrchr(exe, '\\');
	name = name ? name + 1 : exe;
	if (_stricmp(name, "AssassinsCreedIIGame.exe") != 0)
	{
		LogWarn("Not AssassinsCreedIIGame.exe; doing nothing.");
		return TRUE;
	}
	// This mod used to be called AC2Graphics. Both loaded would patch the game twice.
	char old[MAX_PATH];
	snprintf(old, sizeof(old), "%sAC2Graphics.asi", ModuleDir());
	if (GetFileAttributesA(old) != INVALID_FILE_ATTRIBUTES)
	{
		LogWarn("Found the old AC2Graphics.asi next to AC2Shadows.asi. Delete AC2Graphics.asi (and AC2Graphics.ini); "
				"AC2Shadows is doing nothing until then.");
		return TRUE;
	}

	CrashLog_Init();
	bool tuning = Tuning_Init();
	Shadows_Init(NULL, tuning);
	bool soft = SoftShadows_Init();
	FrameDump_Init(soft || tuning);
	return TRUE;
}
