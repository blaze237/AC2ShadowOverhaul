// AC2ShadowOverhaul ambient occlusion: GTAO ("Practical Real-Time Strategies for Accurate Indirect
// Occlusion", Jimenez et al. 2016; integration as in Intel's XeGTAO) computed at half resolution
// from the game's z/w target, a depth-aware blur, and a composite that removes the occluded share
// of the indirect light from the scene colour.
//
// Compiled by tools/gen_shaders.c into src/shaders_gen.h - run it after editing this file.
//
// Coordinates: full-res pixel p (integer, D3D9 pixel centres) samples at uv = (p + 0.5) / size and
// corresponds to NDC (2p / size - 1, 1 - 2p / size). View space: x right, y up, z forward (metres).

float4 c_Full   : register(c0); // full-res width, height, 1/width, 1/height
float4 c_Half   : register(c1); // AO-res width, height, 1/width, 1/height
float4 c_Proj   : register(c2); // A, B (z/w = A + B / z), P00, P11
float4 c_Proj2  : register(c3); // P02, P12 (off-centre), 0, 0
float4 c_Params : register(c4); // radius (m), pixels per (metre / z) [0.5 * P00 * width], max radius (px), falloff share
float4 c_Params2: register(c5); // strength (exponent), view mode, blur direction x, y

sampler2D s_Depth    : register(s0); // game z/w, R32F, full res (point)
sampler2D s_AO       : register(s1); // r = AO, g = view z (point)
sampler2D s_Indirect : register(s2); // indirect light x (1 - fog), full res (point)

// tests/ao_warp_test.cpp runs these shaders on D3D11 (WARP), where the pixel position is the pixel
// centre (x + 0.5) rather than D3D9's integer VPOS.
#ifdef AO_D3D11
#define VPOS_T float4
#define PIXEL(v) ((v).xy - 0.5)
#else
#define VPOS_T float2
#define PIXEL(v) (v)
#endif

#define SLICES 3
#define STEPS 6
static const float PI = 3.14159265;
static const float HALF_PI = 1.57079633;

float LinearZ(float d) { return c_Proj.y / (d - c_Proj.x); }

float FetchZ(float2 px) // full-res pixel coordinates
{
	return LinearZ(tex2Dlod(s_Depth, float4((px + 0.5) * c_Full.zw, 0, 0)).x);
}

float3 ViewPos(float2 px, float z)
{
	float2 ndc = float2(px.x * 2.0 * c_Full.z - 1.0, 1.0 - px.y * 2.0 * c_Full.w);
	return float3((ndc - c_Proj2.xy) / c_Proj.zw * z, z);
}

float FastAcos(float x)
{
	float a = abs(x);
	float r = (-0.156583 * a + HALF_PI) * sqrt(1.0 - a);
	return x >= 0 ? r : PI - r;
}

// ---- Fullscreen triangle -----------------------------------------------------------------------

float4 VS_Fullscreen(float4 p : POSITION) : POSITION
{
	return p;
}

// ---- GTAO (half res) ---------------------------------------------------------------------------
// Output: r = visibility (about 1 = open; noisy, may exceed 1), g = view z of the full-res pixel.

