// AC2ShadowOverhaul.asi - graphics tweaks for Assassin's Creed II (loaded by Ultimate ASI Loader).
// Works alongside EaglePatch+; does not depend on it.
#include "common.h"
#include "softshadows.h"
#include "ao.h"
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
	// This mod used to be called AC2Graphics, then AC2Shadows. Loading an old copy as well would patch the game twice.
	for (const char* oldName : { "AC2Graphics", "AC2Shadows" })
	{
		char old[MAX_PATH];
		snprintf(old, sizeof(old), "%s%s.asi", ModuleDir(), oldName);
		if (GetFileAttributesA(old) != INVALID_FILE_ATTRIBUTES)
		{
			LogWarn("Found the old %s.asi next to AC2ShadowOverhaul.asi. Delete %s.asi (and %s.ini); "
					"AC2ShadowOverhaul is doing nothing until then.",
				oldName, oldName, oldName);
			return TRUE;
		}
	}

	CrashLog_Init();
	bool tuning = Tuning_Init();
	Shadows_Init(NULL, tuning);
	bool soft = SoftShadows_Init();
	bool ao = AO_Init();
	FrameDump_Init(soft || tuning || ao);
	return TRUE;
}
