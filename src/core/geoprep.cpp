// See geoprep.h.
#include "core/geoprep.h"
#include "core/fxpass.h"
#include "core/scene.h"
#include "core/shaders_gen.h"
#include "common.h"

namespace core
{

static RenderTarget g_geo[2];
static IDirect3DPixelShader9* g_ps;
static UINT g_w, g_h;          // full-res size the textures are for
static int g_cur = 0;          // the texture written last
static unsigned g_frame = 0;   // Scene().frame it was written in
static bool g_written = false; // g_geo[g_cur] holds that frame's geometry
static bool g_prevOk = false;  // g_geo[1 - g_cur] holds the frame before's
static bool g_failed = false;
static unsigned g_runs = 0;

static void Release()
{
	for (RenderTarget& t : g_geo)
		t.Release();
	g_w = g_h = 0;
	g_written = g_prevOk = false;
}

void Geo_OnLostDevice() { Release(); }

unsigned Geo_Runs() { return g_runs; }

IDirect3DTexture9* Geo_Current() { return g_written && g_frame == Scene().frame ? g_geo[g_cur].tex : nullptr; }

IDirect3DTexture9* Geo_Previous() { return Geo_Current() && g_prevOk ? g_geo[1 - g_cur].tex : nullptr; }

IDirect3DTexture9* Geo_Ensure(IDirect3DDevice9* dev, IDirect3DTexture9* zw, UINT w, UINT h, const CameraFloats& cam)
{
	if (IDirect3DTexture9* t = Geo_Current())
		if (g_w == w && g_h == h)
			return t;
	if (g_failed || !zw)
		return nullptr;
	const DeviceFns& D = Orig();
	const UINT hw = (w + 1) / 2, hh = (h + 1) / 2;
	if (!g_ps)
	{
		HRESULT hr = D.CreatePixelShader(dev, kGeoPrepPs, &g_ps);
		if (FAILED(hr))
		{
			LogWarn("Shared geometry prep: could not create its shader (0x%08lX); ambient occlusion and SSGI are off.", hr);
			g_failed = true;
			return nullptr;
		}
	}
	const unsigned frame = Scene().frame;
	const bool lastFrame = g_written && g_frame + 1 == frame;
	if (!g_geo[0].tex || g_w != w || g_h != h)
	{
		Release();
		HRESULT hr;
		if (FAILED(hr = g_geo[0].Create(dev, hw, hh, D3DFMT_A16B16G16R16F)) || FAILED(hr = g_geo[1].Create(dev, hw, hh, D3DFMT_A16B16G16R16F)))
		{
			LogWarn("Shared geometry prep: could not create its render targets (0x%08lX).", hr);
			Release();
			return nullptr; // tried again next frame (may be a passing shortage of video memory)
		}
		g_w = w;
		g_h = h;
		LogInfo("Shared geometry prep: %ux%u normals and depth for %ux%u.", hw, hh, w, h);
	}
	else
	{
		g_prevOk = lastFrame;
		g_cur = 1 - g_cur;
	}
	const float c[4][4] = {
		{ (float)w, (float)h, 1.0f / w, 1.0f / h },
		{ (float)hw, (float)hh, 1.0f / hw, 1.0f / hh },
		{ cam.A, cam.B, cam.P00, cam.P11 },
		{ cam.P02, cam.P12, 0, 0 },
	};
	dev->SetPixelShaderConstantF(0, &c[0][0], 4);
	D.SetTexture(dev, 0, zw);
	SetSamplerClamp(dev, 0, D3DTEXF_POINT);
	SetTarget(dev, g_geo[g_cur].surf, hw, hh);
	D.SetPixelShader(dev, g_ps);
	DrawFullscreen(dev);
	g_frame = frame;
	g_written = true;
	g_runs++;
	return g_geo[g_cur].tex;
}

} // namespace core
