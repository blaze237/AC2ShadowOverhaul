// See inputblock.h.
#define DIRECTINPUT_VERSION 0x0800
#include "core/inputblock.h"
#include "core/menu.h"
#include "common.h"
#include <dinput.h>
#include <string.h>

namespace core
{

static const GUID kGUID_SysKeyboard = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const BYTE kArrowsDik[4] = { DIK_UP, DIK_DOWN, DIK_LEFT, DIK_RIGHT };

static bool Blocking() { return MenuOwner() != MENU_NONE; }

static bool IsArrowDik(DWORD ofs)
{
	for (BYTE k : kArrowsDik)
		if (ofs == k)
			return true;
	return false;
}

// ---- DirectInput -------------------------------------------------------------------------------------

typedef HRESULT(__stdcall* PFN_GetDeviceState)(IDirectInputDevice8A*, DWORD, LPVOID);
typedef HRESULT(__stdcall* PFN_GetDeviceData)(IDirectInputDevice8A*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
typedef HRESULT(__stdcall* PFN_CreateDevice)(IDirectInput8A*, REFGUID, LPDIRECTINPUTDEVICE8A*, LPUNKNOWN);
typedef HRESULT(WINAPI* PFN_DirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

static PFN_GetDeviceState o_GetDeviceState;
static PFN_GetDeviceData o_GetDeviceData;
static PFN_CreateDevice o_CreateDevice;
static PFN_DirectInput8Create o_DirectInput8Create;
static void* volatile g_keyboard; // the game's keyboard device (the vtable is shared with its mouse)

static HRESULT __stdcall h_GetDeviceState(IDirectInputDevice8A* d, DWORD size, LPVOID data)
{
	HRESULT hr = o_GetDeviceState(d, size, data);
	if (SUCCEEDED(hr) && data && d == g_keyboard && size >= 256 && Blocking())
		for (BYTE k : kArrowsDik)
			((BYTE*)data)[k] = 0;
	return hr;
}

static HRESULT __stdcall h_GetDeviceData(IDirectInputDevice8A* d, DWORD size, LPDIDEVICEOBJECTDATA data, LPDWORD inOut, DWORD flags)
{
	HRESULT hr = o_GetDeviceData(d, size, data, inOut, flags);
	if (FAILED(hr) || !data || !inOut || d != g_keyboard || size < sizeof(DWORD) || !Blocking())
		return hr;
	// Drop the arrow key events, keeping the rest in order.
	DWORD kept = 0;
	for (DWORD i = 0; i < *inOut; i++)
	{
		BYTE* e = (BYTE*)data + (size_t)i * size;
		if (IsArrowDik(*(DWORD*)e))
			continue;
		if (kept != i)
			memmove((BYTE*)data + (size_t)kept * size, e, size);
		kept++;
	}
	*inOut = kept;
	return hr;
}

template <typename T> static void HookVtbl(void** vtbl, int index, T hook, T& orig)
{
	if (vtbl[index] == (void*)hook)
		return;
	orig = (T)vtbl[index];
	void* p = (void*)hook;
	WriteMemory((uintptr_t)&vtbl[index], &p, sizeof(p));
}

static HRESULT __stdcall h_CreateDevice(IDirectInput8A* di, REFGUID guid, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN outer)
{
	HRESULT hr = o_CreateDevice(di, guid, out, outer);
	if (SUCCEEDED(hr) && out && *out && IsEqualGUID(guid, kGUID_SysKeyboard))
	{
		g_keyboard = *out;
		void** vt = *(void***)*out;
		HookVtbl(vt, 9, h_GetDeviceState, o_GetDeviceState); // IDirectInputDevice8::GetDeviceState
		HookVtbl(vt, 10, h_GetDeviceData, o_GetDeviceData);  // ::GetDeviceData
		LogInfo("Menus: the game's DirectInput keyboard found; arrow keys go to the menus only while one is open.");
	}
	return hr;
}

static HRESULT WINAPI h_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID iid, LPVOID* out, LPUNKNOWN outer)
{
	HRESULT hr = o_DirectInput8Create(inst, version, iid, out, outer);
	if (SUCCEEDED(hr) && out && *out)
		HookVtbl(*(void***)*out, 3, h_CreateDevice, o_CreateDevice); // IDirectInput8::CreateDevice (A and W alike)
	return hr;
}

// ---- GetAsyncKeyState ----------------------------------------------------------------------------

typedef SHORT(WINAPI* PFN_GetAsyncKeyState)(int);
static PFN_GetAsyncKeyState o_GetAsyncKeyState;

static SHORT WINAPI h_GetAsyncKeyState(int vk)
{
	if ((vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT) && Blocking())
		return 0;
	return o_GetAsyncKeyState(vk);
}

void InputBlock_Init()
{
	o_DirectInput8Create = (PFN_DirectInput8Create)PatchImport(NULL, "dinput8.dll", "DirectInput8Create", (void*)h_DirectInput8Create);
	o_GetAsyncKeyState = (PFN_GetAsyncKeyState)PatchImport(NULL, "user32.dll", "GetAsyncKeyState", (void*)h_GetAsyncKeyState);
	if (!o_DirectInput8Create)
		LogWarn("Menus: the game's DirectInput8Create import wasn't found; the arrow keys reach the game while a menu is open.");
	if (!o_GetAsyncKeyState)
		o_GetAsyncKeyState = GetAsyncKeyState;
}

} // namespace core
