// Checks the camera extraction and the reprojection rows (src/gimath.cpp) against cameras built
// here like the game's (left-handed perspective, near 0.1 m, far ~1779 m, P00 1.317; world z up):
//  - CameraFromMatrices recovers the view-projection, eye, forward and projection terms from
//    g_WorldViewProj and g_World, for an arbitrary object world matrix;
//  - ReprojectionRows maps (z/w, x, y, 1) of a pixel in the current frame to the texel of the same
//    world point in last frame's half-res texture, and w to its view depth then / now;
//  - IdentityRows maps each pixel onto itself.
#include "../src/gi.h"
#include <stdio.h>
#include <math.h>

static int g_fail = 0;
#define CHECK(cond, ...)                                                                                                              \
	do                                                                                                                                \
	{                                                                                                                                 \
		if (!(cond))                                                                                                                  \
		{                                                                                                                             \
			g_fail++;                                                                                                                 \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                               \
			printf(__VA_ARGS__);                                                                                                      \
			printf("\n");                                                                                                             \
		}                                                                                                                             \
	} while (0)

static const double kNear = 0.1, kFar = 1779.0, kP00 = 1.317;
static const unsigned W = 3840, H = 2160;

static void Normalize(double v[3])
{
	double l = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	for (int k = 0; k < 3; k++)
		v[k] /= l;
}
static void Cross(const double a[3], const double b[3], double o[3])
{
	o[0] = a[1] * b[2] - a[2] * b[1];
	o[1] = a[2] * b[0] - a[0] * b[2];
	o[2] = a[0] * b[1] - a[1] * b[0];
}

// View-projection for an eye looking at `target`, world z up (left-handed: right = up x forward).
static Mat4 MakeViewProj(const double eye[3], const double target[3], double fwdOut[3])
{
	double f[3] = { target[0] - eye[0], target[1] - eye[1], target[2] - eye[2] };
	Normalize(f);
	const double worldUp[3] = { 0, 0, 1 };
	double r[3], u[3];
	Cross(worldUp, f, r);
	Normalize(r);
	Cross(f, r, u);
	Mat4 V = {};
	const double* axes[3] = { r, u, f };
	for (int i = 0; i < 3; i++)
	{
		for (int k = 0; k < 3; k++)
			V.m[i][k] = axes[i][k];
		V.m[i][3] = -(axes[i][0] * eye[0] + axes[i][1] * eye[1] + axes[i][2] * eye[2]);
	}
	V.m[3][3] = 1;
	const double A = kFar / (kFar - kNear), B = -kNear * kFar / (kFar - kNear);
	Mat4 P = {};
	P.m[0][0] = kP00;
	P.m[1][1] = kP00 * W / H;
	P.m[2][2] = A;
	P.m[2][3] = B;
	P.m[3][2] = 1;
	for (int k = 0; k < 3; k++)
		fwdOut[k] = f[k];
	return Mul(P, V);
}

static void Apply(const Mat4& M, const double p[4], double o[4])
{
	for (int r = 0; r < 4; r++)
		o[r] = M.m[r][0] * p[0] + M.m[r][1] * p[1] + M.m[r][2] * p[2] + M.m[r][3] * p[3];
}

// Full-res pixel (D3D9 convention) and z/w of a world point; false if off screen.
static bool PixelOf(const Mat4& VP, const double world[3], double& px, double& py, double& zw, double& viewZ)
{
	double p[4] = { world[0], world[1], world[2], 1 }, c[4];
	Apply(VP, p, c);
	if (c[3] <= kNear)
		return false;
	double nx = c[0] / c[3], ny = c[1] / c[3];
	px = (nx + 1) * W / 2;
	py = (1 - ny) * H / 2;
	zw = c[2] / c[3];
	viewZ = c[3];
	return px >= 0 && py >= 0 && px < W && py < H;
}

