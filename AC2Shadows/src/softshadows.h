// Soft sun shadows: rewrites the game's shadow-receiving pixel shaders at load time.
#pragma once

#include <windows.h>
#include <stdint.h>
#include <string>
#include <vector>

// Reads [SoftShadows] from the ini and starts the toggle hotkey. Returns true when enabled.
bool SoftShadows_Init();
bool SoftShadows_Enabled(); // configured on (shaders get rewritten at creation)
bool SoftShadows_Active();  // currently toggled on (rewritten shaders are used)

// Rewrites ps_3_0 bytecode that does AC2's 4-tap shadow filter so it uses a wider filter of
// `width` texels (1 = same footprint as the game). Returns false (with a reason) if the shader
// doesn't sample the shadow map or doesn't have the expected structure; `out` is then untouched.
bool SoftShadows_Rewrite(const DWORD* code, int width, std::vector<DWORD>& out, std::string& reason);
int SoftShadows_Width();         // configured FilterWidth (the variant handed to the game)
int SoftShadows_LiveWidth();     // width currently selected (debug menu); shaders switch on the fly
void SoftShadows_SetLiveWidth(int width);
void SoftShadows_SetActive(bool on);
void SoftShadows_SetBuildAllWidths(bool all); // create every width variant up front (debug menu)
bool SoftShadows_BuildAllWidths();

// Where the rewrite put things, for tests.
struct SoftShadowsLayout
{
	size_t insertAt = 0;  // token index (original code) of the first shadow tap
	size_t outputAt = 0;  // token index (original code) of the filter's final lrp
	int tempBase = 0;     // first temp register used by the inserted filter (result in tempBase+9 .x)
	int outReg = 0, outComp = 0; // where the game's filter result lands
	int sampler = 0, sizeConst = 0;
	int uvReg = 0, uvComp[2] = {}, depthReg = 0, depthType = 0, depthComp = 0;
};
extern SoftShadowsLayout g_softShadowsLastLayout;
