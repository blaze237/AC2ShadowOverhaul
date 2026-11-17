AC2Shadows - better sun shadows for Assassin's Creed II (UPlay / Ubisoft Connect exe)

Assassin's Creed II cuts its shadows off a short distance from the camera. Parts of buildings
only start casting shadows once you get close, which shows up as a line sweeping across the city
as shadows pop in. Push the shadows out to fix that and you hit two more problems: the shadows
turn blocky, and past a point the game crashes. AC2Shadows fixes all three.

Key features
- No more shadow pop-in line: pushes out the distance at which buildings and smaller objects
  cast shadows, along with the sun shadow cascades and their fade-out. Every part has its own
  multiplier.
- Shadows out to the horizon: the sun cascades can reach up to 20x the vanilla distance.
- Soft shadows: the game blends shadow edges across a single shadow-map texel, which looks
  blocky once the shadows are stretched. AC2Shadows widens that blend to 2-4 texels (up to 25
  samples) by rewriting the game's shadow shaders as they load. The cascades stay seam-free.
- Crash fix: the game keeps shadow casters in fixed-size lists with no bounds check, so long
  shadow distances crashed it. The lists are enlarged, and anything that still doesn't fit is
  skipped instead of corrupting memory.
- Live tuning: an optional in-game menu (top right) changes every setting on the fly. Number
  keys select a setting and the arrow keys change it; changes are saved to the ini.
- Before/after hotkeys: F7 switches the distance tweaks between your settings and vanilla; F6
  switches soft shadows on and off.
- Safe by design: every patch is found by byte signature and is skipped if the exe doesn't
  match. Shaders it doesn't recognise are left untouched. If the game crashes, AC2Shadows.log
  records where.

Presets (AC2Shadows.ini; the shipped values are Balanced)
  Preset      Distance  Splits  Casters  Detail  Width   Good for
  Light          2.0     1.0      2.0     1.0      2     fixing the pop-in line at almost no cost
  Balanced       3.0     1.5      4.0     2.0      3     everyday play: no pop-in, soft but tidy
  Crisp          4.0     1.0      4.0     2.0      2     sharp shadows near you, long far shadows
  Long View      6.0     6.0     10.0    10.0      4     rooftops: shadows to the horizon, very soft
  Max           10.0     4.0     20.0    15.0      4     screenshots; heavy on the CPU
  (Distance = ShadowDistanceScale, Splits = ShadowCascadeSplitScale, Casters =
   ShadowCasterDistanceScale, Detail = DetailDrawDistanceScale, Width = [SoftShadows] FilterWidth)
- For every preset except Light, set ImproveShadowMapResolution=1 in EaglePatch+'s ini. It
  gives the shadow map 4x the resolution, which the larger distances need to stay sharp.
- With Max, check AC2Shadows.log for "Render lists full". If it appears, some distant shadows
  are being skipped; lower Casters/Detail a little.

Install
1. Needs an ASI loader (Ultimate ASI Loader as dinput8.dll, as used by EaglePatch+).
2. Copy scripts\AC2Shadows.asi and scripts\AC2Shadows.ini into the game's scripts folder.
3. Upgrading from AC2Graphics: delete AC2Graphics.asi and AC2Graphics.ini (AC2Shadows stays
   inactive while the old one is present, and says so in its log).
4. Works alongside EaglePatch+.

Hotkeys (all configurable in the ini)
  F7  distance tweaks: your settings / vanilla      F6  soft shadows: on / game filter
  F9  show/hide the tuning menu ([DebugMenu] Enabled=1); 1-7 select, Up/Down change
  F8  frame dump (diagnostic, [Debug] FrameDump=1)

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
