// Shared half-resolution geometry prep (core/geoprep.h): view-space normal and view z per 2x2 block
// of the game's z/w target, computed once per frame for ambient occlusion (GTAO, contact shadows)
// and SSGI (trace, temporal, blur).
//
// Compiled by tools/gen_shaders.c into src/core/shaders_gen.h - run it after editing this file.
//
// Output (A16B16G16R16F): xyz = view normal (unit, facing the camera), w = view z in metres
// (0 = sky: z/w >= 1).
//
// The rule is the one both effects used on their own before (GTAO in PS_GTAO, SSGI in PS_Prep;
// identical apart from the form of the facing test, dot(N, V) < 0 vs dot(N, P) > 0 with V = -P / |P|,
// which agree): half-res texel j stands for full-res pixel 2j (the top-left of its block, not an
// average, so the depth stays one real surface's), and the normal is the cross product of the
// position differences to the horizontal and vertical neighbours, each axis taking the side with the
// smaller depth step, so silhouettes don't bend it.
//
// Coordinates: full-res pixel p (integer, D3D9 pixel centres) samples at uv = (p + 0.5) / size and
// corresponds to NDC (2p / size - 1, 1 - 2p / size). View space: x right, y up, z forward (metres).

float4 c_Full  : register(c0); // full-res width, height, 1/width, 1/height
float4 c_Half  : register(c1); // half-res width, height, 1/width, 1/height
float4 c_Proj  : register(c2); // A, B (z/w = A + B / z), P00, P11
float4 c_Proj2 : register(c3); // P02, P12 (off-centre), -, -

sampler2D s_Depth : register(s0); // game z/w, R32F, full res (point)

// The WARP tests run this on D3D11, where the pixel position is the pixel centre (x + 0.5) rather
// than D3D9's integer VPOS.
#ifdef GEO_D3D11
#define VPOS_T float4
#define PIXEL(v) ((v).xy - 0.5)
#else
#define VPOS_T float2
#define PIXEL(v) (v)
#endif

float LinearZ(float d) { return c_Proj.y / (d - c_Proj.x); }

float3 ViewPos(float2 px, float z)
{
	float2 ndc = float2(px.x * 2.0 * c_Full.z - 1.0, 1.0 - px.y * 2.0 * c_Full.w);
	return float3((ndc - c_Proj2.xy) / c_Proj.zw * z, z);
}

float FetchZ(float2 px)
{
	return LinearZ(tex2Dlod(s_Depth, float4((px + 0.5) * c_Full.zw, 0, 0)).x);
}

float4 VS_Fullscreen(float4 p : POSITION) : POSITION
{
	return p;
}

float4 PS_GeoPrep(VPOS_T vposIn : VPOS) : COLOR
{
	float2 px = PIXEL(vposIn) * 2.0;
	float d = tex2Dlod(s_Depth, float4((px + 0.5) * c_Full.zw, 0, 0)).x;
	if (d >= 1.0)
		return float4(0, 0, -1, 0);
	float z = LinearZ(d);
	float3 P = ViewPos(px, z);
	float3 pl = ViewPos(px - float2(1, 0), FetchZ(px - float2(1, 0)));
	float3 pr = ViewPos(px + float2(1, 0), FetchZ(px + float2(1, 0)));
	float3 pu = ViewPos(px - float2(0, 1), FetchZ(px - float2(0, 1)));
	float3 pd = ViewPos(px + float2(0, 1), FetchZ(px + float2(0, 1)));
	float3 dx = abs(pr.z - P.z) < abs(P.z - pl.z) ? pr - P : P - pl;
	float3 dy = abs(pd.z - P.z) < abs(P.z - pu.z) ? pd - P : P - pu;
	float3 N = normalize(cross(dx, dy));
	N = dot(N, P) > 0 ? -N : N;
	return float4(N, z);
}
