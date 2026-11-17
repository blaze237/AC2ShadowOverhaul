#include "common.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

// ---- Logging ---------------------------------------------------------------------------------

static FILE* g_log = nullptr;
static CRITICAL_SECTION g_logLock;

void LogInit(const char* path, bool enabled)
{
	InitializeCriticalSection(&g_logLock);
	if (enabled)
		g_log = fopen(path, "w");
}

bool LogEnabled()
{
	return g_log != nullptr;
}

static void LogWrite(const char* level, const char* fmt, va_list args)
{
	if (!g_log)
		return;
	SYSTEMTIME t;
	GetLocalTime(&t);
	EnterCriticalSection(&g_logLock);
	fprintf(g_log, "[%02d:%02d:%02d.%03d] %s ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, level);
	vfprintf(g_log, fmt, args);
	fputc('\n', g_log);
	fflush(g_log);
	LeaveCriticalSection(&g_logLock);
}

void LogInfo(const char* fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	LogWrite("INFO ", fmt, a);
	va_end(a);
}

void LogWarn(const char* fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	LogWrite("WARN ", fmt, a);
	va_end(a);
}

// ---- INI -------------------------------------------------------------------------------------

static char g_iniPath[MAX_PATH];
static char g_moduleDir[MAX_PATH];

void IniInit(HMODULE self)
{
	char path[MAX_PATH];
	DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
	if (!n || n >= MAX_PATH)
		strcpy(path, "AC2Shadows.asi");
	strcpy(g_moduleDir, path);
	char* slash = strrchr(g_moduleDir, '\\');
	if (slash)
		slash[1] = 0;
	else
		g_moduleDir[0] = 0;

	char* dot = strrchr(path, '.');
	if (dot && (!slash || dot > path + (slash - g_moduleDir)))
		*dot = 0;
	snprintf(g_iniPath, MAX_PATH, "%s.ini", path);
}

void IniSetPath(const char* path)
{
	snprintf(g_iniPath, MAX_PATH, "%s", path);
}

const char* IniPath()
{
	return g_iniPath;
}

const char* ModuleDir()
{
	return g_moduleDir;
}

float IniFloat(const char* section, const char* key, float def)
{
	char buf[64];
	GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), g_iniPath);
	for (char* p = buf; *p; p++)
		if (*p == ',') // tolerate a decimal comma
			*p = '.';
	char* end;
	float v = strtof(buf, &end);
	return (end == buf) ? def : v;
}

int IniInt(const char* section, const char* key, int def)
{
	char buf[64];
	GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), g_iniPath);
	char* end;
	long v = strtol(buf, &end, 0);
	return (end == buf) ? def : (int)v;
}

// ---- Patching --------------------------------------------------------------------------------

bool WriteMemory(uintptr_t address, const void* data, size_t size)
{
	DWORD old;
	if (!VirtualProtect((void*)address, size, PAGE_EXECUTE_READWRITE, &old))
		return false;
	memcpy((void*)address, data, size);
	VirtualProtect((void*)address, size, old, &old);
	FlushInstructionCache(GetCurrentProcess(), (void*)address, size);
	return true;
}

bool InjectHook(uintptr_t address, const void* target, int type)
{
	unsigned char code[5];
	code[0] = (unsigned char)type;
	int32_t rel = (int32_t)((uintptr_t)target - address - 5);
	memcpy(code + 1, &rel, 4);
	return WriteMemory(address, code, 5);
}

bool Nop(uintptr_t address, size_t count)
{
	unsigned char nops[16];
	if (count > sizeof(nops))
		return false;
	memset(nops, 0x90, count);
	return WriteMemory(address, nops, count);
}

static int HexNibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

uintptr_t FindPattern(const char* pattern, int* outCount, HMODULE module)
{
	if (outCount)
		*outCount = 0;
	if (!pattern)
		return 0;

	unsigned char bytes[512];
	unsigned char mask[512]; // 0 = wildcard
	size_t len = 0;
	for (const char* p = pattern; *p;)
	{
		if (*p == ' ')
		{
			p++;
			continue;
		}
		if (len >= sizeof(bytes))
			return 0;
		if (p[0] == '?')
		{
			bytes[len] = 0;
			mask[len++] = 0;
			p += (p[1] == '?') ? 2 : 1;
			continue;
		}
		int hi = HexNibble(p[0]);
		int lo = p[1] ? HexNibble(p[1]) : -1;
		if (hi < 0 || lo < 0)
			return 0;
		bytes[len] = (unsigned char)((hi << 4) | lo);
		mask[len++] = 1;
		p += 2;
	}
	if (len == 0 || !mask[0])
		return 0;

	HMODULE base = module ? module : GetModuleHandleA(NULL);
	auto dos = (const IMAGE_DOS_HEADER*)base;
	if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE)
		return 0;
	auto nt = (const IMAGE_NT_HEADERS*)((const unsigned char*)base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return 0;

	uintptr_t first = 0;
	int count = 0;
	const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
	for (WORD s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++)
	{
		if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE))
			continue;
		const unsigned char* start = (const unsigned char*)base + sec->VirtualAddress;
		size_t size = sec->Misc.VirtualSize;
		if (size < len)
			continue;
		const unsigned char* end = start + size - len;
		for (const unsigned char* cur = start; cur <= end; cur++)
		{
			if (*cur != bytes[0])
				continue;
			size_t i = 1;
			while (i < len && (!mask[i] || cur[i] == bytes[i]))
				i++;
			if (i == len)
			{
				if (!first)
					first = (uintptr_t)cur;
				count++;
			}
		}
	}
	if (outCount)
		*outCount = count;
	return first;
}

uintptr_t FindUniquePattern(const char* name, const char* pattern, HMODULE module)
{
	int count = 0;
	uintptr_t addr = FindPattern(pattern, &count, module);
	if (count != 1)
	{
		LogWarn("Signature '%s' matched %d times (expected 1); skipping.", name, count);
		return 0;
	}
	return addr;
}

void* PatchImport(HMODULE module, const char* dll, const char* func, void* replacement)
{
	unsigned char* base = (unsigned char*)(module ? module : GetModuleHandleA(NULL));
	auto dos = (IMAGE_DOS_HEADER*)base;
	auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
	auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir.VirtualAddress)
		return nullptr;
	for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++)
	{
		if (_stricmp((const char*)(base + imp->Name), dll) != 0)
			continue;
		auto names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
		auto iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
		for (; names->u1.AddressOfData; names++, iat++)
		{
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
				continue;
			auto byName = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
			if (strcmp((const char*)byName->Name, func) != 0)
				continue;
			void* previous = (void*)iat->u1.Function;
			uintptr_t value = (uintptr_t)replacement;
			if (!WriteMemory((uintptr_t)&iat->u1.Function, &value, sizeof(value)))
				return nullptr;
			return previous;
		}
	}
	return nullptr;
}

bool GameHasFocus()
{
	DWORD pid = 0;
	HWND fg = GetForegroundWindow();
	if (fg)
		GetWindowThreadProcessId(fg, &pid);
	return pid == GetCurrentProcessId();
}
