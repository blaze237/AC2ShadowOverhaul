// Soft sun shadows: rewrites the game's shadow-receiving pixel shaders at load time.
#pragma once

#include <windows.h>
#include <stdint.h>
#include <string>
#include <vector>

// Reads [SoftShadows] from the ini; returns true. FilterWidth=0 starts with the game's own filter
// (the menu's width 0).
bool SoftShadows_Init();
bool SoftShadows_Active();  // currently on (rewritten shaders are used)
void SoftShadows_SetActive(bool on);

// Rewrites ps_3_0 bytecode that does AC2's 4-tap shadow filter so it uses a wider filter of
// `width` texels (1 = same footprint as the game). Returns false (with a reason) if the shader
// doesn't sample the shadow map or doesn't have the expected structure; `out` is then untouched.
bool SoftShadows_Rewrite(const DWORD* code, int width, std::vector<DWORD>& out, std::string& reason);
int SoftShadows_Width();         // configured FilterWidth (the variant handed to the game)
int SoftShadows_LiveWidth();     // width currently selected (debug menu); shaders switch on the fly
void SoftShadows_SetLiveWidth(int width);
void SoftShadows_SetBuildAllWidths(bool all); // create every width variant up front (debug menu)
bool SoftShadows_BuildAllWidths();

// Contact hardening ([SoftShadows] ContactHardening): a variant of its own whose filter width
// follows the distance from the receiver to the shadow's caster (PCSS). With it off the shaders
// are the plain FilterWidth ones above, unchanged.
struct SoftShadowsPcss
{
	float penumbraScale = 0.04f; // penumbra width in metres per metre between caster and receiver
	float minWidth = 1.0f;       // filter width limits in shadow map texels (1..4)
	float maxWidth = 4.0f;
};
SoftShadowsPcss SoftShadows_ClampPcss(const SoftShadowsPcss& p); // into the ranges the shader supports
// Same as SoftShadows_Rewrite, for the contact hardening filter with these settings. Fails (with a
// reason) for every shader SoftShadows_Rewrite fails for, and for a few more (missing cascade
// constants, too few free registers or instruction slots); those keep the plain filter.
bool SoftShadows_RewritePcss(const DWORD* code, const SoftShadowsPcss& p, std::vector<DWORD>& out, std::string& reason);
bool SoftShadows_ContactHardeningAtStart(); // the ini's value (the variant handed to the game)
bool SoftShadows_ContactHardening();        // currently on (menu)
void SoftShadows_SetContactHardening(bool on);
SoftShadowsPcss SoftShadows_Pcss();         // settings as currently chosen (menu)
void SoftShadows_SetPcss(const SoftShadowsPcss& p);
// The settings to build the contact hardening variants with, and their version: 0 = as at start,
// one more each time the menu's settings have settled on new values (the variants built with an
// older version are rebuilt).
int SoftShadows_PcssBuilt(SoftShadowsPcss* p);

// Instruction slots a ps_3_0 program uses (the limit is 512), for tests and checks.
int SoftShadows_InstructionSlots(const std::vector<DWORD>& code);

// Where the rewrite put things, for tests.
struct SoftShadowsLayout
{
	size_t insertAt = 0;  // token index (original code) of the first shadow tap
	size_t outputAt = 0;  // token index (original code) of the filter's final lrp
	int tempBase = 0;     // first temp register used by the inserted filter (result in tempBase+9 .x)
	int outReg = 0, outComp = 0; // where the game's filter result lands
	int sampler = 0, sizeConst = 0;
	int uvReg = 0, uvComp[2] = {}, depthReg = 0, depthType = 0, depthComp = 0;
	bool pcss = false;    // the contact hardening filter
	int scalesX = -1, offsetsX = -1, nearFar = -1; // g_ShadowScalesX, g_ShadowOffsetsX, g_ShadowNearFar
};
extern SoftShadowsLayout g_softShadowsLastLayout;
