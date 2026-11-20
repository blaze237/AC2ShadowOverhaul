// AC2ShadowOverhaul ambient occlusion and contact shadows: GTAO ("Practical Real-Time Strategies
// for Accurate Indirect Occlusion", Jimenez et al. 2016; integration as in Intel's XeGTAO) and a
// short ray march towards the sun, both at half resolution from the game's z/w target (normals from
// the geometry prep shared with SSGI, src/core/shaders/geoprep.hlsl), a
// depth-aware blur, and a composite that removes the occluded share of the indirect light and the
// contact-shadowed share of the sun light from the scene colour.
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
float4 c_Params2: register(c5); // AO strength (exponent; 0 = AO off), view mode, blur direction x, y
float4 c_Sun    : register(c6); // direction towards the sun (view space), contact shadow length (m; 0 = off)
float4 c_Contact: register(c7); // thickness (m) = x + y * distance, strength, depth bias per metre of distance

sampler2D s_Depth    : register(s0); // game z/w, R32F, full res (point)
sampler2D s_Geo      : register(s1); // GTAO: the shared geometry prep (core/geoprep.h): view normal, view z (point)
sampler2D s_AO       : register(s1); // Blur / Composite: r = AO, g = view z, b = contact shadow (point)
sampler2D s_Indirect : register(s2); // indirect light x (1 - fog), full res (point)
sampler2D s_Sun      : register(s3); // shadowed sun light x (1 - fog), full res (point)

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
#define CS_STEPS 16
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

// ---- Contact shadows ---------------------------------------------------------------------------
// March from the surface towards the sun; anything in the depth buffer a little in front of the
// ray blocks it. "A little": closer to the camera than the ray by more than a bias (so a surface
// doesn't shadow itself) and by less than `thickness` (the depth buffer only has front surfaces,
// so everything is assumed to be this thick; larger values make halos behind thin objects such as
// legs). Both scale with the depth step between neighbouring pixels on this surface, which grows
// steeply at glancing angles. An occluder found further along the ray shadows less, so the
// shadow fades out towards `length` instead of ending in a hard edge.
// Returns the light that gets through (1 = unshadowed).

float ContactShadow(float3 P, float3 N, float z, float noise)
{
	float len = c_Sun.w;
	float3 L = c_Sun.xyz;
	if (len <= 0 || dot(N, L) <= 0.02)
		return 1; // off, or facing away from the sun (the game's own lighting already darkens it)
	float NdotV = saturate(dot(N, normalize(-P)));
	float pixel = 2.0 * z * c_Full.w / c_Proj.w;  // metres per pixel at this distance
	float slope = pixel / max(NdotV, 0.08);        // depth step per pixel on this surface
	float bias = c_Contact.w * z + 2.0 * slope;
	float3 start = P + N * (0.5 * slope);         // lift off the surface by half a pixel's depth step
	[loop] for (int i = 0; i < CS_STEPS; i++)
	{
		float t = (i + noise) / CS_STEPS * len;
		float3 Q = start + L * t;
		if (Q.z < 0.05)
			break;
		float2 ndc = Q.xy * c_Proj.zw / Q.z + c_Proj2.xy;
		float2 qpx = float2(ndc.x + 1.0, 1.0 - ndc.y) * 0.5 * c_Full.xy;
		if (qpx.x < 0 || qpx.y < 0 || qpx.x >= c_Full.x || qpx.y >= c_Full.y)
			break;
		float dz = Q.z - FetchZ(floor(qpx + 0.5));
		if (dz > bias && dz < c_Contact.x + c_Contact.y * Q.z + bias)
		{
			float f = t / len;
			return 1.0 - c_Contact.z * (1.0 - f * f); // dark at the contact, fading out towards `length`
		}
	}
	return 1;
}

// ---- GTAO + contact shadows (half res) ---------------------------------------------------------
// Output: r = AO visibility (about 1 = open; noisy, may exceed 1), g = view z of the full-res pixel,
// b = contact shadow (1 = lit).

float4 PS_GTAO(VPOS_T vposIn : VPOS) : COLOR
{
	float2 vpos = PIXEL(vposIn);
	float2 px = vpos * 2.0; // top-left full-res pixel of this 2x2 block
	float d = tex2Dlod(s_Depth, float4((px + 0.5) * c_Full.zw, 0, 0)).x;
	float z = LinearZ(d);
	if (d >= 1.0)
		return float4(1, z, 1, 0); // sky

	float3 P = ViewPos(px, z);
	float3 V = normalize(-P);

	// The normal from the shared geometry prep (the same full-res pixel 2 x vpos, the same
	// reconstruction this pass used to do itself). The position stays from the full-precision z/w
	// target: the prep's half-float z is off by up to 1/2048 of the distance, which next to the
	// full-precision depths of the samples would tilt the horizons on distant surfaces.
	float3 N = tex2Dlod(s_Geo, float4((vpos + 0.5) * c_Half.zw, 0, 0)).xyz;

	// Per-pixel rotation and step offset (interleaved gradient noise; the blur removes the pattern).
	float noiseA = frac(52.9829189 * frac(dot(vpos, float2(0.06711056, 0.00583715))));
	float noiseB = frac(52.9829189 * frac(dot(vpos + float2(17.0, 31.0), float2(0.06711056, 0.00583715))));

	float contact = ContactShadow(P, N, z, noiseB);

	float radiusPx = min(c_Params.x * c_Params.y / z, c_Params.z);
	if (c_Params2.x <= 0 || radiusPx < 2.0)
		return float4(1, z, contact, 0); // AO off, or too far away for the radius to cover a pixel

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
	return float4(max(visibility / SLICES, 0), z, contact, 0);
}

