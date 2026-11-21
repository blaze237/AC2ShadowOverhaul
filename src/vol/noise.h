// The fog's 3D noise volume, made on the CPU once (no file to ship): tiling gradient (Perlin) noise,
// three octaves, in an N^3 cube that repeats in every direction, with its mip chain. Header-only so
// tests/vol/warp_test.cpp runs the shaders on exactly the volume the plugin uploads.
//
// The octaves have 8, 16 and 32 lattice cells across the cube (N = 64: blobs of 8, 4 and 2 texels),
// so the finest detail still spans two texels and linear filtering keeps it smooth. The plugin
// scales the cube to 8 x [VolumetricFog] NoiseSize metres: NoiseSize is the size of the largest
// blobs. Values are remapped to mean 0.5 (the shader turns them into a density factor of
// 1 +- amount, the same fog on average), +-3 standard deviations across 0..1.
#pragma once
#include <math.h>
#include <stdint.h>
#include <vector>

namespace vol
{

namespace noise_detail
{

inline uint32_t Hash(uint32_t x, uint32_t y, uint32_t z, uint32_t seed)
{
	uint32_t h = x * 0x8DA6B343u ^ y * 0xD8163841u ^ z * 0xCB1AB31Fu ^ seed * 0x9E3779B9u;
	h ^= h >> 15;
	h *= 0x2C1B3C6Du;
	h ^= h >> 12;
	h *= 0x297A2D39u;
	h ^= h >> 15;
	return h;
}

// Gradient noise with period `p` lattice cells (coordinates in cells, 0..p): about -1..1.
inline float Perlin(float x, float y, float z, int p, uint32_t seed)
{
	// The 12 edge directions of a cube (Perlin 2002).
	static const float g[12][3] = { { 1, 1, 0 }, { -1, 1, 0 }, { 1, -1, 0 }, { -1, -1, 0 }, { 1, 0, 1 }, { -1, 0, 1 }, { 1, 0, -1 }, { -1, 0, -1 },
		{ 0, 1, 1 }, { 0, -1, 1 }, { 0, 1, -1 }, { 0, -1, -1 } };
	int ix = (int)floorf(x), iy = (int)floorf(y), iz = (int)floorf(z);
	float fx = x - ix, fy = y - iy, fz = z - iz;
	auto fade = [](float t) { return t * t * t * (t * (t * 6 - 15) + 10); };
	float u = fade(fx), v = fade(fy), w = fade(fz);
	float c[2][2][2];
	for (int k = 0; k < 2; k++)
		for (int j = 0; j < 2; j++)
			for (int i = 0; i < 2; i++)
			{
				// Lattice points wrap at the period: the cube tiles.
				uint32_t hx = (uint32_t)(((ix + i) % p + p) % p), hy = (uint32_t)(((iy + j) % p + p) % p), hz = (uint32_t)(((iz + k) % p + p) % p);
				const float* d = g[Hash(hx, hy, hz, seed) % 12];
				c[k][j][i] = d[0] * (fx - i) + d[1] * (fy - j) + d[2] * (fz - k);
			}
	auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
	float x00 = lerp(c[0][0][0], c[0][0][1], u), x10 = lerp(c[0][1][0], c[0][1][1], u);
	float x01 = lerp(c[1][0][0], c[1][0][1], u), x11 = lerp(c[1][1][0], c[1][1][1], u);
	return lerp(lerp(x00, x10, v), lerp(x01, x11, v), w);
}

} // namespace noise_detail

// levels[0] = n^3 bytes (x fastest, then y, then z), levels[k] = (n >> k)^3, down to 1^3. n: a power
// of two, at least 32.
inline void MakeNoiseVolume(int n, std::vector<std::vector<uint8_t>>& levels)
{
	using namespace noise_detail;
	const int periods[3] = { n / 8, n / 4, n / 2 };
	const float amps[3] = { 0.5f, 0.25f, 0.125f };
	std::vector<float> v((size_t)n * n * n);
	double sum = 0, sum2 = 0;
	for (int z = 0; z < n; z++)
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++)
			{
				float f = 0;
				for (int o = 0; o < 3; o++)
				{
					float s = (float)periods[o] / n; // cells per texel
					f += amps[o] * Perlin((x + 0.5f) * s, (y + 0.5f) * s, (z + 0.5f) * s, periods[o], 17u + o);
				}
				v[((size_t)z * n + y) * n + x] = f;
				sum += f;
				sum2 += (double)f * f;
			}
	const double count = (double)v.size(), mean = sum / count, sd = sqrt(fmax(sum2 / count - mean * mean, 1e-12));
	levels.assign(1, std::vector<uint8_t>(v.size()));
	for (size_t i = 0; i < v.size(); i++)
	{
		double q = 0.5 + (v[i] - mean) / (6.0 * sd);
		q = q < 0 ? 0 : q > 1 ? 1 : q;
		levels[0][i] = (uint8_t)(q * 255.0 + 0.5);
	}
	// Mips: 2x2x2 box filter (n is a power of two, so no texel straddles the wrap).
	for (int s = n / 2; s >= 1; s /= 2)
	{
		const std::vector<uint8_t>& up = levels.back();
		const int us = s * 2;
		std::vector<uint8_t> d((size_t)s * s * s);
		for (int z = 0; z < s; z++)
			for (int y = 0; y < s; y++)
				for (int x = 0; x < s; x++)
				{
					int acc = 0;
					for (int k = 0; k < 8; k++)
						acc += up[((size_t)(2 * z + (k >> 2)) * us + (2 * y + ((k >> 1) & 1))) * us + (2 * x + (k & 1))];
					d[((size_t)z * s + y) * s + x] = (uint8_t)((acc + 4) / 8);
				}
		levels.push_back(std::move(d));
	}
}

} // namespace vol
