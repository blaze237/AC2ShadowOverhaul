// AC2SSGI passes: screen-space global illumination from the game's lit colour and z/w target.
//
// Compiled by tools/gen_shaders.c into src/shaders_gen.h - run it after editing this file.
//
//  (geometry)  view-space normal + view z at half res: the prep shared with ambient occlusion
//              (src/core/shaders/geoprep.hlsl, core/geoprep.h), computed once per frame
//  PS_Radiance (half res)  lit colour (2x2 average, clamped), read after the ambient occlusion and
//                          contact shadows are composited, so the bounce comes from occluded light;
//                          above intensity 1, part of last frame's bounce is taken back out of it
//                          (see the comment there)
//  PS_Trace    (half res)  gathers light from the screen around each pixel: per slice through the
//                          view direction, march out both ways; each sample covers the angles between
//                          its front face and a point `thickness` behind it. A 16-sector mask over the
//                          slice's hemisphere (sectors of equal cosine-weighted measure, GTAO's slice
//                          integral) records what's already covered, so only the nearest surface in a
//                          direction contributes its light. ("Screen Space Indirect Lighting with Visibility
//                          Bitmask", Therrien et al. 2023, with float sectors since ps_3_0 has no
//                          integer bit operations; slice integration as in GTAO.)
//  PS_Temporal (half res)  blends with last frame's result at the reprojected position, where the
//                          depth there matches, clipped to the range of this frame's trace around
//                          the pixel
//  PS_Blur     (half res)  depth- and normal-aware blur, one direction per pass, wider where fewer
//                          frames are accumulated; the second pass applies the intensity and writes
//                          the texture the game's shaders read
//  PS_Debug    (full res)  debug views drawn over the scene colour
//
// Result: irradiance-like light colour E such that a surface of albedo a gets a * E added. A surface
// fully surrounded by surfaces of radiance L gets E = L.
//
// Coordinates: full-res pixel p (integer, D3D9 pixel centres) has NDC (2p / size - 1, 1 - 2p / size).
// Half-res texel j holds full-res pixel 2j. View space: x right, y up, z forward (metres).

float4 c_Full     : register(c0);  // full-res width, height, 1/width, 1/height
float4 c_Half     : register(c1);  // half-res width, height, 1/width, 1/height
float4 c_Proj     : register(c2);  // A, B (z/w = A + B / z), P00, P11
float4 c_Proj2    : register(c3);  // P02, P12, frame index (0..63), radiance clamp
float4 c_Trace    : register(c4);  // radius (m), full-res pixels per (metre / z) [0.5 * P00 * width], max radius (full-res px), thickness (m)
float4 c_Trace2   : register(c5);  // slices, steps per side, intensity, debug view
float4 c_Reproj0  : register(c6);  // (z/w, x, y, 1) of a full-res pixel -> u * w in last frame's half-res textures
float4 c_Reproj1  : register(c7);  //                                     -> v * w
float4 c_Reproj2  : register(c8);  //                                     -> w (view z then / view z now)
float4 c_Temporal : register(c9);  // minimum blend, depth tolerance (relative), history valid, maximum frame count
float4 c_Blur     : register(c10); // direction (half-res texels per tap), 1 = last pass (apply intensity), 0
float4 c_GiRow0   : register(c11); // (z/w, x, y, 1) of a full-res pixel -> u * w in the GI texture the game's shaders read this frame
float4 c_GiRow1   : register(c12); //                                     -> v * w
float4 c_GiRow2   : register(c13); //                                     -> w
float4 c_Feedback : register(c14); // share of that bounce taken out of the radiance, history clip (sigmas, 0 = off), 0, 0