float4 PS_GTAO(VPOS_T vposIn : VPOS) : COLOR
{
	float2 vpos = PIXEL(vposIn);
	float2 px = vpos * 2.0; // top-left full-res pixel of this 2x2 block
	float d = tex2Dlod(s_Depth, float4((px + 0.5) * c_Full.zw, 0, 0)).x;
	float z = LinearZ(d);
	if (d >= 1.0)
		return float4(1, z, 0, 0); // sky

	float3 P = ViewPos(px, z);
	float3 V = normalize(-P);

	// Normal from the neighbouring pixels: on each axis the side with the smaller depth step, so
	// silhouettes don't bend it.
	float3 pl = ViewPos(px - float2(1, 0), FetchZ(px - float2(1, 0)));
	float3 pr = ViewPos(px + float2(1, 0), FetchZ(px + float2(1, 0)));
	float3 pu = ViewPos(px - float2(0, 1), FetchZ(px - float2(0, 1)));
	float3 pd = ViewPos(px + float2(0, 1), FetchZ(px + float2(0, 1)));
	float3 dx = abs(pr.z - P.z) < abs(P.z - pl.z) ? pr - P : P - pl;
	float3 dy = abs(pd.z - P.z) < abs(P.z - pu.z) ? pd - P : P - pu;
	float3 N = normalize(cross(dx, dy));
	N = dot(N, V) < 0 ? -N : N;

	float radiusPx = min(c_Params.x * c_Params.y / z, c_Params.z);
	if (radiusPx < 2.0)
		return float4(1, z, 0, 0); // too far away for the radius to cover a pixel

	// Per-pixel rotation and step offset (interleaved gradient noise; the blur removes the pattern).
	float noiseA = frac(52.9829189 * frac(dot(vpos, float2(0.06711056, 0.00583715))));
	float noiseB = frac(52.9829189 * frac(dot(vpos + float2(17.0, 31.0), float2(0.06711056, 0.00583715))));

	float falloffRange = c_Params.w * c_Params.x;
	float falloffMul = -1.0 / falloffRange;
	float falloffAdd = (c_Params.x - falloffRange) / falloffRange + 1.0;
	float minStep = 1.3 / radiusPx;

	float visibility = 0;
	[loop] for (int s = 0; s < SLICES; s++)
	{
		float phi = (s + noiseA) * (PI / SLICES);
		float2 omega = float2(cos(phi), -sin(phi)); // screen space, y down
		float3 dirV = float3(omega.x, -omega.y, 0);  // view space, y up
		float3 orthoDir = dirV - dot(dirV, V) * V;
		float3 axis = normalize(cross(orthoDir, V));
		float3 projN = N - axis * dot(N, axis);
		float projLen = length(projN);
		float cosN = saturate(dot(projN, V) / projLen);
		float n = sign(dot(orthoDir, projN)) * FastAcos(cosN);
		float lowCos0 = cos(n + HALF_PI), lowCos1 = cos(n - HALF_PI);
		float hc0 = lowCos0, hc1 = lowCos1;

		[loop] for (int j = 0; j < STEPS; j++)
		{
			float t = (j + noiseB) / STEPS;
			t = t * t + minStep;
			float2 o = round(omega * (t * radiusPx));
			float3 d0 = ViewPos(px + o, FetchZ(px + o)) - P;
			float3 d1 = ViewPos(px - o, FetchZ(px - o)) - P;
			float l0 = length(d0), l1 = length(d1);
			float w0 = saturate(l0 * falloffMul + falloffAdd), w1 = saturate(l1 * falloffMul + falloffAdd);
			hc0 = max(hc0, lerp(lowCos0, dot(d0, V) / l0, w0));
			hc1 = max(hc1, lerp(lowCos1, dot(d1, V) / l1, w1));
		}

		projLen = lerp(projLen, 1, 0.05);
		float h0 = -FastAcos(hc1);
		float h1 = FastAcos(hc0);
		h0 = n + clamp(h0 - n, -HALF_PI, HALF_PI);
		h1 = n + clamp(h1 - n, -HALF_PI, HALF_PI);
		float sinN = sin(n);
		float arc0 = (cosN + 2 * h0 * sinN - cos(2 * h0 - n)) / 4;
		float arc1 = (cosN + 2 * h1 * sinN - cos(2 * h1 - n)) / 4;
		visibility += projLen * (arc0 + arc1);
	}
	// Not clamped yet: single slices legitimately exceed 1 (surfaces seen at a glancing angle), and
	// clipping before the blur would bias the average towards "unoccluded".
	return float4(max(visibility / SLICES, 0), z, 0, 0);
}

