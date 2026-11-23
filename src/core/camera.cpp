// See camera.h.
#include "core/camera.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace core
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

static double Det3(const double m[3][3])
{
	return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
		m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
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
	const Mat4 VP = Mul(M, Wi);
	auto dot3 = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
	// Row 3 is (forward, ...) with |forward| = 1 for a standard projection.
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
	// Row 2 must be A * row 3 (+ B), and right / up must be perpendicular: a plain perspective, no
	// oblique near plane or skew.
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
	// Camera position: clip x, y and w are 0 there.
	double sys[3][3] = { { VP.m[0][0], VP.m[0][1], VP.m[0][2] }, { VP.m[1][0], VP.m[1][1], VP.m[1][2] },
		{ VP.m[3][0], VP.m[3][1], VP.m[3][2] } };
	double rhs[3] = { -VP.m[0][3], -VP.m[1][3], -VP.m[3][3] };
	double det = Det3(sys);
	if (fabs(det) < 1e-12)
	{
		snprintf(why, whySize, "camera position not solvable");
		return false;
	}
	for (int k = 0; k < 3; k++)
	{
		double m[3][3];
		memcpy(m, sys, sizeof(m));
		for (int r = 0; r < 3; r++)
			m[r][k] = rhs[r];
		cam.eye[k] = Det3(m) / det;
	}
	cam.viewProj = VP;
	cam.A = A;
	cam.B = B;
	cam.P00 = p00;
	cam.P11 = p11;
	cam.P02 = p02;
	cam.P12 = p12;
	cam.nearZ = near_;
	cam.farZ = A > 1.0 ? B / (1.0 - A) : 1e6;
	for (int k = 0; k < 3; k++)
	{
		cam.right[k] = r0[k] / p00;
		cam.up[k] = r1[k] / p11;
		cam.forward[k] = f[k];
	}
	CameraFloats& F = cam.f;
	F.A = (float)A;
	F.B = (float)B;
	F.P00 = (float)p00;
	F.P11 = (float)p11;
	F.P02 = (float)p02;
	F.P12 = (float)p12;
	F.nearZ = (float)near_;
	F.farZ = A > 1.0 ? (float)(B / (1.0 - A)) : 1e6f;
	for (int k = 0; k < 3; k++)
	{
		F.right[k] = (float)cam.right[k];
		F.up[k] = (float)cam.up[k];
		F.forward[k] = (float)f[k];
		F.eye[k] = (float)cam.eye[k];
	}
	cam.ok = true;
	return true;
}

bool IsCameraCut(const Camera& cur, const Camera& prev)
{
	double d2 = 0, dot = 0;
	for (int k = 0; k < 3; k++)
	{
		double e = cur.eye[k] - prev.eye[k];
		d2 += e * e;
		dot += cur.forward[k] * prev.forward[k];
	}
	return d2 > 4.0 * 4.0 || dot < 0.85; // 4 m or about 32 degrees in one frame
}

Camera Jittered(const Camera& cam, float x, float y)
{
	Camera j = cam;
	if (x == 0.0f && y == 0.0f)
		return j;
	for (int c = 0; c < 4; c++)
	{
		j.viewProj.m[0][c] += x * cam.viewProj.m[3][c];
		j.viewProj.m[1][c] += y * cam.viewProj.m[3][c];
	}
	j.P02 = cam.P02 + x;
	j.P12 = cam.P12 + y;
	j.f.P02 = (float)j.P02;
	j.f.P12 = (float)j.P12;
	return j;
}

// ---- Reprojection -------------------------------------------------------------------------------

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

bool ReprojectionMatrix(const Camera& cur, const Camera& prev, float out[4][4])
{
	Mat4 inv, P = {};
	if (!Invert(cur.viewProj, inv))
		return false;
	P.m[0][0] = cur.P00;
	P.m[0][2] = cur.P02;
	P.m[1][1] = cur.P11;
	P.m[1][2] = cur.P12;
	P.m[2][2] = cur.A;
	P.m[2][3] = cur.B;
	P.m[3][2] = 1.0;
	const Mat4 M = Mul(prev.viewProj, Mul(inv, P));
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++)
			out[r][c] = (float)M.m[r][c];
	return true;
}

} // namespace core
