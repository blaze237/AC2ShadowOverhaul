// AC2TAA temporal anti-aliasing: resolve and output passes.
//
// The scene is rendered with a different sub-pixel offset (jitter) every frame. The resolve pass
// blends this frame's colour into the accumulated history, fetched from where each surface was in
// the previous frame (camera reprojection from the game's z/w target), after clipping the history
// to this frame's local colour range so stale colours can't smear. The output pass writes the
// result (optionally sharpened) back into the game's scene colour target, before its bloom and
// tone mapping read it.
//
// Compiled by tools/gen_shaders.c into src/shaders_gen.h - run it after editing this file.
//
// Coordinates: pixel p (integer in D3D9, the pixel centre) samples at uv = (p + 0.5) / size and
// corresponds to NDC (2p / size - 1, 1 - 2p / size). "Jitter o" means the rendered image is the
// unjittered image moved by o pixels (x right, y down): pixel q shows the scene at q - o.
//
// History texture: rgb = colour, compressed (c / (1 + k luma)) so very bright pixels can't dominate
// the blend or ring in the bicubic fetch; a = the pixel's linear depth (metres), to detect surfaces
// that were hidden in the previous frame.

float4 c_Size    : register(c0); // width, height, 1/width, 1/height
float4 c_Jitter  : register(c1); // jitter of this frame (pixels), history valid (0/1), debug view
float4 c_Reproj0 : register(c2); // rows of the matrix: this frame's view space -> previous frame's clip space
float4 c_Reproj1 : register(c3);
float4 c_Reproj2 : register(c4);
float4 c_Reproj3 : register(c5);
float4 c_Proj    : register(c6); // A, B (z/w = A + B / z), P00, P11
float4 c_Proj2   : register(c7); // P02, P12 (off-centre projection), 0, 0
float4 c_Params  : register(c8); // current weight when still, current weight in fast motion, motion (px) for that, depth tolerance
float4 c_Params2 : register(c9); // sharpness, clip box size (standard deviations), HDR compression k, distrust threshold

sampler2D s_Scene     : register(s0); // the game's scene colour (point)
sampler2D s_History   : register(s1); // previous history (bilinear) / the new history in the output pass (point)
sampler2D s_Depth     : register(s2); // the game's z/w (point)
sampler2D s_HistPoint : register(s3); // previous history (point): depth in alpha

// tests/taa_warp_test.cpp runs these shaders on D3D11 (WARP), where the pixel position is the
// pixel centre (x + 0.5) rather than D3D9's integer VPOS.
#ifdef TAA_D3D11
#define VPOS_T float4
#define PIXEL(v) ((v).xy - 0.5)
#else
#define VPOS_T float2
#define PIXEL(v) (v)
#endif

float4 VS_Fullscreen(float4 p : POSITION) : POSITION
{
	return p;
}

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
// Reversible: c / (1 + k luma). Damps very bright pixels (fireflies) in the blend; k small keeps the
// usual 0..1 range nearly linear so edges average to the right brightness.
float3 Compress(float3 c) { return c / (1.0 + c_Params2.z * max(Luma(c), 0.0)); }
float3 Expand(float3 c) { return c / max(1.0 - c_Params2.z * max(Luma(c), 0.0), 1e-4); }
float3 ToYCoCg(float3 c) { return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25))); }
float3 FromYCoCg(float3 c) { return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }
// Keeps a NaN or infinity in the game's colour from poisoning the history for good.
float3 Sane(float3 c) { return min(max(c, -1e4), 1e4); }

float2 PixUV(float2 px) { return (px + 0.5) * c_Size.zw; }
float LinearZ(float d) { return c_Proj.y / (d - c_Proj.x); }

