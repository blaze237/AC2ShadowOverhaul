// AC2Volumetrics: volumetric fog lit through the game's own sun shadow map (so the fog shows
// light shafts wherever sunlight gets between buildings), and screen-space sun shafts streaming
// from the bright sky around the sun.
//
// Compiled by tools/gen_shaders.c into src/shaders_gen.h - run it after editing this file.
//
// Passes (all full-screen triangles):
//   PS_FogMarch    fog resolution    ray march camera -> scene through the fog: rgb = light scattered
//                                    towards the camera, a = transmittance (share of the scene left)
//   PS_FogBlur     fog resolution    depth-aware blur, once across and once down
//   PS_ShaftsMask  shafts resolution sky pixels near the sun (by brightness)
//   PS_ShaftsBlur  shafts resolution radial blur towards the sun, twice (coarse, then fine steps)
//   PS_Composite   full resolution   depth-aware upsampling of the fog, plus the shafts; blended
//                                    onto the scene as scene * a + rgb
//
// Coordinates: full-res pixel p (integer, D3D9 pixel centres) samples at uv = (p + 0.5) / size and
// corresponds to NDC (2p / size - 1, 1 - 2p / size). World space is the game's: metres, z up.
// The camera is described by its world axes and position plus the projection terms, so view rays
// and distances are exact at any distance from the world origin.

float4 c_Full     : register(c0);  // full-res width, height, 1/width, 1/height
float4 c_Low      : register(c1);  // fog-res width, height, 1/width, 1/height
float4 c_Proj     : register(c2);  // A, B (z/w = A + B / z), P00, P11
float4 c_Proj2    : register(c3);  // P02, P12 (with TAA's jitter folded in), fog downscale factor, z/w at or above which a pixel is sky
float4 c_Eye      : register(c4);  // camera position (world), fog distance limit (m)
float4 c_Right    : register(c5);  // camera right (world), ray march steps
float4 c_Up       : register(c6);  // camera up (world), view mode (0 normal, 1 fog only, 2 transmittance, 3 shafts only)
float4 c_Fwd      : register(c7);  // camera forward (world), shafts downscale factor
float4 c_Light0   : register(c8);  // world -> sun shadow space, one row each (x, y across the sun in
float4 c_Light1   : register(c9);  //   metres, centred near the camera; z along the direction
float4 c_Light2   : register(c10); //   towards the sun) - the game's g_WorldToLightProjVS
float4 c_ShadowZ  : register(c11); // atlas depth = z * x + y (g_ShadowNearFar); z = 1: shadow map available
float4 c_RangeMin : register(c12); // cascade ring per cascade, coarsest first (g_ShadowCascadesRangesMin/Max):
float4 c_RangeMax : register(c13); //   a point belongs to the cascade with min <= max(|x|, |y|) <= max
float4 c_ScaleX   : register(c14); // atlas uv = light xy * scale + offset, per cascade (g_ShadowScales/Offsets)
float4 c_ScaleY   : register(c15);
float4 c_OffsetX  : register(c16);
float4 c_OffsetY  : register(c17);
float4 c_Bias     : register(c18); // depth bias per cascade (g_LightDirShadowBiasPerCascade)
float4 c_Fade     : register(c19); // shadow fade-out towards the cascades' edge: saturate((r + x) * y); z = 1: cloud shadows
float4 c_Cloud    : register(c20); // cloud shadow uv = world xy * xy + zw (g_CloudUVScaleOffset)
float4 c_SunDir   : register(c21); // direction towards the sun (world), Henyey-Greenstein g
float4 c_SunCol   : register(c22); // sun colour x sun scattering strength
float4 c_Ambient  : register(c23); // sky light scattered by the fog (the game's fog colour x ambient strength)
float4 c_Medium   : register(c24); // density at and below the base height (1/m; 0 = fog off), height falloff (1/m), base height (m),
                                   //   noise offset (added to the step noise: 0 without TAA, see PS_FogMarch)
