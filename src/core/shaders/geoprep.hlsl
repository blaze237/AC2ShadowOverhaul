// Shared half-resolution geometry prep (core/geoprep.h): view-space normal and view z per 2x2 block
// of the game's z/w target, computed once per frame for ambient occlusion (GTAO, contact shadows)
// and SSGI (trace, temporal, blur).
//
// Compiled by tools/gen_shaders.c into src/core/shaders_gen.h - run it after editing this file.
//
// Output (A16B16G16R16F): xyz = view normal (unit, facing the camera), w = view z in metres
// (0 = sky: z/w >= 1). PS_GeoPrep: the normal from the depth; PS_GeoPrepNormals: the lit shaders'
// shading normal where there is a usable one, else the same (see below).
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

// The normal from the depth (P: the view position of full-res pixel px, z/w d < 1).
float3 DepthNormal(float2 px, float z, float3 P)
{
	float3 pl = ViewPos(px - float2(1, 0), FetchZ(px - float2(1, 0)));
	float3 pr = ViewPos(px + float2(1, 0), FetchZ(px + float2(1, 0)));
	float3 pu = ViewPos(px - float2(0, 1), FetchZ(px - float2(0, 1)));
	float3 pd = ViewPos(px + float2(0, 1), FetchZ(px + float2(0, 1)));
	float3 dx = abs(pr.z - P.z) < abs(P.z - pl.z) ? pr - P : P - pl;
	float3 dy = abs(pd.z - P.z) < abs(P.z - pu.z) ? pd - P : P - pu;
	float3 N = normalize(cross(dx, dy));
	return dot(N, P) > 0 ? -N : N;
}

float4 PS_GeoPrep(VPOS_T vposIn : VPOS) : COLOR
{
	float2 px = PIXEL(vposIn) * 2.0;
	float d = tex2Dlod(s_Depth, float4((px + 0.5) * c_Full.zw, 0, 0)).x;
	if (d >= 1.0)
		return float4(0, 0, -1, 0);
	float z = LinearZ(d);
	float3 P = ViewPos(px, z);
	return float4(DepthNormal(px, z, P), z);
}

// ---- Shading normals (core/geoprep.h) ----------------------------------------------------------
// The lit shaders' own normal of full-res pixel 2j (the same pixel as the depth), in the alpha of
// AO's two targets: octahedral (the world normal divided by |x| + |y| + |z|, the lower half folded
// over the diagonals), each component + 2, so 0 (cleared, or a shader without it) means none.

float4 c_ViewX : register(c4); // the view axes in world space: x right,
float4 c_ViewY : register(c5); // y up,
float4 c_ViewZ : register(c6); // z forward

sampler2D s_NormalA : register(s1); // AO's indirect light target: a = first component + 2 (point)
sampler2D s_NormalB : register(s2); // AO's sun light target: a = second component + 2 (point)

float3 OctDecode(float2 e)
{
	float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
	if (n.z < 0)
		n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0 ? 1.0 : -1.0, n.y >= 0 ? 1.0 : -1.0);
	return normalize(n);
}

// Where the stored normal is used: written (both components in [1, 3]; anything else is no normal
// or not ours) and within about 78 degrees of the depth's normal. Further off it isn't this
// surface's (a full-res pixel whose normal and depth came from different draws, or a silhouette
// pixel where the depth's normal is meaningless anyway: the depth's is the safe choice there) or
// is too far from the geometry for GTAO's horizons, which come from the depth, to make sense with
// it. Normal maps bend normals by much less than that.
static const float kAgree = 0.2;
// The lowest the normal may face the camera: normal maps and smoothed vertex normals can point away
// from the viewer on surfaces seen at a grazing angle. GTAO would then count nearly the whole slice
// as below the horizon (XeGTAO's projected-normal clamp limits but doesn't remove that), and the
// contact shadow ray would start into the surface. Such normals are bent towards the camera until
// they face it by this much (the depth's normal faces it by construction).
static const float kMinFacing = 0.05;

float4 PS_GeoPrepNormals(VPOS_T vposIn : VPOS) : COLOR
{
	float2 px = PIXEL(vposIn) * 2.0;
	float2 uv = (px + 0.5) * c_Full.zw;
	float d = tex2Dlod(s_Depth, float4(uv, 0, 0)).x;
	if (d >= 1.0)
		return float4(0, 0, -1, 0);
	float z = LinearZ(d);
	float3 P = ViewPos(px, z);
	float3 N = DepthNormal(px, z, P);
	float2 e = float2(tex2Dlod(s_NormalA, float4(uv, 0, 0)).a, tex2Dlod(s_NormalB, float4(uv, 0, 0)).a) - 2.0;
	if (all(abs(e) <= 1.001))
	{
		float3 w = OctDecode(clamp(e, -1.0, 1.0));
		float3 S = float3(dot(c_ViewX.xyz, w), dot(c_ViewY.xyz, w), dot(c_ViewZ.xyz, w));
		if (dot(S, N) >= kAgree)
		{
			float3 V = normalize(-P);
			float facing = dot(S, V);
			if (facing < kMinFacing)
				S = normalize(S + (kMinFacing - facing) * V);
			N = S;
		}
	}
	return float4(N, z);
}