int main()
{
	unsigned seed = 1;
	auto rnd = [&]() { return (seed = seed * 1664525u + 1013904223u) / 4294967296.0; };

	// ---- Camera extraction
	const double eye0[3] = { 120.5, -340.25, 31.0 }, target0[3] = { 160.0, -300.0, 25.0 };
	double fwd0[3];
	Mat4 VP0 = MakeViewProj(eye0, target0, fwd0);
	for (int trial = 0; trial < 20; trial++)
	{
		// Some object world matrix (rotation about z, scale, translation).
		double a = rnd() * 6.283, s = 0.5 + rnd() * 2;
		Mat4 Wm = {};
		Wm.m[0][0] = s * cos(a);
		Wm.m[0][1] = -s * sin(a);
		Wm.m[1][0] = s * sin(a);
		Wm.m[1][1] = s * cos(a);
		Wm.m[2][2] = s;
		Wm.m[0][3] = rnd() * 500 - 250;
		Wm.m[1][3] = rnd() * 500 - 250;
		Wm.m[2][3] = rnd() * 50;
		Wm.m[3][3] = 1;
		Mat4 WVP = Mul(VP0, Wm);
		float wvp[16], world[16];
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 4; c++)
			{
				wvp[r * 4 + c] = (float)WVP.m[r][c];
				world[r * 4 + c] = (float)Wm.m[r][c];
			}
		Camera cam;
		char why[160] = "";
		bool ok = CameraFromMatrices(wvp, world, cam, why, sizeof(why));
		CHECK(ok, "camera not extracted: %s", why);
		if (!ok)
			continue;
		double worst = 0;
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 4; c++)
				worst = fmax(worst, fabs(cam.viewProj.m[r][c] - VP0.m[r][c]) / (1 + fabs(VP0.m[r][c])));
		CHECK(worst < 2e-4, "view-projection off by %g", worst);
		for (int k = 0; k < 3; k++)
		{
			CHECK(fabs(cam.eye[k] - eye0[k]) < 0.05, "eye[%d] %g vs %g", k, cam.eye[k], eye0[k]);
			CHECK(fabs(cam.forward[k] - fwd0[k]) < 1e-4, "forward[%d] %g vs %g", k, cam.forward[k], fwd0[k]);
		}
		CHECK(fabs(cam.P00 - kP00) < 1e-4 && fabs(-cam.B / cam.A - kNear) < 1e-3, "projection P00 %g near %g", cam.P00, -cam.B / cam.A);
	}

	// ---- Reprojection: the camera moves and turns between frames
	const double eye1[3] = { 121.3, -339.6, 31.2 }, target1[3] = { 158.0, -297.0, 24.0 };
	double fwd1[3];
	Mat4 VP1 = MakeViewProj(eye1, target1, fwd1);
	float rows[3][4];
	CHECK(ReprojectionRows(VP0, VP1, W, H, rows), "rows not computed");
	const double hw = (W + 1) / 2, hh = (H + 1) / 2;
	int tested = 0;
	double worstUv = 0, worstZ = 0;
	for (int trial = 0; trial < 2000; trial++)
	{
		double wpt[3];
		double t = 2 + rnd() * 200;
		for (int k = 0; k < 3; k++)
			wpt[k] = eye1[k] + fwd1[k] * t + (rnd() - 0.5) * t;
		double x1, y1, zw1, vz1, x0, y0, zw0, vz0;
		if (!PixelOf(VP1, wpt, x1, y1, zw1, vz1) || !PixelOf(VP0, wpt, x0, y0, zw0, vz0))
			continue;
		tested++;
		float q[4] = { (float)zw1, (float)x1, (float)y1, 1 };
		double h[3];
		for (int r = 0; r < 3; r++)
			h[r] = (double)rows[r][0] * q[0] + (double)rows[r][1] * q[1] + (double)rows[r][2] * q[2] + (double)rows[r][3] * q[3];
		double u = h[0] / h[2], v = h[1] / h[2];
		double eu = (x0 / 2 + 0.5) / hw, ev = (y0 / 2 + 0.5) / hh;
		// In half-res texels; the float rows and float z/w limit precision far away.
		double err = fmax(fabs(u - eu) * hw, fabs(v - ev) * hh);
		worstUv = fmax(worstUv, err);
		worstZ = fmax(worstZ, fabs(h[2] * vz1 - vz0) / vz0);
	}
	CHECK(tested > 500, "only %d points on screen in both frames", tested);
	CHECK(worstUv < 0.25, "reprojected position off by %g half-res texels", worstUv);
	CHECK(worstZ < 1e-3, "reprojected depth off by %g (relative)", worstZ);
	printf("reprojection: %d points, worst %.4f half-res texels, depth %.2g relative\n", tested, worstUv, worstZ);

	// Same camera: every pixel maps onto itself.
	CHECK(ReprojectionRows(VP1, VP1, W, H, rows), "rows not computed");
	float idr[3][4];
	IdentityRows(W, H, idr);
	double worstSame = 0, worstId = 0;
	for (int trial = 0; trial < 200; trial++)
	{
		float q[4] = { (float)(0.9 + 0.0999 * rnd()), (float)floor(rnd() * W), (float)floor(rnd() * H), 1 };
		double eu = (q[1] / 2 + 0.5) / hw, ev = (q[2] / 2 + 0.5) / hh;
		double h[3], g[3];
		for (int r = 0; r < 3; r++)
		{
			h[r] = (double)rows[r][0] * q[0] + (double)rows[r][1] * q[1] + (double)rows[r][2] * q[2] + (double)rows[r][3] * q[3];
			g[r] = (double)idr[r][0] * q[0] + (double)idr[r][1] * q[1] + (double)idr[r][2] * q[2] + (double)idr[r][3] * q[3];
		}
		worstSame = fmax(worstSame, fmax(fabs(h[0] / h[2] - eu) * hw, fabs(h[1] / h[2] - ev) * hh));
		worstId = fmax(worstId, fmax(fabs(g[0] / g[2] - eu) * hw, fabs(g[1] / g[2] - ev) * hh));
	}
	CHECK(worstSame < 0.05, "static camera moves pixels by %g half-res texels", worstSame);
	CHECK(worstId < 1e-3, "identity rows move pixels by %g half-res texels", worstId);

	printf(g_fail ? "%d FAILURES\n" : "all camera / reprojection checks passed\n", g_fail);
	return g_fail ? 1 : 0;
}