// ---- Depth-aware blur (half res, one direction per pass) ---------------------------------------

float4 PS_Blur(VPOS_T vposIn : VPOS) : COLOR
{
	float2 vpos = PIXEL(vposIn);
	float2 uv = (vpos + 0.5) * c_Half.zw;
	float3 centre = tex2Dlod(s_AO, float4(uv, 0, 0)).xyz;
	float2 stepUv = c_Params2.zw * c_Half.zw;
	float tolerance = 1.0 / (0.08 * centre.y);
	float sum = centre.x, wsum = 1.0;
	float csSum = centre.z;
	const float gauss[5] = { 1.0, 0.8825, 0.6065, 0.3247, 0.1353 }; // sigma 2
	[unroll] for (int i = 1; i <= 4; i++)
	{
		float3 a = tex2Dlod(s_AO, float4(uv + stepUv * i, 0, 0)).xyz;
		float3 b = tex2Dlod(s_AO, float4(uv - stepUv * i, 0, 0)).xyz;
		float wa = gauss[i] * saturate(1.0 - abs(a.y - centre.y) * tolerance);
		float wb = gauss[i] * saturate(1.0 - abs(b.y - centre.y) * tolerance);
		sum += a.x * wa + b.x * wb;
		csSum += a.z * wa + b.z * wb;
		wsum += wa + wb;
	}
	return float4(sum / wsum, centre.y, csSum / wsum, 0);
}

// ---- Composite (full res) ----------------------------------------------------------------------
// Blended onto the scene with "destination - source": returns the light to remove.
// View modes (no blending): 1 = AO, 2 = the indirect light buffer, 3 = contact shadows, 4 = the sun
// light buffer.

float4 PS_Composite(VPOS_T vposIn : VPOS) : COLOR
{
	float2 vpos = PIXEL(vposIn);
	float2 uv = (vpos + 0.5) * c_Full.zw;
	float d = tex2Dlod(s_Depth, float4(uv, 0, 0)).x;
	float3 indirect = tex2Dlod(s_Indirect, float4(uv, 0, 0)).rgb;
	float3 sun = tex2Dlod(s_Sun, float4(uv, 0, 0)).rgb;
	float view = c_Params2.y;
	if (view > 3.5)
		return float4(sun, 1);
	if (view > 1.5 && view < 2.5)
		return float4(indirect, 1);
	if (d >= 1.0)
		return view > 0.5 ? float4(1, 1, 1, 1) : float4(0, 0, 0, 0);
	float z = LinearZ(d);

	// AO texel i was computed for full-res pixel 2i: blend the four around this pixel, weighted
	// by distance and by how close their depth is to this pixel's.
	float2 h = vpos * 0.5;
	float2 i0 = floor(h);
	float2 f = h - i0;
	float2 uv0 = (i0 + 0.5) * c_Half.zw;
	float3 a00 = tex2Dlod(s_AO, float4(uv0, 0, 0)).xyz;
	float3 a10 = tex2Dlod(s_AO, float4(uv0 + float2(c_Half.z, 0), 0, 0)).xyz;
	float3 a01 = tex2Dlod(s_AO, float4(uv0 + float2(0, c_Half.w), 0, 0)).xyz;
	float3 a11 = tex2Dlod(s_AO, float4(uv0 + c_Half.zw, 0, 0)).xyz;
	float4 w = float4((1 - f.x) * (1 - f.y), f.x * (1 - f.y), (1 - f.x) * f.y, f.x * f.y) + 0.01;
	float4 dz = abs(float4(a00.y, a10.y, a01.y, a11.y) - z) / (0.05 * z);
	w *= saturate(1.0 - dz) + 0.001;
	float ao = dot(w, float4(a00.x, a10.x, a01.x, a11.x)) / dot(w, 1);
	float contact = saturate(dot(w, float4(a00.z, a10.z, a01.z, a11.z)) / dot(w, 1));
	ao = pow(saturate(ao), c_Params2.x);
	if (view > 2.5)
		return float4(contact, contact, contact, 1);
	if (view > 0.5)
		return float4(ao, ao, ao, 1);
	return float4(indirect * (1.0 - ao) + sun * (1.0 - contact), 0);
}

// ---- Frame dump: copy a depth texture (INTZ) into a colour target so it can be read back -------

sampler2D s_Copy : register(s0);

float4 PS_DepthCopy(float2 uv : TEXCOORD0) : COLOR
{
	return tex2D(s_Copy, uv).x;
}
