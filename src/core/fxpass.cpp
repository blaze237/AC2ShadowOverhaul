// See fxpass.h.
#include "core/fxpass.h"

namespace core
{

static DeviceFns g_fns;
void SetDeviceFns(const DeviceFns& fns) { g_fns = fns; }
const DeviceFns& Orig() { return g_fns; }

// ---- PassState -----------------------------------------------------------------------------------

HRESULT PassState::Create(IDirect3DDevice9* dev)
{
	Release();
	return dev->CreateStateBlock(D3DSBT_ALL, &m_block);
}

void PassState::Release()
{
	if (m_block)
		m_block->Release();
	m_block = nullptr;
}

void PassState::Save(IDirect3DDevice9* dev)
{
	m_block->Capture();
	for (DWORD i = 0; i < 4; i++)
		if (FAILED(dev->GetRenderTarget(i, &m_rt[i])))
			m_rt[i] = nullptr;
	if (FAILED(dev->GetDepthStencilSurface(&m_ds)))
		m_ds = nullptr;
	for (UINT i = 0; i < kStreams; i++)
	{
		m_freq[i] = 1;
		if (FAILED(dev->GetStreamSource(i, &m_vb[i], &m_offset[i], &m_stride[i])))
			m_vb[i] = nullptr;
		dev->GetStreamSourceFreq(i, &m_freq[i]);
	}
	if (FAILED(dev->GetIndices(&m_ib)))
		m_ib = nullptr;
	if (FAILED(dev->GetVertexDeclaration(&m_decl)))
		m_decl = nullptr;
	m_fvf = 0;
	dev->GetFVF(&m_fvf);
	for (UINT i = 0; i < kStreams; i++)
		if (m_freq[i] != 1)
			dev->SetStreamSourceFreq(i, 1);
}

void PassState::BeginPasses(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9* decl, IDirect3DVertexShader9* vs)
{
	const DWORD rs[][2] = {
		{ D3DRS_ZENABLE, FALSE }, { D3DRS_ZWRITEENABLE, FALSE }, { D3DRS_STENCILENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE },
		{ D3DRS_ALPHABLENDENABLE, FALSE }, { D3DRS_SEPARATEALPHABLENDENABLE, FALSE }, { D3DRS_CULLMODE, D3DCULL_NONE },
		{ D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_COLORWRITEENABLE, 0xF }, { D3DRS_SRGBWRITEENABLE, FALSE },
		{ D3DRS_FOGENABLE, FALSE }, { D3DRS_CLIPPLANEENABLE, 0 }, { D3DRS_FILLMODE, D3DFILL_SOLID },
		{ D3DRS_TWOSIDEDSTENCILMODE, FALSE }, { D3DRS_POINTSPRITEENABLE, FALSE },
	};
	for (const auto& r : rs)
		dev->SetRenderState((D3DRENDERSTATETYPE)r[0], r[1]);
	g_fns.SetDepthStencilSurface(dev, nullptr);
	for (DWORD i = 1; i < 4; i++)
		if (m_rt[i])
			g_fns.SetRenderTarget(dev, i, nullptr);
	dev->SetVertexDeclaration(decl);
	g_fns.SetVertexShader(dev, vs);
}

void PassState::Restore(IDirect3DDevice9* dev)
{
	for (DWORD i = 0; i < 4; i++)
		if (i == 0 || m_rt[i])
			g_fns.SetRenderTarget(dev, i, m_rt[i]);
	g_fns.SetDepthStencilSurface(dev, m_ds);
	m_block->Apply();
	if (m_decl)
		dev->SetVertexDeclaration(m_decl);
	else
		dev->SetFVF(m_fvf);
	for (UINT i = 0; i < kStreams; i++)
	{
		dev->SetStreamSource(i, m_vb[i], m_offset[i], m_stride[i]);
		dev->SetStreamSourceFreq(i, m_freq[i]);
		if (m_vb[i])
			m_vb[i]->Release();
		m_vb[i] = nullptr;
	}
	dev->SetIndices(m_ib);
	if (m_ib)
		m_ib->Release();
	if (m_decl)
		m_decl->Release();
	for (DWORD i = 0; i < 4; i++)
	{
		if (m_rt[i])
			m_rt[i]->Release();
		m_rt[i] = nullptr;
	}
	if (m_ds)
		m_ds->Release();
	m_ib = nullptr;
	m_decl = nullptr;
	m_ds = nullptr;
}

// ---- Fullscreen passes ---------------------------------------------------------------------------

IDirect3DVertexDeclaration9* FullscreenDecl(IDirect3DDevice9* dev)
{
	// Declarations aren't in D3DPOOL_DEFAULT: one per device, kept across resets.
	static IDirect3DDevice9* s_dev = nullptr;
	static IDirect3DVertexDeclaration9* s_decl = nullptr;
	if (s_decl && s_dev == dev)
		return s_decl;
	const D3DVERTEXELEMENT9 decl[] = { { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 }, D3DDECL_END() };
	IDirect3DVertexDeclaration9* d = nullptr;
	if (FAILED(dev->CreateVertexDeclaration(decl, &d)) || !d)
		return nullptr;
	s_dev = dev;
	s_decl = d;
	return d;
}

void DrawFullscreen(IDirect3DDevice9* dev)
{
	static const float tri[3][4] = { { -1, -1, 0, 1 }, { -1, 3, 0, 1 }, { 3, -1, 0, 1 } };
	g_fns.DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0]));
}

void SetTarget(IDirect3DDevice9* dev, IDirect3DSurface9* s, UINT w, UINT h)
{
	g_fns.SetRenderTarget(dev, 0, s);
	D3DVIEWPORT9 vp = { 0, 0, w, h, 0.0f, 1.0f };
	g_fns.SetViewport(dev, &vp);
}

void SetSamplerClamp(IDirect3DDevice9* dev, DWORD s, D3DTEXTUREFILTERTYPE filter)
{
	g_fns.SetSamplerState(dev, s, D3DSAMP_MINFILTER, filter);
	g_fns.SetSamplerState(dev, s, D3DSAMP_MAGFILTER, filter);
	g_fns.SetSamplerState(dev, s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	g_fns.SetSamplerState(dev, s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	g_fns.SetSamplerState(dev, s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	g_fns.SetSamplerState(dev, s, D3DSAMP_SRGBTEXTURE, FALSE);
	g_fns.SetSamplerState(dev, s, D3DSAMP_MAXMIPLEVEL, 0);
}

// ---- RenderTarget --------------------------------------------------------------------------------

HRESULT RenderTarget::Create(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt)
{
	Release();
	HRESULT hr = g_fns.CreateTexture(dev, w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &tex, NULL);
	if (SUCCEEDED(hr) && !tex)
		hr = E_FAIL;
	if (SUCCEEDED(hr))
		hr = tex->GetSurfaceLevel(0, &surf);
	if (SUCCEEDED(hr) && !surf)
		hr = E_FAIL;
	return hr;
}

void RenderTarget::Release()
{
	if (surf)
		surf->Release();
	if (tex)
		tex->Release();
	surf = nullptr;
	tex = nullptr;
}

} // namespace core