// Pixel position of the unjittered point at pixel `px` with linear depth `z`, in the previous frame,
// and that point's depth in the previous frame (w).
float3 Reproject(float2 px, float z)
{
	float2 ndc = float2(px.x * 2.0 * c_Size.z - 1.0, 1.0 - px.y * 2.0 * c_Size.w);
	float4 v = float4((ndc - c_Proj2.xy) / c_Proj.zw * z, z, 1.0);
	float4 clip = float4(dot(c_Reproj0, v), dot(c_Reproj1, v), dot(c_Reproj2, v), dot(c_Reproj3, v));
	float2 prevNdc = clip.xy / clip.w;
	return float3((prevNdc.x + 1.0) * 0.5 * c_Size.x, (1.0 - prevNdc.y) * 0.5 * c_Size.y, clip.w);
}

// Could the history at position `px` hold the surface seen around p? Yes if the depths around p
// (expLo..expHi, as they were in the previous frame) overlap those of the 4 history pixels around
// px (with tolerance); no means something else was there (it was hidden, or it moved). Ranges, so
// the jitter moving samples along a steep slope (ground at a glancing angle) doesn't count.
bool HistoryDepthMatches(float2 px, float expLo, float expHi)
{
	float2 base = floor(px);
	float d0 = tex2Dlod(s_HistPoint, float4(PixUV(base), 0, 0)).a;
	float d1 = tex2Dlod(s_HistPoint, float4(PixUV(base + float2(1, 0)), 0, 0)).a;
	float d2 = tex2Dlod(s_HistPoint, float4(PixUV(base + float2(0, 1)), 0, 0)).a;
	float d3 = tex2Dlod(s_HistPoint, float4(PixUV(base + float2(1, 1)), 0, 0)).a;
	float lo = min(min(d0, d1), min(d2, d3)), hi = max(max(d0, d1), max(d2, d3));
	float tol = c_Params.w;
	return expHi >= lo * (1.0 - tol) && expLo <= hi * (1.0 + tol);
}

// Catmull-Rom history fetch from 5 bilinear taps (the 4 corner taps are dropped), keeps the
// history sharp when it is resampled at fractional positions.
float3 SampleHistory(float2 px)
{
	float2 pos = px + 0.5; // texel units, texel centres at +0.5
	float2 t1 = floor(pos - 0.5) + 0.5;
	float2 f = pos - t1;
	float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	float2 w3 = f * f * (-0.5 + 0.5 * f);
	float2 w12 = w1 + w2;
	float2 t0 = (t1 - 1.0) * c_Size.zw;
	float2 t3 = (t1 + 2.0) * c_Size.zw;
	float2 t12 = (t1 + w2 / w12) * c_Size.zw;
	float3 c = tex2Dlod(s_History, float4(t12.x, t0.y, 0, 0)).rgb * (w12.x * w0.y);
	c += tex2Dlod(s_History, float4(t0.x, t12.y, 0, 0)).rgb * (w0.x * w12.y);
	c += tex2Dlod(s_History, float4(t12.x, t12.y, 0, 0)).rgb * (w12.x * w12.y);
	c += tex2Dlod(s_History, float4(t3.x, t12.y, 0, 0)).rgb * (w3.x * w12.y);
	c += tex2Dlod(s_History, float4(t12.x, t3.y, 0, 0)).rgb * (w12.x * w3.y);
	float w = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
	return c / w;
}

// Moves `c` towards the centre of the box until it is inside (keeps its hue better than a clamp).
// The box is padded by a little more than float16 rounding: where the neighbourhood has no spread
// in one channel (grey or single-hue surfaces have none in chroma), the history's rounding noise in
// that channel would otherwise count as far outside and drag all channels towards the centre.
float3 BoxExtent(float3 lo, float3 hi) { return 0.5 * (hi - lo) + 0.002 * (1.0 + abs(0.5 * (hi.x + lo.x))); }

// How far outside the box `c` is, in box half-sizes (<= 1: inside).
float BoxDistance(float3 c, float3 lo, float3 hi)
{
	float3 a = abs((c - 0.5 * (hi + lo)) / BoxExtent(lo, hi));
	return max(a.x, max(a.y, a.z));
}

