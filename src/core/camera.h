// The game camera, recovered from the shader constants of one draw, and the reprojection maths the
// temporal effects need. Plain maths (no device calls), so tests check it directly; the per-frame
// capture (which draw, current and previous frame) is in core/scene.h.
//
// Conventions (verified in game): the game's matrices are applied as clip = M * position, and its
// shader constant registers hold the matrix rows (clip.x = dot(position, c[wvp]) ...). The opaque
// shaders' g_WorldViewProj and g_World give ViewProj = WVP * World^-1, which for a D3D (left-handed)
// perspective projection is P * V with
//
//     P = [P00 0 P02 0; 0 P11 P12 0; 0 0 A B; 0 0 1 0]
//
// so the rows of ViewProj give everything directly: row 3 is (forward, -forward.eye), and A, B,
// P00, P11, P02, P12 and the right / up axes come from rows 0-2 once the forward part is taken out.
// View depth z (metres along forward) and the z/w the game writes to its z/w target: z/w = A + B / z.
// NDC: x = P00 x/z + P02, y = P11 y/z + P12. A full-res pixel p (integer, D3D9 pixel centres) has
// NDC (2 p.x / W - 1, 1 - 2 p.y / H).
#pragma once

#include <stddef.h>

namespace core
{

struct Mat4
{
	double m[4][4]; // rows
};
bool Invert(const Mat4& a, Mat4& out); // false if singular
Mat4 Mul(const Mat4& a, const Mat4& b);

// The camera's terms as the shaders get them (floats). Rounded once from the double values, so an
// effect that hands them on gets exactly what it would have computed in float itself.
struct CameraFloats
{
	float A, B, P00, P11, P02, P12;
	float right[3], up[3], forward[3]; // view axes in world space
	float eye[3];                      // camera position, world space
	float nearZ, farZ;                 // metres (farZ 1e6 for a projection without a far plane)
};

struct Camera
{
	bool ok = false;
	Mat4 viewProj;                  // world -> clip (rows)
	double A, B, P00, P11, P02, P12; // see above
	double right[3], up[3], forward[3];
	double eye[3];
	double nearZ, farZ;
	CameraFloats f;
};

// Camera from one draw's g_WorldViewProj and g_World (16 floats each, rows). Fails (with a reason)
// for anything but a standard perspective projection.
bool CameraFromMatrices(const float wvp[16], const float world[16], Camera& cam, char* why, size_t whySize);

// A camera cut (a teleport, a cutscene change): the camera moved more than 4 m or turned more than
// about 32 degrees from `prev`. History from `prev` is no use then. (SSGI's test, verified in game.)
bool IsCameraCut(const Camera& cur, const Camera& prev);

// ---- Reprojection -------------------------------------------------------------------------------

// SSGI's GI texture: rows mapping (z/w, x, y, 1) of a full-res pixel (x, y integer, D3D9 pixel
// centres) in the current frame to (u*w, v*w, w) of the same world point in a half-res texture
// rendered in the previous frame (texel i holds full-res pixel 2i, so pixel p is read at
// uv = (p / 2 + 0.5) / halfSize), w = its view depth then divided by its view depth now.
// Returns false if the current matrix can't be inverted.
bool ReprojectionRows(const Mat4& prevViewProj, const Mat4& curViewProj, unsigned w, unsigned h, float rows[3][4]);
// The same for "no previous frame": maps each pixel onto itself (w = 1).
void IdentityRows(unsigned w, unsigned h, float rows[3][4]);

// TAA's resolve: rows of the matrix taking this frame's view space (x right, y up, z forward,
// metres) to the previous frame's clip space = VP_prev * VP_cur^-1 * P_cur (P_cur takes view space
// to this frame's clip space). False if the current matrix can't be inverted.
bool ReprojectionMatrix(const Camera& cur, const Camera& prev, float out[4][4]);

} // namespace core
