// AC2Shadows - shared helpers (logging, ini, memory patching, signature scanning)
#pragma once

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

#define ASM(name) void __declspec(naked) name(void)

// ---- Logging ---------------------------------------------------------------------------------
void LogInit(const char* path, bool enabled);
bool LogEnabled();
void LogInfo(const char* fmt, ...);
void LogWarn(const char* fmt, ...);

// ---- INI -------------------------------------------------------------------------------------
// The ini lives next to the .asi with the same base name (AC2Shadows.ini).
void IniInit(HMODULE self);
void IniSetPath(const char* path); // tests only
const char* IniPath();
const char* ModuleDir();           // directory of the .asi, with trailing backslash
float IniFloat(const char* section, const char* key, float def);
int IniInt(const char* section, const char* key, int def); // accepts decimal or 0x hex
bool IniWrite(const char* section, const char* key, const char* value); // keeps comments and other keys

// ---- Patching --------------------------------------------------------------------------------
enum { PATCH_CALL = 0xE8, PATCH_JUMP = 0xE9 };
bool WriteMemory(uintptr_t address, const void* data, size_t size);
bool InjectHook(uintptr_t address, const void* target, int type);
bool Nop(uintptr_t address, size_t count);

// Scans the executable sections of `module` (NULL = game exe) for a byte pattern such as
// "8B 45 ?? 85 C0". Returns the first match; *outCount receives the number of matches.
uintptr_t FindPattern(const char* pattern, int* outCount = nullptr, HMODULE module = NULL);
// FindPattern that requires exactly one match; logs and returns 0 otherwise.
uintptr_t FindUniquePattern(const char* name, const char* pattern, HMODULE module = NULL);

// Replaces the import `dll!func` of `module` (NULL = game exe). Returns the previous pointer.
void* PatchImport(HMODULE module, const char* dll, const char* func, void* replacement);

// True when the foreground window belongs to the game (hotkeys are ignored otherwise).
bool GameHasFocus();

// ---- Features --------------------------------------------------------------------------------
void Shadows_Init(HMODULE patternModule, bool forceInstall = false);
// Live tuning: settings 0 = far cascade distance, 1 = inner cascade splits, 2 = caster distance,
// 3 = detail draw distance (the configured values; the F7 toggle switches them against vanilla).
bool Shadows_Installed();
bool Shadows_TweaksOn();
void Shadows_SetTweaksOn(bool on);
float Shadows_GetSetting(int which);
void Shadows_SetSetting(int which, float value);
// Debug menu (number keys select a setting, +/- change it, drawn top right).
bool Tuning_Init();
void Tuning_Flush(); // writes unsaved debug menu changes to the ini
bool Tuning_Enabled();
struct IDirect3DDevice9;
void Tuning_OnPresent(IDirect3DDevice9* dev, unsigned backBufferWidth, unsigned backBufferHeight);
void Tuning_OnLostDevice();
void Tuning_OnResetDevice();
void FrameDump_Init(bool deviceHooksNeeded);
void CrashLog_Init();
void CrashLog_Detach(bool processExit);
void LogMemoryState(const char* when);
