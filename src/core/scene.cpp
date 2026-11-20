// See scene.h.
#include "core/scene.h"
#include "core/shaderinfo.h"
#include "core/fxpass.h"
#include "common.h"
#include <math.h>
#include <unordered_map>

namespace core
{

static SceneState S;

const SceneState& Scene() { return S; }

bool Scene_MsaaRecently() { return (int)S.frame - S.msaaFrame < 120; }

bool Scene_CameraCut() { return S.camera.ok && S.prevCamera.ok && IsCameraCut(S.camera, S.prevCamera); }

ZwBinding InspectRt1(IDirect3DSurface9* s, IDirect3DSurface9* rt0)
{
	ZwBinding b;
	if (!s || !rt0)
		return b;
	D3DSURFACE_DESC d = {}, c = {};
	if (FAILED(s->GetDesc(&d)) || FAILED(rt0->GetDesc(&c)))
		return b;
	if (d.Format != D3DFMT_R32F || !(d.Usage & D3DUSAGE_RENDERTARGET) || d.MultiSampleType != D3DMULTISAMPLE_NONE ||
		c.Width != d.Width || c.Height != d.Height)
		return b;
	b.zw = true;
	b.width = d.Width;
	b.height = d.Height;
	b.colourMsaa = c.MultiSampleType != D3DMULTISAMPLE_NONE;
	IDirect3DTexture9* tex = nullptr;
	if (SUCCEEDED(s->GetContainer(kIID_Texture9, (void**)&tex)) && tex)
	{
		b.zwTexture = true;
		tex->Release();
	}
	tex = nullptr;
	if (SUCCEEDED(rt0->GetContainer(kIID_Texture9, (void**)&tex)) && tex)
	{
		b.colourTexture = tex;
		tex->Release(); // the game holds it
	}
	return b;
}

// ---- Vertex shaders: where the camera matrices are ------------------------------------------------

// Game vertex shader -> its g_WorldViewProj / g_World registers. Shaders are created on the loading
// threads too, so the map is locked.
static SRWLOCK g_vsLock = SRWLOCK_INIT;
static std::unordered_map<void*, CameraRegisters> g_vsMatrices;

void Scene_OnCreateVertexShader(IDirect3DVertexShader9* vs, const DWORD* code)
{
	if (!vs)
		return;
	ConstantTable t;
	CameraRegisters r;
	if (code && (code[0] & 0xFFFF0000) == 0xFFFE0000 && ReadConstantTable(code, t))
		r = FindCameraRegisters(t);
	AcquireSRWLockExclusive(&g_vsLock);
	if (r.Found())
		g_vsMatrices[vs] = r;
	else
		g_vsMatrices.erase(vs); // the address may have belonged to a released shader
	ReleaseSRWLockExclusive(&g_vsLock);
}

// ---- Pixel shaders: where the sun and fog constants are ---------------------------------------------

struct LightRegisters
{
	int sun = -1, sunCount = 0, fog = -1;
};
static SRWLOCK g_psLock = SRWLOCK_INIT;
static std::unordered_map<void*, LightRegisters> g_psLights;

void Scene_OnCreatePixelShader(IDirect3DPixelShader9* ps, const DWORD* code)
{
	if (!ps)
		return;
	ConstantTable t;
	LightRegisters r;
	if (code && (code[0] & 0xFFFF0000) == 0xFFFF0000 && ReadConstantTable(code, t))
	{
		if (const ShaderConstant* c = t.Find("g_ShadowedDirect", RS_FLOAT4)) // struct { m_Direction; m_Color; ... }
		{
			r.sun = c->reg;
			r.sunCount = c->count;
		}
		r.fog = t.Register("g_FogColor", RS_FLOAT4);
	}
	AcquireSRWLockExclusive(&g_psLock);
	if (r.sun >= 0 || r.fog >= 0)
		g_psLights[ps] = r;
	else
		g_psLights.erase(ps); // the address may have belonged to a released shader
	ReleaseSRWLockExclusive(&g_psLock);
}

static void CaptureLights(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps)
{
	if (!ps)
		return;
	LightRegisters r;
	AcquireSRWLockShared(&g_psLock);
	auto it = g_psLights.find(ps);
	if (it != g_psLights.end())
		r = it->second;
	ReleaseSRWLockShared(&g_psLock);
	float v[8];
	if (r.sun >= 0 && (!S.sunOk || (!S.sunColourOk && r.sunCount >= 2)) &&
		SUCCEEDED(dev->GetPixelShaderConstantF(r.sun, v, r.sunCount >= 2 ? 2 : 1)))
	{
		float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (len > 0.5f && len < 2.0f) // a direction (not an unset register)
		{
			for (int k = 0; k < 3; k++)
				S.sunDir[k] = v[k] / len;
			if (!S.sunKnown)
				LogInfo("Main pass: sun direction found (%.3f, %.3f, %.3f).", S.sunDir[0], S.sunDir[1], S.sunDir[2]);
			S.sunOk = S.sunKnown = true;
			if (r.sunCount >= 2)
			{
				for (int k = 0; k < 3; k++)
					S.sunColour[k] = v[4 + k] > 0 ? v[4 + k] : 0;
				S.sunColourOk = true;
			}
		}
	}
	if (r.fog >= 0 && !S.fogOk && SUCCEEDED(dev->GetPixelShaderConstantF(r.fog, S.fogColour, 1)))
		S.fogOk = true;
}

// ---- Camera ------------------------------------------------------------------------------------

static void CaptureCamera(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs)
{
	if (!vs)
		return;
	CameraRegisters r;
	AcquireSRWLockShared(&g_vsLock);
	auto it = g_vsMatrices.find(vs);
	if (it != g_vsMatrices.end())
		r = it->second;
	ReleaseSRWLockShared(&g_vsLock);
	if (!r.Found())
		return;
	// Needs a device that keeps shader constants readable (not a pure device).
	float wvp[16], world[16];
	if (FAILED(dev->GetVertexShaderConstantF(r.wvp, wvp, 4)) || FAILED(dev->GetVertexShaderConstantF(r.world, world, 4)))
		return;
	char why[160];
	Camera cam;
	if (!CameraFromMatrices(wvp, world, cam, why, sizeof(why)))
	{
		static bool warned = false;
		if (!warned)
		{
			warned = true;
			LogWarn("Main pass: could not read the camera from the shader constants (%s).", why);
		}
		return;
	}

	// ---- JITTER: the place for TAA's projection offset (not applied yet) ----------------------------
	// These matrices are the game's, unjittered: TAA moves the image by its sub-pixel offset inside the
	// vertex shaders (c255, see taa/vsjitter.h), not in the constants. So this camera and prevCamera
	// are unjittered, and so is every effect's reprojection. To make the effects jitter-aware, apply
	// the frame's offset here (TAA's current one to `camera`, the previous frame's is then in
	// prevCamera): in NDC, P02 += x and P12 += y (cam.f too), and viewProj rows 0 / 1 += x / y * row 3.

	static bool logged = false;
	if (!logged)
	{
		logged = true;
		LogInfo("Main pass: camera found (near %.3f m, far %.0f m, horizontal fov %.1f deg).", -cam.f.B / cam.f.A,
			cam.f.B / (1.0f - cam.f.A), 2.0 * atan(1.0 / cam.f.P00) * 57.29578);
	}
	S.camera = cam;
}

// ---- Hooks ---------------------------------------------------------------------------------------

void Scene_AfterSetRenderTarget(DWORD index, IDirect3DSurface9* s)
{
	if (index == 0)
	{
		S.rt0 = s;
		if (s)
		{
			// D3D9 resets the viewport to the full target when RT0 changes.
			D3DSURFACE_DESC d;
			if (SUCCEEDED(s->GetDesc(&d)))
				S.viewport = { 0, 0, d.Width, d.Height, 0.0f, 1.0f };
		}
		return;
	}
	if (index != 1)
		return;
	S.rt1 = s;
	if (!s)
	{
		// Between batches, or the end of the opaque pass.
		S.inOpaquePass = false;
		return;
	}
	// The game's anti-aliasing: its z/w target or the colour next to it is multisampled. (Any R32F
	// render target as RT1 counts here, so the warning shows even when the rest doesn't match.)
	D3DSURFACE_DESC d;
	if (SUCCEEDED(s->GetDesc(&d)) && d.Format == D3DFMT_R32F && (d.Usage & D3DUSAGE_RENDERTARGET))
	{
		D3DSURFACE_DESC c;
		if (d.MultiSampleType != D3DMULTISAMPLE_NONE || (S.rt0 && SUCCEEDED(S.rt0->GetDesc(&c)) && c.MultiSampleType != D3DMULTISAMPLE_NONE))
			S.msaaFrame = (int)S.frame;
	}
	// Binding something else as RT1 doesn't end the pass (only unbinding does), as in the effects.
	const ZwBinding b = InspectRt1(s, S.rt0);
	if (!b.MainPass())
		return;
	S.passSeen = S.inOpaquePass = true;
	S.sceneColour = S.rt0;
	S.sceneColourTexture = b.colourTexture;
	S.zw = s;
	S.width = b.width;
	S.height = b.height;
}

void Scene_AfterSetViewport(const D3DVIEWPORT9* vp)
{
	if (vp)
		S.viewport = *vp;
}

void Scene_BeforeDraw(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs, IDirect3DPixelShader9* ps)
{
	if (S.inOpaquePass)
	{
		// The first opaque draw with the camera matrices sets this frame's camera; the first ones
		// with the sun and fog constants, the sun and fog (needs a device whose constants can be
		// read back, not a pure device).
		if (!S.camera.ok)
			CaptureCamera(dev, vs);
		if (!S.sunOk || !S.sunColourOk || !S.fogOk)
			CaptureLights(dev, ps);
		return;
	}
	if (S.passSeen && S.rt0 == S.sceneColour && IsSkyViewport(S.viewport))
		S.skySeen = true;
}

void Scene_EndFrame()
{
	if (S.camera.ok)
		S.prevCamera = S.camera;
	S.camera.ok = false;
	S.sunOk = S.sunColourOk = S.fogOk = false;
	S.passSeen = S.inOpaquePass = S.skySeen = false;
	S.sceneColour = nullptr;
	S.sceneColourTexture = nullptr;
	S.zw = nullptr;
	S.frame++;
}

void Scene_OnLostDevice()
{
	S.passSeen = S.inOpaquePass = S.skySeen = false;
	S.sceneColour = nullptr;
	S.sceneColourTexture = nullptr;
	S.zw = nullptr;
	S.camera.ok = S.prevCamera.ok = false;
	S.sunOk = S.sunColourOk = S.fogOk = false;
}

void Scene_OnDeviceReady(IDirect3DSurface9* backBuffer)
{
	Scene_OnLostDevice();
	S.rt0 = backBuffer;
	S.rt1 = nullptr;
	D3DSURFACE_DESC d;
	if (backBuffer && SUCCEEDED(backBuffer->GetDesc(&d)))
		S.viewport = { 0, 0, d.Width, d.Height, 0.0f, 1.0f };
}

} // namespace core
