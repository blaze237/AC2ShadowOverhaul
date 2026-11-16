// Crash logger: writes the faulting address, registers, likely call stack and the state of the
// 32-bit address space to AC2Graphics.log when the game crashes, so a crash report can be
// traced back to game code (addresses are printed as game VAs, i.e. relative to 0x400000).
#include "common.h"
#include <stdio.h>
#include <string.h>

static volatile LONG g_crashCount = 0;
static uintptr_t g_gameBase = 0, g_gameEnd = 0;

static void DescribeAddress(uintptr_t a, char* out, size_t n)
{
	if (a >= g_gameBase && a < g_gameEnd)
	{
		snprintf(out, n, "game:%08X", (unsigned)(a - g_gameBase + 0x400000));
		return;
	}
	HMODULE m = NULL;
	char path[MAX_PATH];
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m &&
		GetModuleFileNameA(m, path, MAX_PATH))
	{
		const char* name = strrchr(path, '\\');
		snprintf(out, n, "%s+%X", name ? name + 1 : path, (unsigned)(a - (uintptr_t)m));
		return;
	}
	snprintf(out, n, "%08X", (unsigned)a);
}

static bool IsCode(uintptr_t a)
{
	MEMORY_BASIC_INFORMATION mbi;
	if (!VirtualQuery((void*)a, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
		return false;
	return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static unsigned g_lastUsedMB = 0;

void LogMemoryState(const char* when)
{
	MEMORYSTATUSEX ms = { sizeof(ms) };
	GlobalMemoryStatusEx(&ms);
	size_t largestFree = 0, totalFree = 0;
	uintptr_t a = 0x10000;
	MEMORY_BASIC_INFORMATION mbi;
	while (a < 0xFFFF0000u && VirtualQuery((void*)a, &mbi, sizeof(mbi)))
	{
		if (mbi.State == MEM_FREE)
		{
			totalFree += mbi.RegionSize;
			if (mbi.RegionSize > largestFree)
				largestFree = mbi.RegionSize;
		}
		uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
		if (next <= a)
			break;
		a = next;
	}
	g_lastUsedMB = (unsigned)((ms.ullTotalVirtual - ms.ullAvailVirtual) >> 20);
	LogInfo("Memory (%s): address space used %u of %u MB, free %u MB, largest free block %u MB; system RAM free %u MB.", when,
		(unsigned)((ms.ullTotalVirtual - ms.ullAvailVirtual) >> 20), (unsigned)(ms.ullTotalVirtual >> 20), (unsigned)(totalFree >> 20),
		(unsigned)(largestFree >> 20), (unsigned)(ms.ullAvailPhys >> 20));
}

// Return addresses on the stack (heuristic: any dword pointing into executable code).
static void LogStack(uintptr_t esp)
{
	char line[1024];
	int len = 0, found = 0;
	const uintptr_t* sp = (const uintptr_t*)esp;
	for (int i = 0; i < 1024 && found < 24; i++)
	{
		uintptr_t v;
		__try
		{
			v = sp[i];
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			break;
		}
		if (v < 0x10000 || !IsCode(v))
			continue;
		char d[160];
		DescribeAddress(v, d, sizeof(d));
		len += snprintf(line + len, sizeof(line) - len, " %s", d);
		if (++found % 6 == 0)
		{
			LogWarn("  stack:%s", line);
			len = 0;
		}
	}
	if (len)
		LogWarn("  stack:%s", line);
}

static LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* e)
{
	DWORD code = e->ExceptionRecord->ExceptionCode;
	switch (code)
	{
	case EXCEPTION_ACCESS_VIOLATION:
	case EXCEPTION_ILLEGAL_INSTRUCTION:
	case EXCEPTION_PRIV_INSTRUCTION:
	case EXCEPTION_INT_DIVIDE_BY_ZERO:
	case EXCEPTION_STACK_OVERFLOW:
	case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
	case EXCEPTION_IN_PAGE_ERROR:
	case 0xE06D7363: // C++ exception (std::bad_alloc etc.)
		break;
	default:
		return EXCEPTION_CONTINUE_SEARCH;
	}
	// First-chance: the game and overlays (e.g. UPlay's MessageBus.dll) throw and handle C++
	// exceptions routinely, so those get a small budget of their own and can't use up the
	// budget for real faults.
	static volatile LONG cppCount = 0;
	if (!LogEnabled() || InterlockedIncrement(code == 0xE06D7363 ? &cppCount : &g_crashCount) > (code == 0xE06D7363 ? 2 : 10))
		return EXCEPTION_CONTINUE_SEARCH;

	CONTEXT* c = e->ContextRecord;
	char where[160];
	DescribeAddress((uintptr_t)e->ExceptionRecord->ExceptionAddress, where, sizeof(where));
	if (code == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters >= 2)
	{
		ULONG_PTR op = e->ExceptionRecord->ExceptionInformation[0];
		LogWarn("EXCEPTION 0x%08lX (access violation, %s %08X) at %s", code, op == 0 ? "reading" : op == 1 ? "writing" : "executing",
			(unsigned)e->ExceptionRecord->ExceptionInformation[1], where);
	}
	else
	{
		LogWarn("EXCEPTION 0x%08lX at %s%s", code, where, code == 0xE06D7363 ? " (C++ exception; usually handled, harmless)" : "");
	}
	LogWarn("  eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX", c->Eax, c->Ebx, c->Ecx, c->Edx,
		c->Esi, c->Edi, c->Ebp, c->Esp);
	LogStack(c->Esp);
	LogMemoryState("at exception");
	return EXCEPTION_CONTINUE_SEARCH;
}

// Logs the address space use whenever it moves by 64 MB, so a climb towards the 4 GB limit
// shows up in the log even if the game exits through its own out-of-memory path.
static DWORD WINAPI MemoryWatchThread(void*)
{
	for (;;)
	{
		Sleep(2000);
		MEMORYSTATUSEX ms = { sizeof(ms) };
		GlobalMemoryStatusEx(&ms);
		unsigned used = (unsigned)((ms.ullTotalVirtual - ms.ullAvailVirtual) >> 20);
		if (used > g_lastUsedMB + 64 || used + 64 < g_lastUsedMB)
			LogMemoryState("changed");
	}
	return 0;
}

// The game's own exit paths: logs who ended the process, so a shutdown without a hardware
// fault (e.g. an error dialog or a deliberate abort) still leaves a trace.
static void LogExit(const char* what, uintptr_t ret, unsigned code)
{
	char d[160];
	DescribeAddress(ret, d, sizeof(d));
	LogWarn("%s(%u / 0x%X) called from %s", what, code, code, d);
	uintptr_t here;
	__asm mov here, esp
	LogStack(here);
	LogMemoryState(what);
}

static void(WINAPI* g_origExitProcess)(UINT);
static decltype(&TerminateProcess) g_origTerminateProcess;
static void(WINAPI* g_origFatalAppExitA)(UINT, LPCSTR);
static decltype(&MessageBoxA) g_origMessageBoxA;

static void WINAPI ExitProcess_Hook(UINT code)
{
	LogExit("ExitProcess", (uintptr_t)__builtin_return_address(0), code);
	g_origExitProcess(code);
}

static BOOL WINAPI TerminateProcess_Hook(HANDLE process, UINT code)
{
	if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId())
		LogExit("TerminateProcess", (uintptr_t)__builtin_return_address(0), code);
	return g_origTerminateProcess(process, code);
}

static void WINAPI FatalAppExitA_Hook(UINT action, LPCSTR text)
{
	LogWarn("FatalAppExit: %s", text ? text : "(null)");
	LogExit("FatalAppExitA", (uintptr_t)__builtin_return_address(0), action);
	g_origFatalAppExitA(action, text);
}

static int WINAPI MessageBoxA_Hook(HWND wnd, LPCSTR text, LPCSTR caption, UINT type)
{
	char d[160];
	DescribeAddress((uintptr_t)__builtin_return_address(0), d, sizeof(d));
	LogWarn("MessageBox from %s: [%s] %s", d, caption ? caption : "", text ? text : "");
	return g_origMessageBoxA(wnd, text, caption, type);
}

template <typename T> static void HookImport(const char* dll, const char* func, T hook, T* orig)
{
	if (void* prev = PatchImport(NULL, dll, func, (void*)hook))
		*orig = (T)prev;
}

void CrashLog_Detach(bool processExit)
{
	LogInfo("AC2Graphics unloading (%s).", processExit ? "process exit" : "FreeLibrary");
	LogMemoryState("exit");
}

void CrashLog_Init()
{
	g_gameBase = (uintptr_t)GetModuleHandleA(NULL);
	auto dos = (IMAGE_DOS_HEADER*)g_gameBase;
	auto nt = (IMAGE_NT_HEADERS*)(g_gameBase + dos->e_lfanew);
	g_gameEnd = g_gameBase + nt->OptionalHeader.SizeOfImage;
	AddVectoredExceptionHandler(1, CrashHandler);
	LogMemoryState("startup");
	if (LogEnabled())
	{
		HookImport("KERNEL32.dll", "ExitProcess", ExitProcess_Hook, &g_origExitProcess);
		HookImport("KERNEL32.dll", "TerminateProcess", TerminateProcess_Hook, &g_origTerminateProcess);
		HookImport("KERNEL32.dll", "FatalAppExitA", FatalAppExitA_Hook, &g_origFatalAppExitA);
		HookImport("USER32.dll", "MessageBoxA", MessageBoxA_Hook, &g_origMessageBoxA);
		HANDLE t = CreateThread(NULL, 0, MemoryWatchThread, NULL, 0, NULL);
		if (t)
			CloseHandle(t);
	}
}
