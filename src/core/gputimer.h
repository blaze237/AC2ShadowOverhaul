// GPU frame times of the effects, for the "Frame times" list ([Debug] ShowFrameTimes, F9 menu).
//
// Each effect brackets the passes it runs in a frame with GpuTimer_Begin / GpuTimer_End (D3D9
// timestamp queries); GpuTimer_EndFrame at Present closes the frame. The results are read back a few
// frames later without waiting (a ring of query sets) and smoothed. Nothing is issued while the list
// is off. Drivers without timestamp queries: logged once, the list says so.
//
// What is measured: the passes the effects add. Not the extra work they put into the game's own
// shaders (soft shadow filtering, AO's outputs, SSGI's bounce light in the lit shaders) or the
// bigger shadow maps, which run inside the game's draws.
#pragma once

#include <windows.h>
#include <d3d9.h>

namespace core
{

enum GpuTimerId
{
	GT_AO,   // ambient occlusion + contact shadows (and the shared half-res normals when AO makes them)
	GT_SSGI, // SSGI (and the shared normals when AO didn't run)
	GT_VOL,  // volumetric fog + sun shafts
	GT_TAA,
	GT_MB,   // motion blur (with its velocity replays)
	GT_COUNT
};
const char* GpuTimer_Name(int id);

void GpuTimer_SetOn(bool on);
bool GpuTimer_On();
// Around an effect's passes (no-ops while off). The device's calls: through the vtable as usual.
void GpuTimer_Begin(IDirect3DDevice9* dev, int id);
void GpuTimer_End(int id);
// Just before Present: closes this frame's measurements and reads finished ones back.
void GpuTimer_EndFrame(IDirect3DDevice9* dev);
void GpuTimer_OnLostDevice(); // releases the queries (made again when needed)

struct GpuTimes
{
	bool failed = false;           // the driver has no timestamp queries
	bool valid = false;            // at least one frame measured
	bool active[GT_COUNT] = {};    // ran in the last half second
	float ms[GT_COUNT] = {};       // smoothed GPU time per frame while it runs
	float totalMs = 0;             // smoothed sum of them all
	float frameMs = 0;             // smoothed time between Presents (CPU clock), for scale
};
GpuTimes GpuTimer_Get();

} // namespace core