float4 c_Blur     : register(c25); // blur direction (fog-res pixels)
float4 c_Shafts   : register(c26); // sun position (screen uv), step (share of the way to the sun), decay per step
float4 c_Shafts2  : register(c27); // sky brightness threshold, sun falloff exponent
float4 c_ShaftCol : register(c28); // shaft colour (sun colour x strength x fade), w = 1: shafts on
float4 c_ShRes    : register(c29); // shafts-res width, height, 1/width, 1/height

sampler2D s_Depth  : register(s0); // game z/w, R32F, full res (point, clamp)
sampler2D s_Shadow : register(s1); // game sun shadow atlas, R32F (point, clamp)
sampler2D s_Cloud  : register(s2); // game cloud shadow texture (linear, wrap)
sampler2D s_Fog    : register(s3); // fog-res in-scatter + transmittance (point, clamp)
sampler2D s_Scene  : register(s4); // scene colour (linear, clamp)
sampler2D s_Shafts : register(s5); // shafts-res (linear, border = 0)

// tests/warp_test.cpp runs these shaders on D3D11 (WARP), where the pixel position is the pixel
// centre (x + 0.5) rather than D3D9's integer VPOS.
#ifdef VOL_D3D11
#define VPOS_T float4
#define PIXEL(v) ((v).xy - 0.5)
#else
#define VPOS_T float2
#define PIXEL(v) (v)
#endif

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

float LinearZ(float d) { return c_Proj.y / (d - c_Proj.x); }

float FetchD(float2 px) // full-res pixel
{
	return tex2Dlod(s_Depth, float4((px + 0.5) * c_Full.zw, 0, 0)).x;
}

// View z for blur / upsampling weights. Everything past the fog's distance limit gets the same
// fog, so it counts as equally far.
float FogZ(float d)
{
	return d >= c_Proj2.w ? c_Eye.w : min(LinearZ(d), c_Eye.w);
}

// View-space ray through a full-res pixel, scaled to view z = 1.
float3 ViewRay(float2 px)
{
	float2 ndc = float2(px.x * 2.0 * c_Full.z - 1.0, 1.0 - px.y * 2.0 * c_Full.w);
	return float3((ndc - c_Proj2.xy) / c_Proj.zw, 1.0);
}

float3 WorldDir(float3 viewRay)
{
	return normalize(viewRay.x * c_Right.xyz + viewRay.y * c_Up.xyz + c_Fwd.xyz);
}

// The full-res pixel a fog-res pixel stands for.
float2 FogToFull(float2 lp)
{
	return min(lp * c_Proj2.z + floor(c_Proj2.z * 0.5), c_Full.xy - 1.0);
}

float LowZ(float2 lp) { return FogZ(FetchD(FogToFull(lp))); }

float4 FetchFog(float2 lp) { return tex2Dlod(s_Fog, float4((lp + 0.5) * c_Low.zw, 0, 0)); }

