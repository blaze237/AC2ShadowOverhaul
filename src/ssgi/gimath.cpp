// Camera and reprojection maths for the SSGI passes and the rewritten shaders. No device calls, so
// tests/gimath_test.cpp checks it directly.
//
// Conventions (as in AC2ShadowOverhaul's AO, verified in game): the game's matrices are applied as
// clip = M * position (constant registers hold the rows). A full-res pixel p (integer, D3D9 pixel
// centres) has NDC (2 p.x / W - 1, 1 - 2 p.y / H). Our half-res textures hold full-res pixel 2i in
// texel i, so pixel p is read at uv = (p / 2 + 0.5) / halfSize.

#include "gi.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace ssgi
{

bool Invert(const Mat4& in, Mat4& out)
{
	double a[4][8];
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 8; c++)
			a[r][c] = c < 4 ? in.m[r][c] : (c - 4 == r ? 1.0 : 0.0);
	for (int c = 0; c < 4; c++)
	{
		int piv = c;
		for (int r = c + 1; r < 4; r++)
			if (fabs(a[r][c]) > fabs(a[piv][c]))
				piv = r;
		if (fabs(a[piv][c]) < 1e-12)
			return false;
		for (int k = 0; k < 8; k++)
		{
			double t = a[c][k];
			a[c][k] = a[piv][k];
			a[piv][k] = t;
		}
		double inv = 1.0 / a[c][c];
		for (int k = 0; k < 8; k++)
			a[c][k] *= inv;
		for (int r = 0; r < 4; r++)
			if (r != c)
			{
				double f = a[r][c];
				for (int k = 0; k < 8; k++)
					a[r][k] -= f * a[c][k];
			}
	}
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++)
			out.m[r][c] = a[r][c + 4];
	return true;
}

Mat4 Mul(const Mat4& a, const Mat4& b)
{
	Mat4 o;
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++)
			o.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] + a.m[r][2] * b.m[2][c] + a.m[r][3] * b.m[3][c];
	return o;
}

static void HalfSize(unsigned w, unsigned h, double& hw, double& hh)
{
	hw = (double)((w + 1) / 2);
	hh = (double)((h + 1) / 2);
}

bool ReprojectionRows(const Mat4& prevViewProj, const Mat4& curViewProj, unsigned w, unsigned h, float rows[3][4])
{
	Mat4 inv;
	if (!Invert(curViewProj, inv))
		return false;
	double hw, hh;
	HalfSize(w, h, hw, hh);
	// (z/w, x, y, 1) -> NDC (homogeneous, w = 1)
	const Mat4 S = { { { 0, 2.0 / w, 0, -1 }, { 0, 0, -2.0 / h, 1 }, { 1, 0, 0, 0 }, { 0, 0, 0, 1 } } };
	// last frame's clip -> (u*w, v*w, w, unused)
	const double sx = w / (4.0 * hw), sy = h / (4.0 * hh);
	const Mat4 T = { { { sx, 0, 0, sx + 0.5 / hw }, { 0, -sy, 0, sy + 0.5 / hh }, { 0, 0, 0, 1 }, { 0, 0, 0, 0 } } };
	Mat4 M = Mul(T, Mul(prevViewProj, Mul(inv, S)));
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 4; c++)
			rows[r][c] = (float)M.m[r][c];
	return true;
}

void IdentityRows(unsigned w, unsigned h, float rows[3][4])
{
	double hw, hh;
	HalfSize(w, h, hw, hh);
	const float r[3][4] = {
		{ 0, (float)(0.5 / hw), 0, (float)(0.5 / hw) },
		{ 0, 0, (float)(0.5 / hh), (float)(0.5 / hh) },
		{ 0, 0, 0, 1 },
	};
	memcpy(rows, r, sizeof(r));
}

bool CameraFromMatrices(const float wvp[16], const float world[16], Camera& cam, char* why, size_t whySize)
{
	Mat4 M, Wm, Wi;
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++)
		{
			M.m[r][c] = wvp[r * 4 + c];
			Wm.m[r][c] = world[r * 4 + c];
		}
	if (!Invert(Wm, Wi))
	{
		snprintf(why, whySize, "world matrix not invertible");
		return false;
	}
	Mat4 VP = Mul(M, Wi);
	auto dot3 = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
	double s = sqrt(dot3(VP.m[3], VP.m[3]));
	if (fabs(s - 1.0) > 0.01)
	{
		snprintf(why, whySize, "not a standard perspective projection (|w row| = %g)", s);
		return false;
	}
	double f[3] = { VP.m[3][0] / s, VP.m[3][1] / s, VP.m[3][2] / s };
	double A = dot3(VP.m[2], f) / s, B = VP.m[2][3] - A * VP.m[3][3];
	double p02 = dot3(VP.m[0], f) / s, p12 = dot3(VP.m[1], f) / s;
	double r0[3], r1[3], res[3];
	for (int k = 0; k < 3; k++)
	{
		r0[k] = VP.m[0][k] - p02 * VP.m[3][k];
		r1[k] = VP.m[1][k] - p12 * VP.m[3][k];
		res[k] = VP.m[2][k] - A * VP.m[3][k];
	}
	double p00 = sqrt(dot3(r0, r0)), p11 = sqrt(dot3(r1, r1));
	double near_ = A != 0 ? -B / A : 0;
	if (sqrt(dot3(res, res)) > 1e-3 * fabs(A) || !(A > 0.5) || !(B < 0) || !(near_ > 0) || p00 < 1e-3 || p11 < 1e-3 ||
		fabs(dot3(r0, r1)) > 1e-3 * p00 * p11)
	{
		snprintf(why, whySize, "unexpected projection (A %g, B %g, P00 %g, P11 %g)", A, B, p00, p11);
		return false;
	}
	Mat4 inv;
	if (!Invert(VP, inv))
	{
		snprintf(why, whySize, "view-projection not invertible");
		return false;
	}
	// The eye is where view space is (0, 0, 0, 1), i.e. clip (0, 0, B, 0).
	double e[4];
	for (int r = 0; r < 4; r++)
		e[r] = inv.m[r][2] * B;
	if (fabs(e[3]) < 1e-9)
	{
		snprintf(why, whySize, "eye at infinity");
		return false;
	}
	cam.viewProj = VP;
	cam.A = (float)A;
	cam.B = (float)B;
	cam.P00 = (float)p00;
	cam.P11 = (float)p11;
	cam.P02 = (float)p02;
	cam.P12 = (float)p12;
	for (int k = 0; k < 3; k++)
	{
		cam.eye[k] = e[k] / e[3];
		cam.forward[k] = f[k];
	}
	cam.ok = true;
	return true;
}

} // namespace ssgi
