// AC2Graphics.asi - graphics tweaks for Assassin's Creed II (loaded by Ultimate ASI Loader).
// Works alongside EaglePatch+; does not depend on it.
#include "common.h"
#include <stdio.h>
#include <string.h>

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_DETACH)
		CrashLog_Detach(reserved != NULL);
	if (reason != DLL_PROCESS_ATTACH)
		return TRUE;
	DisableThreadLibraryCalls(self);

	IniInit(self);
	char logPath[MAX_PATH];
	snprintf(logPath, sizeof(logPath), "%sAC2Graphics.log", ModuleDir());
	LogInit(logPath, IniInt("Debug", "EnableLogging", 1) != 0);

	char exe[MAX_PATH];
	GetModuleFileNameA(NULL, exe, MAX_PATH);
	LogInfo("AC2Graphics loaded into %s (ini: %s)", exe, IniPath());
	const char* name = strrchr(exe, '\\');
	name = name ? name + 1 : exe;
	if (_stricmp(name, "AssassinsCreedIIGame.exe") != 0)
	{
		LogWarn("Not AssassinsCreedIIGame.exe; doing nothing.");
		return TRUE;
	}

	CrashLog_Init();
	Shadows_Init(NULL);
	FrameDump_Init();
	return TRUE;
}