sampler2D s_Colour  : register(s1); // Radiance: game scene colour (linear)
sampler2D s_Geo     : register(s0); // Trace/Temporal/Blur: the shared geometry prep (normal, z; z = 0: sky) (point)
sampler2D s_Rad     : register(s1); // Trace: radiance (point)
sampler2D s_Raw     : register(s1); // Temporal: trace output (point)
sampler2D s_Hist    : register(s2); // Temporal: last frame's history (point)
sampler2D s_Gi      : register(s2); // Radiance: the GI texture the game's shaders read this frame (linear)
sampler2D s_PrevGeo : register(s3); // Temporal: last frame's geometry prep (point)
sampler2D s_Src     : register(s1); // Blur: input (point)
sampler2D s_Out     : register(s1); // Debug: a half-res result (linear)
sampler2D s_Normals : register(s2); // Debug: the geometry prep (point)

// tests/ssgi_warp_test.cpp runs these on D3D11 (WARP), where the pixel position is the pixel centre.
#ifdef SSGI_D3D11
#define VPOS_T float4
#define PIXEL(v) ((v).xy - 0.5)
#else
#define VPOS_T float2
#define PIXEL(v) (v)
#endif

static const float PI = 3.14159265;
static const float HALF_PI = 1.57079633;

float3 ViewPos(float2 px, float z) // full-res pixel, view z
{
	float2 ndc = float2(px.x * 2.0 * c_Full.z - 1.0, 1.0 - px.y * 2.0 * c_Full.w);
	return float3((ndc - c_Proj2.xy) / c_Proj.zw * z, z);
}

float2 HalfUv(float2 j) { return (j + 0.5) * c_Half.zw; }

