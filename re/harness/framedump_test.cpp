// End-to-end test for the frame dump: built as AssassinsCreedIIGame.exe, statically imports
// d3d9!Direct3DCreate9 like the game, loads AC2Graphics.asi, renders a few frames with extra
// render targets / depth buffers, and taps the hotkey (F8) via SendInput.
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

// No interactive desktop here, so fake the hotkey by replacing the asi's GetAsyncKeyState import.
static bool g_keyDown = false;
static SHORT WINAPI FakeGetAsyncKeyState(int vk)
{
	return (vk == VK_F8 && g_keyDown) ? (SHORT)0x8000 : 0;
}

static bool PatchAsiImport(HMODULE mod, const char* func, void* repl)
{
	unsigned char* base = (unsigned char*)mod;
	auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
	auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++)
	{
		auto names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
		auto iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
		for (; names->u1.AddressOfData; names++, iat++)
		{
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
				continue;
			if (strcmp((const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name, func) == 0)
			{
				DWORD old;
				VirtualProtect(&iat->u1.Function, 4, PAGE_READWRITE, &old);
				iat->u1.Function = (uintptr_t)repl;
				return true;
			}
		}
	}
	return false;
}

static HWND g_wnd;
static HWND WINAPI FakeGetForegroundWindow()
{
	return g_wnd;
}

static void Tap(WORD, bool down)
{
	g_keyDown = down;
}

int main()
{
	HMODULE asi = LoadLibraryA("scripts\\AC2Graphics.asi");
	printf("asi %p\n", (void*)asi);
	if (!asi)
		return 1;
	printf("key hook %d\n", PatchAsiImport(asi, "GetAsyncKeyState", (void*)FakeGetAsyncKeyState));

	HWND wnd = CreateWindowA("STATIC", "fd", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, NULL, NULL, NULL, NULL);
	g_wnd = wnd;
	printf("focus hook %d\n", PatchAsiImport(asi, "GetForegroundWindow", (void*)FakeGetForegroundWindow));
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	D3DADAPTER_IDENTIFIER9 id = {};
	d3d->GetAdapterIdentifier(0, 0, &id);
	printf("adapters %u: %s, wnd %p\n", d3d->GetAdapterCount(), id.Description, (void*)wnd);
	D3DPRESENT_PARAMETERS pp = {};
	pp.Windowed = TRUE;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.BackBufferFormat = D3DFMT_X8R8G8B8;
	pp.BackBufferWidth = 320;
	pp.BackBufferHeight = 240;
	pp.EnableAutoDepthStencil = TRUE;
	pp.AutoDepthStencilFormat = D3DFMT_D24S8;
	IDirect3DDevice9* dev = nullptr;
	HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
	printf("CreateDevice 0x%08lX\n", hr);
	if (FAILED(hr))
		return 2;

	IDirect3DTexture9 *scene = nullptr, *depthTex = nullptr;
	IDirect3DSurface9* depth2 = nullptr;
	IDirect3DTexture9* linDepth = nullptr;
	dev->CreateTexture(320, 240, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &linDepth, NULL);
	dev->CreateTexture(320, 240, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &scene, NULL);
	hr = dev->CreateTexture(320, 240, 1, D3DUSAGE_DEPTHSTENCIL, (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'), D3DPOOL_DEFAULT, &depthTex, NULL);
	printf("INTZ 0x%08lX\n", hr);
	dev->CreateDepthStencilSurface(320, 240, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &depth2, NULL);
	IDirect3DSurface9 *sceneSurf = nullptr, *bb = nullptr, *autoDs = nullptr;
	scene->GetSurfaceLevel(0, &sceneSurf);
	dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
	dev->GetDepthStencilSurface(&autoDs);

	struct V { float x, y, z, rhw; } tri[3] = { { 10, 10, 0.5f, 1 }, { 200, 10, 0.5f, 1 }, { 10, 200, 0.5f, 1 } };
	dev->SetFVF(D3DFVF_XYZRHW);
	static const DWORD psCode[] = { 0xFFFF0300, 0x0000FFFF };
	IDirect3DPixelShader9* ps = nullptr;
	dev->CreatePixelShader(psCode, &ps);

	for (int frame = 0; frame < 6; frame++)
	{
		if (frame == 1)
			Tap(VK_F8, true);
		if (frame == 2)
			Tap(VK_F8, false);
		dev->BeginScene();
		dev->SetRenderTarget(0, sceneSurf);
		dev->SetDepthStencilSurface(depth2);
		dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
		for (int i = 0; i < 5; i++)
			dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(V));
		dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(V));
		dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		dev->SetRenderTarget(0, bb);
		dev->SetDepthStencilSurface(autoDs);
		dev->SetTexture(0, scene);
		dev->SetTexture(1, depthTex);
		dev->SetTexture(8, linDepth);
		dev->SetPixelShader(ps);
		dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(V));
		dev->SetTexture(0, NULL);
		dev->SetTexture(1, NULL);
		dev->SetPixelShader(NULL);
		dev->StretchRect(sceneSurf, NULL, bb, NULL, D3DTEXF_LINEAR);
		dev->EndScene();
		dev->Present(NULL, NULL, NULL, NULL);
		Sleep(30);
	}
	pp.BackBufferWidth = 400;
	sceneSurf->Release();
	scene->Release();
	depthTex->Release();
	depth2->Release();
	bb->Release();
	autoDs->Release();
	hr = dev->Reset(&pp);
	printf("Reset 0x%08lX\n", hr);
	dev->Release();
	d3d->Release();
	return 0;
}
