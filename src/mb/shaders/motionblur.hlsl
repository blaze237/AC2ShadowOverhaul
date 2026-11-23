// AC2 motion blur: every pixel's motion from the camera's, then a reconstruction filter (McGuire et
// al. 2012, "A Reconstruction Filter for Plausible Motion Blur", with Jimenez 2014's tile jitter and
// split sampling) on the HDR scene colour, before bloom and tone mapping (motionblur.cpp).
//
// Compiled by tools/gen_shaders.c into src/mb/shaders_gen.h - run it after editing this file.
//
// Units: velocities are what a pixel moved over the last frame, now minus then, in pixels (x right,
// y down). The prep pass scales them by the settings, halves them and clamps them to the tile size
// K: from there on a velocity v means "blurred from -v to +v" (the streak is centred on the pixel and
// 2|v| long), as in McGuire et al.'s filter. Pixel p (integer in D3D9, the pixel centre) samples at
// uv = (p + 0.5) / size and has NDC (2p / size - 1, 1 - 2p / size).
//
// Pass order:
//   PS_Prep            per pixel: the camera's motion (reprojection from the z/w target), as the
//                      final half-velocity in pixels, and the linear depth
//   PS_TileMax         the longest velocity in each K x K tile
//   PS_NeighborMax     ... and in its 3 x 3 tile neighbourhood (how far blur can reach into a pixel)
//   PS_Reconstruct     samples along the neighbourhood's velocity, weighted by depth and by each
//                      sample's own velocity, into the scene colour

float4 c_Size   : register(c0); // width, height, 1/width, 1/height (of the full-res targets)
float4 c_Proj   : register(c1); // A, B (z/w = A + B / z), P00, P11 (of the jittered raster camera)
float4 c_Proj2  : register(c2); // P02, P12 (raster camera), this frame's jitter in NDC
float4 c_Reproj0 : register(c3); // rows: this frame's view space -> the previous frame's clip space (unjittered)
float4 c_Reproj1 : register(c4);
float4 c_Reproj2 : register(c5);
float4 c_Reproj3 : register(c6);
float4 c_Blur   : register(c7); // strength, shutter, 0, K (tile size = longest half-streak, pixels)
float4 c_Params : register(c8); // samples, soft depth extent (relative), debug view, 0
float4 c_Tiles  : register(c9); // tiles across, tiles down, 1 / tiles across, 1 / tiles down

sampler2D s_0 : register(s0);
sampler2D s_1 : register(s1);
sampler2D s_2 : register(s2);

// tests/mb/warp_test.cpp runs these on D3D11 (WARP), where the pixel position is the pixel centre.
#ifdef MB_D3D11
#define VPOS_T float4
#define PIXEL(v) ((v).xy - 0.5)
#else
#define VPOS_T float2
#define PIXEL(v) (v)
#endif

float2 PixUV(float2 px) { return (px + 0.5) * c_Size.zw; }
float4 Fetch(sampler2D s, float2 uv) { return tex2Dlod(s, float4(uv, 0, 0)); }

// Linear view depth (metres) from z/w; the sky and anything at or past the far plane count as far.
float LinearZ(float d)
{
	float z = c_Proj.y / (d - c_Proj.x);
	return (z > 0.0 && d < 0.99999) ? min(z, 1e4) : 1e4;
}

float4 VS_Fullscreen(float4 p : POSITION) : POSITION
{
	return p;
}

// ---- Prep: the velocity of every pixel -----------------------------------------------------------
// s_0 = z/w (point). Out: (half-velocity px, linear depth, 0).

float2 CameraVelocity(float2 p, float z)
{
	float2 ndc = float2(p.x * 2.0 * c_Size.z - 1.0, 1.0 - p.y * 2.0 * c_Size.w);
	float4 v = float4((ndc - c_Proj2.xy) / c_Proj.zw * z, z, 1.0);
	float4 clipPrev = float4(dot(c_Reproj0, v), dot(c_Reproj1, v), dot(c_Reproj2, v), dot(c_Reproj3, v));
	if (clipPrev.w < 1e-3)
		return 0;
	float2 ndcNow = ndc - c_Proj2.zw; // without the jitter, like the previous position
	return (ndcNow - clipPrev.xy / clipPrev.w) * float2(0.5, -0.5) * c_Size.xy;
}

float4 PS_Prep(VPOS_T vpos : VPOS) : COLOR0
{
	float2 p = PIXEL(vpos);
	float2 uv = PixUV(p);
	float z = LinearZ(Fetch(s_0, uv).x);
	float2 v = CameraVelocity(p, z) * (0.5 * c_Blur.x * c_Blur.y); // half: the streak runs from -v to +v
	float len = length(v);
	if (len > c_Blur.w)
		v *= c_Blur.w / len;
	return float4(v, z, 0);
}

// ---- Tiles ---------------------------------------------------------------------------------------
// s_0 = prep (point). One output pixel per K x K tile (K = c_Blur.w, at most 64).