float3 ClipToBox(float3 c, float3 lo, float3 hi)
{
	float3 centre = 0.5 * (hi + lo);
	float m = BoxDistance(c, lo, hi);
	return m > 1.0 ? centre + (c - centre) / m : c;
}

float4 PS_Resolve(VPOS_T vpos : VPOS) : COLOR0
{
	float2 p = PIXEL(vpos);
	float2 o = c_Jitter.xy;

	// This frame around p: 9 colours (YCoCg) and depths.
	float3 col[9];
	float zw[9];
	[unroll] for (int k = 0; k < 9; k++)
	{
		float2 uv = PixUV(p + float2(k % 3 - 1, k / 3 - 1));
		col[k] = ToYCoCg(Compress(Sane(tex2Dlod(s_Scene, float4(uv, 0, 0)).rgb)));
		zw[k] = tex2Dlod(s_Depth, float4(uv, 0, 0)).x;
	}
	float3 centre = col[4];
	float zHere = LinearZ(zw[4]);

	// The colour range (mean / deviation, cut to min / max) that the history is clipped to; the
	// jitter-free estimate at p (a Gaussian through the 9 samples, each at its true position q - o);
	// the nearest surface (its motion is used, so edges move with what is in front) and the depth
	// range.
	float3 m1 = 0, m2 = 0, lo = 1e5, hi = -1e5, filtered = 0;
	float wsum = 0, nearZ = 2.0, farZ = -2.0;
	float2 nearOff = 0;
	[unroll] for (int q = 0; q < 9; q++)
	{
		float2 kk = float2(q % 3 - 1, q / 3 - 1);
		float3 c = col[q];
		float2 d = kk - o;
		float w = exp(-2.29 * dot(d, d));
		filtered += c * w;
		wsum += w;
		m1 += c;
		m2 += c * c;
		lo = min(lo, c);
		hi = max(hi, c);
		if (zw[q] < nearZ)
		{
			nearZ = zw[q];
			nearOff = kk;
		}
		farZ = max(farZ, zw[q]);
	}
	filtered /= wsum;
	float3 mean = m1 / 9.0, sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
	float gamma = c_Params2.y;
	float3 boxLo = max(lo, mean - gamma * sigma), boxHi = min(hi, mean + gamma * sigma);

	// Where p's surface was in the previous frame. Candidates:
	//   B: the camera moved and the world stood still; the nearest surface's motion (the default:
	//      keeps the edges of things in front steady);
	//   A: the same with p's own surface's motion, where the nearest surface is a neighbour's
	//      (next to the edge of something nearer);
	//   C: no motion: the surface moved with the camera (the player character it follows).
	// Each counts only if the history there holds a surface at the right depth. Which fits best is
	// judged by colour, against this frame's jitter-free estimate at p: A over B where it is closer
	// (B carries the player's wrong, world-fixed motion onto the ground next to him); C only where
	// it is clearly closer (the player's own edges). Depth alone can't decide: flat ground looks the
	// same at a fixed screen position when the camera moves sideways, and the ground at the
	// player's feet is as far away as the player.
	float zLo = LinearZ(nearZ), zHi = LinearZ(farZ);
	float3 nearPrev = Reproject(p + nearOff - o, zLo);
	float3 herePrev = Reproject(p - o, zHere);
	float2 motionB = nearPrev.xy - (p + nearOff - o); // pixels
	float2 motionA = herePrev.xy - (p - o);
	float2 pxB = p + motionB, pxA = p + motionA;
	float toPrev = herePrev.z / zHere; // depth change from the camera's motion
	bool valid = c_Jitter.z > 0;
	bool okB = valid && all(pxB > -0.5) && all(pxB < c_Size.xy - 0.5) && HistoryDepthMatches(pxB, zLo * toPrev, zHi * toPrev);
	bool okA = valid && any(nearOff != 0) && all(pxA > -0.5) && all(pxA < c_Size.xy - 0.5) &&
		HistoryDepthMatches(pxA, herePrev.z, herePrev.z);
	bool okC = valid && HistoryDepthMatches(p, zLo, zHi);
	float3 histB = ToYCoCg(Sane(SampleHistory(pxB)));
	float3 histA = ToYCoCg(Sane(tex2Dlod(s_History, float4(PixUV(pxA), 0, 0)).rgb));
	float3 histC = ToYCoCg(Sane(tex2Dlod(s_HistPoint, float4(PixUV(p), 0, 0)).rgb));
	float dB = okB ? length(histB - filtered) : 1e6;
	float dA = okA ? length(histA - filtered) : 1e6;
	float dC = okC ? length(histC - filtered) : 1e6;
	float3 hist = histB;
	float2 motion = motionB;
	float dCam = dB;
	if (dA < dB)
	{
		hist = histA;
		motion = motionA;
		dCam = dA;
	}
	float which = dCam < 1e6 ? 1 : 0; // debug: 1 = camera motion (B or A), 2 = moved with the camera (C), 0 = none
	if (okC && (dCam >= 1e6 || (dCam > 0.03 && dC < 0.5 * dCam)))
	{
		which = 2;
		hist = histC;
		motion = 0;
	}

	float3 result;
	if (which > 0)
	{
		// A history far from this frame's estimate, measured against the local contrast, belongs to
		// something else (uncovered from behind something whose depth matched): trust it less. A
		// converged edge sits mid-range and keeps full weight.
		float contrast = length(hi - lo) + 0.01;
		// Only in motion: with nothing moving no surface can have been uncovered, and on a still
		// edge the single-frame estimate swings with the jitter enough to trip this (shimmer).
		float speed = length(motion);
		float distrust = saturate((length(hist - filtered) / contrast - c_Params2.w) / (1.0 - c_Params2.w)) * saturate(speed * 2.0);
		hist = ClipToBox(hist, boxLo, boxHi);
		float alpha = lerp(c_Params.x, c_Params.y, saturate(speed / c_Params.z));
		result = lerp(lerp(hist, centre, alpha), filtered, distrust);
	}
	else
		result = filtered; // nothing usable from the past: the jitter-free estimate (doesn't shake)

	float3 rgb = FromYCoCg(result);
	if (c_Jitter.w == 1)
		rgb = Compress(which == 1 ? float3(0, 0.6, 0) : which == 2 ? float3(0, 0.2, 1.0) : float3(1.0, 0, 0)) * 0.5 + rgb * 0.5;
	else if (c_Jitter.w == 2)
		rgb = Compress(float3(saturate(abs(motion) / 32.0), 0));
#ifdef TAA_D3D11
	else if (c_Jitter.w == 3) // tests: raw values
		rgb = float3(motion, which);
	else if (c_Jitter.w == 4)
		rgb = SampleHistory(p);
	else if (c_Jitter.w == 5)
		rgb = FromYCoCg(centre);
#endif
	return float4(rgb, zHere);
}

// Writes the new history into the scene colour (colour channels only: the game's alpha is kept),
// with a sharpen limited to the neighbours' range so it can't ring.
float4 PS_Output(VPOS_T vpos : VPOS) : COLOR0
{
	float2 p = PIXEL(vpos);
	float3 c = tex2Dlod(s_History, float4(PixUV(p), 0, 0)).rgb;
	float s = c_Params2.x;
	if (s > 0)
	{
		float3 n = tex2Dlod(s_History, float4(PixUV(p + float2(0, -1)), 0, 0)).rgb;
		float3 so = tex2Dlod(s_History, float4(PixUV(p + float2(0, 1)), 0, 0)).rgb;
		float3 e = tex2Dlod(s_History, float4(PixUV(p + float2(1, 0)), 0, 0)).rgb;
		float3 w = tex2Dlod(s_History, float4(PixUV(p + float2(-1, 0)), 0, 0)).rgb;
		float3 lo = min(c, min(min(n, so), min(e, w))), hi = max(c, max(max(n, so), max(e, w)));
		c = clamp(c + (c - 0.25 * (n + so + e + w)) * s, lo, hi);
	}
	return float4(Expand(c), 1.0);
}
