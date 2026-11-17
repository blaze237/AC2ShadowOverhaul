AC2Graphics - graphics tweaks for Assassin's Creed II (UPlay / Ubisoft Connect exe)

Features
- Configurable shadow distance: pushes the sun shadow cascades, their fade-out, and the distance
  at which smaller objects / building pieces stop casting shadows further from the camera,
  removing the shadow pop-in line that sweeps across buildings.
- Configurable draw distance for small detail objects (which also caps their shadows).
- Fixes a crash with large shadow distances: the game's fixed-size shadow caster lists are
  enlarged, and any overflow is now skipped (and noted in the log) instead of corrupting memory.
- Crash logging: if the game crashes, AC2Graphics.log records where (for bug reports).
- Hotkey (F7) to switch the shadow tweaks between your settings and vanilla for comparisons.
- Soft shadows: widens the game's 1-texel shadow edge blend (FilterWidth, 1-4 texels) by rewriting
  the shadow-receiving pixel shaders as they load. F6 switches between soft and the game's filter.
- Debug menu ([DebugMenu] Enabled=1): live tuning of all the shadow settings in game; number keys
  select, Up/Down change, values drawn top right and logged with their ini keys.
- Frame dump hotkey (diagnostic): writes a text description of how one frame is rendered, plus
  the disassembly and bytecode of every pixel shader that samples the sun shadow map.

Install
1. Needs an ASI loader (Ultimate ASI Loader as dinput8.dll, as used by EaglePatch+).
2. Copy scripts\AC2Graphics.asi and scripts\AC2Graphics.ini into the game's scripts folder.
3. Settings are in AC2Graphics.ini. Works alongside EaglePatch+; use EaglePatch+'s
   ImproveShadowMapResolution=1 for sharper shadows.

Patches are found by byte signature; if the exe doesn't match, that patch is skipped and noted
in AC2Graphics.log.

AC2Depth.addon32 (optional, for ReShade with add-on support)
- Gives ReShade the game's own scene depth (includes the player; no flickering between depth
  buffers) and renders ReShade effects after the game's post-processing but before the HUD, so
  ambient occlusion and other effects don't draw over the HUD.
- Install: put AC2Depth.addon32 next to the game exe (the same folder as ReShade's d3d9.dll).
- In the ReShade overlay, Add-ons tab: untick "Generic Depth" (this add-on replaces it).
- Depth settings (ReShade "Edit global preprocessor definitions"):
    RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=0
    RESHADE_DEPTH_INPUT_IS_REVERSED=0
    RESHADE_DEPTH_INPUT_IS_LOGARITHMIC=0
    RESHADE_DEPTH_LINEARIZATION_FAR_PLANE - tune with DisplayDepth.fx (start at 1000)
- ReShade.log shows "AC2 Depth: using scene depth texture" and "rendering effects before the HUD"
  when it is working. Without HUD (menus, loading) ReShade renders at the end of the frame as usual.
- Uses the ReShade add-on API headers (BSD-3-Clause, see ReShade-API-LICENSE.md).