// Interleaved gradient noise (Jimenez 2014): offsets each pixel's ray march samples so that the
// blur turns banding into a smooth result.
float Noise(float2 p)
{
	return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// ---- Fullscreen triangle -----------------------------------------------------------------------

float4 VS_Fullscreen(float4 p : POSITION) : POSITION
{
	return p;
}

// ---- Fog ray march -----------------------------------------------------------------------------

// Sunlight reaching a point (0..1): the game's own cascade choice, atlas lookup and depth test,
// one tap. Past the cascades it fades to lit like the game's shading does.
float SunVisibility(float3 ls)
{
	if (c_ShadowZ.z <= 0.0)
		return 1.0;
	float r = max(abs(ls.x), abs(ls.y));
	float4 inCascade = step(r, c_RangeMax) * step(c_RangeMin, r);
	float2 uv = float2(dot(inCascade, c_ScaleX), dot(inCascade, c_ScaleY)) * ls.xy + float2(dot(inCascade, c_OffsetX), dot(inCascade, c_OffsetY));
	float depth = ls.z * c_ShadowZ.x + c_ShadowZ.y + dot(inCascade, c_Bias);
	float stored = tex2Dlod(s_Shadow, float4(uv, 0, 0)).x;
	float lit = stored >= depth ? 1.0 : 0.0;
	float fade = saturate((r + c_Fade.x) * c_Fade.y);
	return dot(inCascade, 1.0) > 0.0 ? lerp(lit, 1.0, fade) : 1.0;
}

// Henyey-Greenstein phase function scaled so that isotropic scattering (g = 0) is 1.
float Phase(float cosTheta, float g)
{
	float g2 = g * g;
	return (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5);
}

float4 PS_FogMarch(VPOS_T vposIn : VPOS) : COLOR
{
	float2 lp = PIXEL(vposIn);
	float2 fp = FogToFull(lp);
	float d = FetchD(fp);
	float3 vr = ViewRay(fp);
	float3 dir = WorldDir(vr);
	float dist = d >= c_Proj2.w ? c_Eye.w : LinearZ(d) * length(vr);
	float tEnd = min(dist, c_Eye.w);

	// Light space along the ray: ls(t) = ls0 + lsDir * t.
	float3 eye = c_Eye.xyz;
	float3 ls0 = float3(dot(c_Light0.xyz, eye) + c_Light0.w, dot(c_Light1.xyz, eye) + c_Light1.w, dot(c_Light2.xyz, eye) + c_Light2.w);
	float3 lsDir = float3(dot(c_Light0.xyz, dir), dot(c_Light1.xyz, dir), dot(c_Light2.xyz, dir));
	float3 sunLight = c_SunCol.rgb * Phase(dot(dir, c_SunDir.xyz), c_SunDir.w);

	float steps = c_Right.w;
	// The step offset: interleaved gradient noise plus c_Medium.w, which is 0 without TAA (the same
	// pattern every frame, as the blur expects; frac(n + 0) = n) and a low-discrepancy sequence over
	// the frames while TAA averages them (core::Scene_NoiseOffset).
	float jitter = frac(Noise(lp) + c_Medium.w);
	float transmittance = 1.0;
	float3 scattered = 0;
	// Samples crowd near the camera (t ~ x^2), where shafts and the fog's height change matter most.
	[loop] for (float i = 0; i < steps; i += 1.0)
	{
		float x0 = i / steps, x1 = (i + 1.0) / steps;
		float t0 = tEnd * x0 * x0, t1 = tEnd * x1 * x1;
		float t = lerp(t0, t1, jitter);
		float3 P = eye + dir * t;
		float density = c_Medium.x * exp(-c_Medium.y * max(P.z - c_Medium.z, 0.0));
		float3 sun = sunLight * SunVisibility(ls0 + lsDir * t);
		if (c_Fade.z > 0.0)
			sun *= tex2Dlod(s_Cloud, float4(P.xy * c_Cloud.xy + c_Cloud.zw, 0, 0)).rgb;
		// Exact for light and density constant over the step (albedo 1: all extinction is scattering).
		float stepT = exp(-density * (t1 - t0));
		scattered += transmittance * (sun + c_Ambient.rgb) * (1.0 - stepT);
		transmittance *= stepT;
	}
	return float4(scattered, transmittance);
}

// ---- Fog blur (separable, depth-aware) ---------------------------------------------------------

float DepthWeight(float z, float zRef, float tolerance)
{
	return saturate(1.0 - abs(z - zRef) / (tolerance * zRef + 0.25));
}

float4 PS_FogBlur(VPOS_T vposIn : VPOS) : COLOR
{
	float2 p = PIXEL(vposIn);
	float zc = LowZ(p);
	float4 sum = FetchFog(p);
	float wsum = 1.0;
	static const float kGauss[4] = { 0.8825, 0.6065, 0.3247, 0.1353 }; // exp(-k^2 / 8)
	[unroll] for (int k = 1; k <= 4; k++)
	{
		[unroll] for (int side = -1; side <= 1; side += 2)
		{
			float2 q = clamp(p + c_Blur.xy * (k * side), 0.0, c_Low.xy - 1.0);
			float w = kGauss[k - 1] * DepthWeight(LowZ(q), zc, 0.1);
			sum += FetchFog(q) * w;
			wsum += w;
		}
	}
	return sum / wsum;
}

// ---- Screen-space sun shafts -------------------------------------------------------------------

float4 PS_ShaftsMask(VPOS_T vposIn : VPOS) : COLOR
{
	float2 sp = PIXEL(vposIn);
	float div = c_Fwd.w;
	float2 centre = min(sp * div + div * 0.5, c_Full.xy - 1.0); // between the block's middle pixels
	float2 px = floor(centre);
	if (FetchD(px) < c_Proj2.w)
		return 0;
	float3 col = tex2Dlod(s_Scene, float4(centre * c_Full.zw, 0, 0)).rgb; // bilinear: 2x2 average
	float lum = dot(col, kLuma);
	float bright = saturate((lum - c_Shafts2.x) / max(1.0 - c_Shafts2.x, 0.05));
	float towardsSun = pow(saturate(dot(WorldDir(ViewRay(centre)), c_SunDir.xyz)), c_Shafts2.y);
	return bright * towardsSun;
}

float4 PS_ShaftsBlur(VPOS_T vposIn : VPOS) : COLOR
{
	float2 sp = PIXEL(vposIn);
	float2 uv = (sp + 0.5) * c_ShRes.zw;
	float2 delta = (c_Shafts.xy - uv) * c_Shafts.z;
	float4 sum = 0;
	float w = 1.0, wsum = 0.0;
	[unroll] for (int i = 0; i < 16; i++)
	{
		sum += tex2Dlod(s_Shafts, float4(uv, 0, 0)) * w;
		wsum += w;
		w *= c_Shafts.w;
		uv += delta;
	}
	return sum / wsum;
}

// ---- Composite ---------------------------------------------------------------------------------

float4 PS_Composite(VPOS_T vposIn : VPOS) : COLOR
{
	float2 p = PIXEL(vposIn);
	float4 fog = float4(0, 0, 0, 1);
	if (c_Medium.x > 0.0)
	{
		// Bilinear upsampling, with each of the 4 fog-res pixels weighted down when it stands for a
		// different depth (so fog doesn't bleed across silhouettes).
		float zf = FogZ(FetchD(p));
		float2 lc = (p + 0.5) / c_Proj2.z - 0.5;
		float2 base = floor(lc);
		float2 f = lc - base;
		float4 sum = 0;
		float wsum = 0;
		static const float2 kCorner[4] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 1) };
		[unroll] for (int k = 0; k < 4; k++)
		{
			float2 o = kCorner[k];
			float2 q = clamp(base + o, 0.0, c_Low.xy - 1.0);
			float2 bw2 = lerp(1.0 - f, f, o);
			float w = bw2.x * bw2.y * (1e-3 + DepthWeight(LowZ(q), zf, 0.05));
			sum += FetchFog(q) * w;
			wsum += w;
		}
		fog = sum / wsum;
		// The sky keeps its full brightness: the game's sky already has its haze, and dimming it would
		// dim the HDR sun too - and with it the game's bloom, which is a plain blur of the scene.
		// The air in front of it still adds its light.
		if (FetchD(p) >= c_Proj2.w)
			fog.a = 1.0;
	}
	float3 shafts = 0;
	if (c_ShaftCol.w > 0.0)
		shafts = tex2Dlod(s_Shafts, float4((p + 0.5) / c_Fwd.w * c_ShRes.zw, 0, 0)).rgb * c_ShaftCol.rgb;

	float view = c_Up.w;
	if (view == 1.0)
		return float4(fog.rgb, 0);
	if (view == 2.0)
		return float4(fog.aaa, 0);
	if (view == 3.0)
		return float4(shafts, 0);
	return float4(fog.rgb + shafts, fog.a); // blended: scene * a + rgb
}