float Ign(float2 p) // interleaved gradient noise
{
	return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// ---- Fullscreen triangle -----------------------------------------------------------------------

float4 VS_Fullscreen(float4 p : POSITION) : POSITION
{
	return p;
}

// ---- Radiance ----------------------------------------------------------------------------------

// The game's shaders added albedo x last frame's GI (G) to this colour, so the bounce feeds back into
// itself: light keeps bouncing over the frames, each round scaled by intensity x albedo x the share
// of the surroundings that is lit surface (f, at most 1). Up to intensity 1 that is the physical
// many-bounce sum, and the loop gain (albedo x f) stays below 1. Above 1 it could pass 1 in bright
// enclosed spaces and the light would climb every frame until the radiance clamp. So for intensity
// I > 1 a share k = 1 - 1/I of min(colour, G) is taken back out (the albedo isn't known here, but the
// bounce part of the colour is at most G, and at most the colour itself). Per channel, with D the rest
// of the colour and a the albedo:
//   - where the bounce outweighs the rest (min = colour): the trace sees (1 - k) (D + a G), gain
//     I (1 - k) a f = a f, as at intensity 1;
//   - where the rest outweighs it (min = G): the trace sees D + (a - k) G, gain I (a - k) f, between
//     -(I - 1) f and f (a negative gain only damps; its size stays below 1 up to I = 2, the most the
//     ini allows).
// So the loop never gains more than it does at intensity 1, the extra intensity brightens the first
// bounce, and the colour never drops below (1 - k) of itself. k = 0 up to intensity 1: unchanged.
// G is read where the game's shaders read it for this block's centre (c11-c13, their c216-c218).
float4 PS_Radiance(VPOS_T vposIn : VPOS) : COLOR
{
	float2 j = PIXEL(vposIn);
	float2 px = j * 2.0;
	// Centre of the 2x2 block with bilinear filtering = its average. NaN / negative values from the
	// game's HDR targets are dropped, very bright ones clamped (they'd make the noise flicker).
	float3 rad = max(tex2Dlod(s_Colour, float4((px + 1.0) * c_Full.zw, 0, 0)).rgb, 0);
	float z = tex2Dlod(s_Geo, float4(HalfUv(j), 0, 0)).w;
	if (c_Feedback.x > 0 && z > 0)
	{
		float4 q = float4(c_Proj.x + c_Proj.y / z, px + 0.5, 1.0);
		float3 h = float3(dot(q, c_GiRow0), dot(q, c_GiRow1), dot(q, c_GiRow2));
		float3 g = h.z > 0 ? max(tex2Dlod(s_Gi, float4(h.xy / h.z, 0, 0)).rgb, 0) : 0;
		rad -= c_Feedback.x * min(rad, g);
	}
	return float4(min(rad, c_Proj2.w), 1);
}

// ---- Trace -------------------------------------------------------------------------------------

static const float4 K0 = float4(0, 1, 2, 3) / 16.0;
static const float4 K1 = float4(4, 5, 6, 7) / 16.0;
static const float4 K2 = float4(8, 9, 10, 11) / 16.0;
static const float4 K3 = float4(12, 13, 14, 15) / 16.0;

float FastAcos(float x)
{
	float a = abs(x);
	float r = (-0.156583 * a + HALF_PI) * sqrt(1.0 - a);
	return x >= 0 ? r : PI - r;
}

// Cumulative cosine-weighted measure of a slice up to angle th (from the view vector, positive
// towards +T), as in GTAO: the integral of cos(t - n) |sin t| dt from 0, signed so it increases with
// th. n = angle of the projected normal. The sector mask is uniform in this measure, so every sector
// carries the same share of the irradiance.
float SliceMeasure(float th, float n, float cosN, float sinN)
{
	return sign(th) * (cosN + 2.0 * th * sinN - cos(2.0 * th - n)) * 0.25;
}

// Position (0..1) of a direction in the slice's sector mask. side = which way it was marched; the
// angle is clamped to the hemisphere around the normal (directions below the surface go to its end).
float SectorPos(float3 dir, float3 V, float side, float n, float cosN, float sinN, float lo, float invRange)
{
	float th = side * FastAcos(clamp(dot(dir, V), -1.0, 1.0));
	th = clamp(th, n - HALF_PI, n + HALF_PI);
	return (SliceMeasure(th, n, cosN, sinN) - lo) * invRange;
}

// Marks the sectors between positions lo and hi (0..1) as covered; returns the share of the slice
// newly covered (sectors already covered by nearer samples don't count again).
float Cover(float lo, float hi, inout float4 occ0, inout float4 occ1, inout float4 occ2, inout float4 occ3)
{
	float4 n0 = saturate((min(hi, K0 + 0.0625) - max(lo, K0)) * 16.0) * (1 - occ0);
	float4 n1 = saturate((min(hi, K1 + 0.0625) - max(lo, K1)) * 16.0) * (1 - occ1);
	float4 n2 = saturate((min(hi, K2 + 0.0625) - max(lo, K2)) * 16.0) * (1 - occ2);
	float4 n3 = saturate((min(hi, K3 + 0.0625) - max(lo, K3)) * 16.0) * (1 - occ3);
	occ0 += n0;
	occ1 += n1;
	occ2 += n2;
	occ3 += n3;
	return dot(n0 + n1 + n2 + n3, 0.0625);
}

// (The ambient occlusion keeps its own GTAO trace rather than reading one from this: see the
// comment above PS_GTAO in src/shaders/ao.hlsl.)
float4 PS_Trace(VPOS_T vposIn : VPOS) : COLOR
{
	float2 j = PIXEL(vposIn);
	float4 geo = tex2Dlod(s_Geo, float4(HalfUv(j), 0, 0));
	float z = geo.w;
	if (z <= 0)
		return float4(0, 0, 0, 1);
	float3 N = geo.xyz;
	float3 P = ViewPos(j * 2.0, z);
	float3 V = normalize(-P);
	P += N * (2.0 * z * c_Full.w / c_Proj.w); // lift by a pixel's size against depth noise

	float radius = c_Trace.x;
	float radiusH = 0.5 * min(radius * c_Trace.y / z, c_Trace.z); // half-res texels
	if (radiusH < 1.0)
		return float4(0, 0, 0, 1);
	float2 seed = j + 5.588238 * c_Proj2.z;
	float noiseA = Ign(seed);
	float noiseB = Ign(seed + float2(47.0, 17.0));
	float slices = c_Trace2.x, steps = c_Trace2.y;
	float thickness = c_Trace.w;

	float3 gi = 0;
	float occlusion = 0;
	[loop] for (float s = 0; s < slices; s++)
	{
		float phi = (s + noiseA) * (PI / slices);
		float2 omega = float2(cos(phi), -sin(phi)); // screen, y down
		float3 dirV = float3(omega.x, -omega.y, 0);  // view, y up
		float3 T = normalize(dirV - dot(dirV, V) * V);
		float3 axis = cross(T, V);
		float3 projN = N - axis * dot(N, axis);
		float projLen = length(projN);
		float3 Np = projN / max(projLen, 1e-4);
		float n = (dot(Np, T) >= 0 ? 1.0 : -1.0) * FastAcos(clamp(dot(Np, V), -1.0, 1.0));
		float cosN = cos(n), sinN = sin(n);
		float lo = SliceMeasure(n - HALF_PI, n, cosN, sinN), hi = SliceMeasure(n + HALF_PI, n, cosN, sinN);
		float range = hi - lo, invRange = 1.0 / max(range, 1e-4);

		float4 occ0 = 0, occ1 = 0, occ2 = 0, occ3 = 0;
		float3 sliceGi = 0;
		// Both directions in one loop: steps toward -omega, then toward +omega. (A nested loop per
		// direction makes the compiler copy the mask each time round; the shader is at the ps_3_0
		// limit of 32 temporary registers.)
		// The previous sample on this side, to bridge the gap to this one when both lie on one surface.
		float4 prevPlane = 0; // its surface: normal, -dot(normal, position)
		float4 prevLA = 0;    // its light x face, its sector position
		float prevFace = -1;  // its face; < 0: none
		[loop] for (float k = 0; k < 2 * steps; k++)
		{
			float side = k < steps ? -1.0 : 1.0;
			float ks = k < steps ? k : k - steps;
			if (ks == 0)
				prevFace = -1;
			float t = (ks + noiseB) / steps;
			float2 tj = j + round(omega * side * (t * t * (radiusH - 1.0) + 1.0));
			float2 uv = HalfUv(tj);
			float4 sg = tex2Dlod(s_Geo, float4(uv, 0, 0));
			float3 S = ViewPos(tj * 2.0, sg.w);
			float3 d = S - P;
			float dist = length(d);
			float fall = saturate((radius - dist) / (0.25 * radius));
			// Off screen, sky (no light, no cover), or out of range: nothing, and no bridge across it.
			// (No early continue / break: each exit makes the compiler copy the loop's state.)
			bool onScreen = uv.x >= 0 && uv.y >= 0 && uv.x <= 1 && uv.y <= 1;
			if (!(onScreen && sg.w > 0 && fall > 0 && dist > 1e-4))
			{
				prevFace = -1;
			}
			else
			{
				float3 dn = d / dist;
				// Angles within the slice: samples rounded to texel centres sit a little off its plane.
				float3 dp = normalize(d - axis * dot(d, axis));
				float3 bp = S + normalize(S) * thickness - P;
				bp = normalize(bp - axis * dot(bp, axis));
				float a = SectorPos(dp, V, side, n, cosN, sinN, lo, invRange);
				float b = SectorPos(bp, V, side, n, cosN, sinN, lo, invRange);
				// Light only leaves the side of the sample's surface that faces us. A matt surface looks
				// equally bright from every angle in front of it (the mask already accounts for how big it
				// looks), so this is an in-front test, softened against normal noise.
				float facing = saturate(dot(sg.xyz, -dn) * 10.0 + 0.5);
				float3 L = tex2Dlod(s_Rad, float4(uv, 0, 0)).rgb * fall;
				// How much this sample's surface faces us, for the light between samples (0 edge-on: a
				// surface seen edge-on from here, like the wall we stand on, isn't what lies between).
				float face = saturate(dot(sg.xyz, -dn) * 4.0);
				float cLo = min(a, b), cHi = max(a, b);
				float3 Lc = L * facing;
				// A floor or wall is continuous between samples, but each sample only covers `thickness`
				// behind it, which leaves gaps between sparse far samples: open directions that should be
				// covered (and lit) by the surfaces between them. Where this sample doesn't lie behind the
				// previous one's surface (one plane, or a concave corner like the foot of a wall), the
				// surface is taken to continue between them, lit like the one that faces us more. A
				// background seen past an object's silhouette lies behind it, so it isn't bridged.
				bool bridge = prevFace >= 0 && dot(prevPlane, float4(S, 1)) > -0.25 * dist;
				cLo = bridge ? min(cLo, prevLA.w) : cLo;
				cHi = bridge ? max(cHi, prevLA.w) : cHi;
				Lc = bridge ? (L * face + prevLA.xyz) / max(face + prevFace, 1e-3) : Lc;
				sliceGi += Lc * Cover(cLo, cHi, occ0, occ1, occ2, occ3);
				prevPlane = float4(sg.xyz, -dot(sg.xyz, S));
				prevLA = float4(L * face, a);
				prevFace = face;
			}
		}
		// Each sector holds range / 16 of the slice's cosine-weighted measure (GTAO's slice weight).
		gi += sliceGi * (range * projLen);
		occlusion += dot(occ0 + occ1 + occ2 + occ3, 0.0625) * (range * projLen);
	}
	return float4(gi / slices, saturate(1.0 - occlusion / slices));
}

// ---- Temporal accumulation (half res) ----------------------------------------------------------
// Output: rgb = accumulated light, a = number of frames accumulated (0 = sky).

float4 PS_Temporal(VPOS_T vposIn : VPOS) : COLOR
{
	float2 j = PIXEL(vposIn);
	float2 uv = HalfUv(j);
	float3 cur = tex2Dlod(s_Raw, float4(uv, 0, 0)).rgb;
	float z = tex2Dlod(s_Geo, float4(uv, 0, 0)).w;
	if (z <= 0)
		return 0;
	if (c_Temporal.z < 0.5)
		return float4(cur, 1);
	float4 q = float4(c_Proj.x + c_Proj.y / z, j * 2.0, 1.0);
	float3 h = float3(dot(q, c_Reproj0), dot(q, c_Reproj1), dot(q, c_Reproj2));
	if (h.z <= 0)
		return float4(cur, 1);
	float2 puv = h.xy / h.z;
	float pz = h.z * z; // the rows give (last frame's view z) / (this frame's)
	// Four texels around that point, each used only if the last frame saw the same surface there.
	float2 tp = puv * c_Half.xy - 0.5;
	float2 i0 = floor(tp);
	float2 f = tp - i0;
	float4 sum = 0;
	float wsum = 0;
	const float2 offs[4] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 1) };
	[unroll] for (int k = 0; k < 4; k++)
	{
		float2 o = offs[k];
		float2 tuv = (i0 + o + 0.5) * c_Half.zw;
		float w = (o.x ? f.x : 1 - f.x) * (o.y ? f.y : 1 - f.y);
		float hz = tex2Dlod(s_PrevGeo, float4(tuv, 0, 0)).w;
		bool inside = tuv.x > 0 && tuv.y > 0 && tuv.x < 1 && tuv.y < 1;
		w *= (inside && hz > 0 && abs(hz - pz) < c_Temporal.y * pz) ? 1.0 : 0.0;
		sum += tex2Dlod(s_Hist, float4(tuv, 0, 0)) * w;
		wsum += w;
	}
	if (wsum < 0.05)
		return float4(cur, 1);
	float4 hist = sum / wsum;
	if (c_Feedback.y > 0)
	{
		// Clip the history to the range of this frame's trace around the pixel (mean +- c_Feedback.y
		// standard deviations over 3x3 texels): where the light changed (a sunlit wall's bounce on the
		// floor as someone walks past) the old light goes at once instead of fading over ~10 frames.
		// The trace is noisy, but its noise pattern is spread over each 3x3 block (interleaved gradient
		// noise), so the block's spread covers the noise and settled light stays inside the box.
		float3 m1 = 0, m2 = 0;
		[unroll] for (int y = -1; y <= 1; y++)
		{
			[unroll] for (int x = -1; x <= 1; x++)
			{
				float3 r = tex2Dlod(s_Raw, float4(uv + float2(x, y) * c_Half.zw, 0, 0)).rgb;
				m1 += r;
				m2 += r * r;
			}
		}
		m1 /= 9.0;
		float3 box = c_Feedback.y * sqrt(max(m2 / 9.0 - m1 * m1, 0));
		hist.rgb = clamp(hist.rgb, m1 - box, m1 + box);
	}
	float count = min(hist.a + 1.0, c_Temporal.w);
	float blend = max(1.0 / count, c_Temporal.x);
	return float4(lerp(hist.rgb, cur, blend), count);
}