// ---- Depth-aware blur (half res, one direction per pass) ---------------------------------------

float4 PS_Blur(VPOS_T vposIn : VPOS) : COLOR
{
	float2 vpos = PIXEL(vposIn);
	float2 uv = (vpos + 0.5) * c_Half.zw;
	float2 centre = tex2Dlod(s_AO, float4(uv, 0, 0)).xy;
	float2 stepUv = c_Params2.zw * c_Half.zw;
	float tolerance = 1.0 / (0.08 * centre.y);
	float sum = centre.x, wsum = 1.0;
	const float gauss[5] = { 1.0, 0.8825, 0.6065, 0.3247, 0.1353 }; // sigma 2
	[unroll] for (int i = 1; i <= 4; i++)
	{
		float2 a = tex2Dlod(s_AO, float4(uv + stepUv * i, 0, 0)).xy;
		float2 b = tex2Dlod(s_AO, float4(uv - stepUv * i, 0, 0)).xy;
		float wa = gauss[i] * saturate(1.0 - abs(a.y - centre.y) * tolerance);
		float wb = gauss[i] * saturate(1.0 - abs(b.y - centre.y) * tolerance);
		sum += a.x * wa + b.x * wb;
		wsum += wa + wb;
	}
	return float4(sum / wsum, centre.y, 0, 0);
}

// ---- Composite (full res) ----------------------------------------------------------------------
// Blended onto the scene with "destination - source": returns the indirect light to remove.
// View modes: 1 = AO only (no blending), 2 = the indirect light buffer (no blending).

float4 PS_Composite(VPOS_T vposIn : VPOS) : COLOR
{
	float2 vpos = PIXEL(vposIn);
	float2 uv = (vpos + 0.5) * c_Full.zw;
	float d = tex2Dlod(s_Depth, float4(uv, 0, 0)).x;
	float3 indirect = tex2Dlod(s_Indirect, float4(uv, 0, 0)).rgb;
	if (c_Params2.y > 1.5)
		return float4(indirect, 1);
	if (d >= 1.0)
		return c_Params2.y > 0.5 ? float4(1, 1, 1, 1) : float4(0, 0, 0, 0);
	float z = LinearZ(d);

	// AO texel i was computed for full-res pixel 2i: blend the four around this pixel, weighted
	// by distance and by how close their depth is to this pixel's.
	float2 h = vpos * 0.5;
	float2 i0 = floor(h);
	float2 f = h - i0;
	float2 uv0 = (i0 + 0.5) * c_Half.zw;
	float2 a00 = tex2Dlod(s_AO, float4(uv0, 0, 0)).xy;
	float2 a10 = tex2Dlod(s_AO, float4(uv0 + float2(c_Half.z, 0), 0, 0)).xy;
	float2 a01 = tex2Dlod(s_AO, float4(uv0 + float2(0, c_Half.w), 0, 0)).xy;
	float2 a11 = tex2Dlod(s_AO, float4(uv0 + c_Half.zw, 0, 0)).xy;
	float4 w = float4((1 - f.x) * (1 - f.y), f.x * (1 - f.y), (1 - f.x) * f.y, f.x * f.y) + 0.01;
	float4 dz = abs(float4(a00.y, a10.y, a01.y, a11.y) - z) / (0.05 * z);
	w *= saturate(1.0 - dz) + 0.001;
	float ao = dot(w, float4(a00.x, a10.x, a01.x, a11.x)) / dot(w, 1);
	ao = pow(saturate(ao), c_Params2.x);
	if (c_Params2.y > 0.5)
		return float4(ao, ao, ao, 1);
	return float4(indirect * (1.0 - ao), 0);
}

// ---- Frame dump: copy a depth texture (INTZ) into a colour target so it can be read back -------

sampler2D s_Copy : register(s0);

float4 PS_DepthCopy(float2 uv : TEXCOORD0) : COLOR
{
	return tex2D(s_Copy, uv).x;
}