float4 PS_TileMax(VPOS_T vpos : VPOS) : COLOR0
{
	float2 t = PIXEL(vpos);
	float K = c_Blur.w;
	float2 base = t * K;
	float2 best = 0;
	float bestLen = 0;
	[loop] for (int y = 0; y < 64; y++)
	{
		if (y >= K)
			break;
		[loop] for (int x = 0; x < 64; x++)
		{
			if (x >= K)
				break;
			float2 px = min(base + float2(x, y), c_Size.xy - 1.0);
			float2 v = Fetch(s_0, PixUV(px)).xy;
			float l = dot(v, v);
			if (l > bestLen)
			{
				bestLen = l;
				best = v;
			}
		}
	}
	return float4(best, 0, 0);
}

// s_0 = tile max (point).
float4 PS_NeighborMax(VPOS_T vpos : VPOS) : COLOR0
{
	float2 t = PIXEL(vpos);
	float2 best = 0;
	float bestLen = 0;
	[unroll] for (int k = 0; k < 9; k++)
	{
		float2 n = clamp(t + float2(k % 3 - 1, k / 3 - 1), 0.0, c_Tiles.xy - 1.0);
		float2 v = Fetch(s_0, (n + 0.5) * c_Tiles.zw).xy;
		float l = dot(v, v);
		if (l > bestLen)
		{
			bestLen = l;
			best = v;
		}
	}
	return float4(best, 0, 0);
}

// ---- Reconstruction ------------------------------------------------------------------------------
// s_0 = scene colour copy (linear), s_1 = prep (point), s_2 = neighbour max (point).
// c_Params.x = samples (at most 32), .y = soft depth extent (relative), .z = debug view.

float Cone(float dist, float len) { return saturate(1.0 - dist / max(len, 1e-3)); }
float Cylinder(float dist, float len) { return 1.0 - smoothstep(0.95 * len, 1.05 * len, dist); }
// 1 where depth a is in front of (or level with) depth b, fading to 0 when it is further by ext.
float InFront(float a, float b, float ext) { return saturate(1.0 - (a - b) / ext); }

// Velocity: hue = direction, brightness = length (K = full).
float4 DebugView(float4 colour, float4 prep)
{
	float2 d = prep.xy / max(c_Blur.w, 1.0);
	float3 c = float3(0.5 + 0.5 * d.x, 0.5 + 0.5 * d.y, 0.5) * saturate(length(d) * 2.0 + 0.15);
	return float4(c, colour.a);
}

// Two independent values in [0, 1) per pixel (interleaved gradient noise).
float2 Noise2(float2 p)
{
	return frac(52.9829189 * frac(float2(dot(p, float2(0.06711056, 0.00583715)), dot(p + 17.0, float2(0.00583715, 0.06711056)))));
}

float4 PS_Reconstruct(VPOS_T vpos : VPOS) : COLOR0
{
	float2 p = PIXEL(vpos);
	float2 uv = PixUV(p);
	float4 cX = Fetch(s_0, uv);
	float2 n = Noise2(p);
	// The neighbourhood's velocity, from a tile picked up to a quarter tile away at random: tile
	// edges, where the neighbourhood's velocity jumps, become a fine dither instead of hard box
	// outlines (Jimenez 2014).
	float2 tile = clamp(floor((p + (n - 0.5) * 0.5 * c_Blur.w) / c_Blur.w), 0.0, c_Tiles.xy - 1.0);
	float2 vMax = Fetch(s_2, (tile + 0.5) * c_Tiles.zw).xy;
	float4 prep = Fetch(s_1, uv);
	if (c_Params.z > 0.5)
		return DebugView(cX, prep);
	float lenMax = length(vMax);
	if (lenMax < 0.5)
		return cX;

	float2 vX = prep.xy;
	float zX = prep.z;
	float lenX = max(length(vX), 0.5);
	float ext = max(c_Params.y * zX, 0.05);
	// Shifts the sample positions per pixel so the steps don't band.
	float j = n.x - 0.5;
	float S = c_Params.x;
	// Every other sample follows the pixel's own motion instead of the neighbourhood's (where it has
	// one): near things and far things streak by their own amounts, and the tiles don't all smear
	// the same way (Jimenez 2014).
	float2 vOwn = length(vX) > 0.5 ? vX : vMax;

	float wX = 1.0 / lenX;
	float3 sum = cX.rgb * wX;
	float total = wX;
	[loop] for (int i = 0; i < 32; i++)
	{
		if (i >= S)
			break;
		float t = lerp(-1.0, 1.0, (i + j + 1.0) / (S + 1.0));
		float2 dir = frac(i * 0.5) > 0.25 ? vOwn : vMax; // odd samples
		float2 pxY = clamp(floor(p + dir * t + 0.5), 0.0, c_Size.xy - 1.0);
		float2 uvY = PixUV(pxY);
		float4 pY = Fetch(s_1, uvY);
		float dist = length(pxY - p); // to the pixel read (a sample rounded onto the next pixel is a pixel away)
		float lenY = length(pY.xy);
		float f = InFront(pY.z, zX, ext); // Y in front of X: Y's blur reaches X if X is within Y's own motion
		float b = InFront(zX, pY.z, ext); // Y behind X: X's blur shows Y if Y is within X's motion
		float w = f * Cone(dist, lenY) + b * Cone(dist, lenX) + Cylinder(dist, lenY) * Cylinder(dist, lenX) * 2.0;
		sum += Fetch(s_0, uvY).rgb * w;
		total += w;
	}
	return float4(sum / total, cX.a);
}