// ---- Depth- and normal-aware blur (half res, one direction per pass) ---------------------------

float4 PS_Blur(VPOS_T vposIn : VPOS) : COLOR
{
	float2 j = PIXEL(vposIn);
	float2 uv = HalfUv(j);
	float4 centre = tex2Dlod(s_Src, float4(uv, 0, 0));
	float4 geo = tex2Dlod(s_Geo, float4(uv, 0, 0));
	if (geo.w <= 0)
		return 0;
	// Wider where few frames are accumulated (just uncovered, or after a reset), narrower where many
	// are: the noise left after n frames falls as 1 / sqrt(n), and so does the width that evens it out.
	// x 2 for a fresh pixel, x 0.75 at 10 frames (the most a TemporalBlend of 0.1 keeps count of). It
	// widens by taking more taps, not by spacing them further apart: the trace's noise pattern is spread
	// over neighbouring texels, which spaced-out taps would skip. Taps with fewer frames than this pixel
	// count for less, so fresh noise doesn't spread into settled light. (The first pass keeps the count
	// in alpha for the second.)
	float count = max(centre.a, 1.0);
	float width = clamp(2.4 * rsqrt(count), 0.75, 2.0);
	float taps = ceil(4.0 * width); // the Gaussian (sigma 2 x width taps) down to e^-2 of its peak
	float falloff = -0.125 / (width * width);
	float2 stepUv = c_Blur.xy * c_Half.zw;
	float tolerance = 1.0 / (0.04 * geo.w);
	float3 sum = centre.rgb;
	float wsum = 1.0;
	[loop] for (float i = 1; i <= taps; i++)
	{
		float gauss = exp(falloff * i * i);
		[unroll] for (int sgn = -1; sgn <= 1; sgn += 2)
		{
			float2 tuv = uv + stepUv * (i * sgn);
			float4 g = tex2Dlod(s_Geo, float4(tuv, 0, 0));
			float4 c = tex2Dlod(s_Src, float4(tuv, 0, 0));
			float n = saturate(dot(g.xyz, geo.xyz));
			n *= n;
			n *= n;
			float w = gauss * saturate(1.0 - abs(g.w - geo.w) * tolerance) * n * n * saturate(c.a / count);
			sum += c.rgb * w;
			wsum += w;
		}
	}
	float3 r = sum / wsum;
	if (c_Blur.z > 0.5)
		return float4(r * c_Trace2.z, 1);
	return float4(r, centre.a);
}

// ---- Debug views (full res, over the scene colour) ---------------------------------------------
// 1 = the GI light the game's shaders get (x intensity), 2 = the raw trace, 3 = normals.

float4 PS_Debug(VPOS_T vposIn : VPOS) : COLOR
{
	float2 p = PIXEL(vposIn);
	float2 uv = (p * 0.5 + 0.5) * c_Half.zw;
	float view = c_Trace2.w;
	if (view > 2.5)
	{
		float4 g = tex2Dlod(s_Normals, float4(uv, 0, 0));
		return float4(g.w > 0 ? g.xyz * 0.5 + 0.5 : 0, 1);
	}
	return float4(tex2Dlod(s_Out, float4(uv, 0, 0)).rgb, 1);
}
