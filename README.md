# AC2 Shadow Overhaul

A graphics overhaul for **Assassin's Creed II** (the UPlay / Ubisoft Connect `AssassinsCreedIIGame.exe`):
longer and softer sun shadows, ambient occlusion and contact shadows, bounce light (SSGI),
volumetric fog with god rays and sun shafts, and temporal anti-aliasing. All of it is one plugin,
`AC2ShadowOverhaul.asi`, and every effect works inside the game's own renderer rather than as a
post-process filter on top of it.

Assassin's Creed II cuts its shadows off a short distance from the camera. Parts of buildings only
start casting shadows once you get close, which shows up as a line sweeping across the city as
shadows pop in. Push the shadows out to fix that and you hit two more problems: the shadows turn
blocky, and past a point the game crashes. AC2 Shadow Overhaul fixes all three, then adds the
lighting effects the 2009 renderer never had.

## Features

### Shadows

- **No more shadow pop-in line.** Pushes out the distance at which buildings and smaller objects
  cast shadows, along with the sun shadow cascades and their fade-out. Every part has its own
  multiplier.
- **Shadows out to the horizon.** The sun cascades can reach up to 20x the vanilla distance.
- **Soft shadows.** The game blends shadow edges across a single shadow-map texel, which looks
  blocky once the shadows are stretched. This widens that blend to 2-4 texels (up to 25 samples)
  by rewriting the game's shadow shaders as they load. The cascades stay seam-free.
  Optional contact hardening (`[SoftShadows] ContactHardening`, off by default) varies that width
  with the distance to each shadow's caster: sharp where an object meets the ground, softer
  further away. The width is worked out in metres, so it matches across cascade boundaries.
- **Crash fix.** The game keeps shadow casters in fixed-size lists with no bounds check, so long
  shadow distances crashed it. The lists are enlarged, and anything that still doesn't fit is
  skipped instead of corrupting memory.

### Lighting

- **Ambient occlusion.** Darkens the game's ambient light in corners, creases, under ledges and
  around characters, worked out from the scene depth every frame (GTAO, at half resolution). It is
  applied inside the game's own lighting, so sunlight, fog, the sky and transparent effects are left
  alone - unlike a post-process AO, which darkens all of them.
- **Contact shadows.** Short shadows traced through the scene depth towards the sun, for the small
  detail the shadow map is too coarse for: where feet, crates and ledges meet the ground, folds in
  clothing, window frames. Only sunlight is darkened.
- **Surface normals.** Ambient occlusion, contact shadows and SSGI read each surface's own normal
  as the game lights it, normal map detail included, which the game's shaders now pass along with
  their lighting; where a shader doesn't, or the normal doesn't fit the surface the depth shows, the
  normal worked out from the depth is used, as before. `ShadingNormals` in the F9 menu switches it
  for comparison.
- **Bounce light (SSGI).** Light bounces off lit surfaces onto nearby ones: a sunlit wall brightens
  the street in front of it, red cloth tints the floor next to it, and courtyards fill with reflected
  sunlight. The bounce is added to the game's own ambient term before fog, so it is fogged like the
  rest of the scene. Ambient occlusion darkens only the game's baked ambient, not the bounce, which
  already carries its own occlusion; and the bounce is traced from the image after ambient occlusion
  and contact shadows, so it comes from the occluded light. Light keeps bouncing over successive
  frames (each frame's bounce lights the next), held in check so it can't build up without end even
  at the highest intensity; where the light changes (someone walks past a sunlit wall) the old bounce
  goes at once instead of trailing behind.

### Atmosphere

- **Volumetric fog.** Height fog lit by the sun through the game's own shadow map, so wherever
  sunlight gets between buildings, through arches or past towers you see shafts of light in the air.
  It follows the stretched shadow cascades, and is drawn before smoke, glass and other transparent
  effects. The sky is never dimmed, so the sun and the game's bloom around it are unchanged.
  Cover overhead (roofs, vaults, arcades, tombs, narrow alleys) holds back the sky light the fog
  scatters, so enclosed spaces don't glow with haze, and slowly drifting 3D noise breaks the fog
  into wisps.
- **Sun shafts.** Light streaks from the bright sky around the sun when it is partly hidden behind
  buildings, trees or clouds.

### Anti-aliasing

- **Temporal anti-aliasing (TAA).** Smooths jagged edges and shimmer by blending each frame with
  the previous ones, at engine level: the game's geometry is jittered by a fraction of a pixel each
  frame and the result is resolved before bloom and tone mapping, so the HUD stays sharp.
- **The other effects work with it, but don't need it.** Every effect accounts for TAA's jitter when
  it reconstructs positions from depth, and while TAA runs the volumetric fog varies its sampling
  noise every frame for TAA to average out (`[TAA] AssistEffects`). Ambient occlusion, contact
  shadows and SSGI keep the same noise as without TAA: TAA can't follow moving characters (the game
  has no motion vectors), so per-frame noise on them would show through as noise and swimming. With
  TAA off each effect behaves exactly as it does on its own.

### Everything

- **Live tuning.** Four in-game menus (see below) change every setting on the fly; changes are
  saved to the ini.
- **MSAA warning.** The screen-space effects can't work while the game's own anti-aliasing (MSAA) is
  on. If it is, a red warning on screen names the effects that are switched off because of it.
- **Safe by design.** Every patch is found by byte signature and is skipped if the exe doesn't
  match. Shaders it doesn't recognise are left untouched. If the game crashes,
  `AC2ShadowOverhaul.log` records where (logging is off by default: set `[Debug] EnableLogging=1`
  when reporting a problem).

## Install

1. You need an ASI loader (Ultimate ASI Loader as `dinput8.dll`, as used by EaglePatch+).
2. Copy `scripts\AC2ShadowOverhaul.asi` and `scripts\AC2ShadowOverhaul.ini` into the game's
   `scripts` folder, replacing an older AC2 Shadow Overhaul. The shadow and AO section and key
   names are unchanged, so you can copy your old values across into the new ini.
3. **Delete the separate plugins this one now includes**, with their `.ini` files: `AC2SSGI.asi`,
   `AC2Volumetrics.asi`, `AC2TAA.asi`, and the old names of this mod, `AC2Graphics.asi` and
   `AC2Shadows.asi`. While any of them is installed AC2 Shadow Overhaul does nothing at all (to avoid
   patching the game twice) and shows a message box listing them at startup.
4. In the game's graphics options, **switch anti-aliasing off**: ambient occlusion, contact shadows,
   SSGI, the volumetrics and TAA all need it off (TAA replaces it).
5. It works alongside EaglePatch+ but doesn't depend on it. EaglePatch+ is strongly recommended
   for the shadow resolution boost below.

Every effect is on by default. Each has `Enabled` in its ini section (`0` leaves the game
completely untouched by it; needs a restart) and an on/off switch in its menu.

## Recommended: raise the shadow resolution with EaglePatch+

Stretching the shadows over a longer distance spreads the same shadow map over a larger area, so
they get blurrier and blockier. The vanilla shadow map is only 1024x1024, which isn't enough for
the larger distances. EaglePatch+ can raise it to 4096x4096, which is 4x the detail in each
direction and makes the longer distances look right.

In EaglePatch+'s ini (`EaglePatchAC2.ini`), set:

```
ImproveShadowMapResolution=1
```

This mod doesn't change the shadow map size itself; it leaves that to EaglePatch+ so the two don't
fight over the same setting. Without it, the shipped **High** preset still works, but shadows look
softer and blurrier.

## Presets

Copy a row's values into `AC2ShadowOverhaul.ini`. The shipped values are **High**.

| Preset | Distance | Splits | Casters | Detail | Width |
|---|---|---|---|---|---|
| Light | 2.0 | 1.0 | 10.0 | 10.0 | 2 |
| Balanced | 3.0 | 2.0 | 10.0 | 10.0 | 4 |
| **High** | 6.0 | 6.0 | 10.0 | 10.0 | 4 |

Distance = `ShadowDistanceScale`, Splits = `ShadowCascadeSplitScale`, Casters =
`ShadowCasterDistanceScale`, Detail = `DetailDrawDistanceScale`, Width = `[SoftShadows] FilterWidth`.

- Use EaglePatch+'s `ImproveShadowMapResolution=1` (see above) with all of these presets, so the
  larger distances stay sharp.
- If the log shows "Render lists full", some distant shadows are being skipped; lower Casters/Detail
  a little.

## In-game menus

Each group of effects has its own menu, drawn in the top-right corner. Only one is open at a time:
opening one closes the other.

| Key | Menu | Ini key (`[Menu]`) |
|---|---|---|
| F9 | Shadows & AO: shadow distances, soft shadows, ambient occlusion, contact shadows | `MenuKey` |
| F3 | SSGI (with a status line showing its GPU time) | `SSGIMenuKey` |
| F11 | Volumetrics: fog and sun shafts | `VolumetricsMenuKey` |
| F1 | TAA (with a status line saying whether it is working) | `TAAMenuKey` |

**F12** (`[Menu] AllEffectsKey`) switches every effect off and back on at once, for a quick comparison
with the vanilla game: shadows, AO, contact shadows, SSGI, the volumetrics and TAA. Turning them back on
restores whatever was on before, and it isn't saved. While everything is off a small note says so in
the top-left corner. The menu keys and F12 are the only keys the mod uses while every menu is hidden.

| Key | Action (while a menu is shown) |
|---|---|
| Up / Down | select a setting |
| 1-9, 0 | jump to the first ten settings |
| Left / Right | change the selected setting (hold to repeat) |
| the menu's key | hide the menu |

Every change applies at once and is saved to the ini a second later. The exceptions are the two
comparison switches, which aren't saved: "All shadow effects" in the F9 menu switches the shadow
tweaks, soft shadows, ambient occlusion and contact shadows off and back on for a quick comparison
with vanilla, and "All volumetric effects" in the F11 menu does the same for the fog and shafts.
Soft shadow width 0 is the game's own filter; contact hardening only applies while soft shadows are
on, and a change to its penumbra or widths rebuilds the shadow shaders a moment after the last key
press (a short hitch). With `[Debug] FrameDump=1` the last entry of the F9
menu, "Frame dump", writes a frame dump (diagnostic); `[Debug] FrameDumpKey` can also give it a
hotkey for debugging (off by default).

## Building

A 32-bit build is needed. The project uses [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)
(any release with an `i686-w64-mingw32` target):

```sh
./build.sh /path/to/llvm-mingw/bin     # or set LLVM_MINGW, or put the toolchain on PATH
```

This writes `dist/scripts/AC2ShadowOverhaul.asi` (+ ini).

## Tests

`tests/run_tests.sh [path-to-llvm-mingw/bin]` builds the mod once and runs all the offline tests,
which need neither the game nor a GPU, and exits non-zero if any check fails: end-to-end tests that
load the built .asi into a fake `AssassinsCreedIIGame.exe` over a mock `d3d9.dll` (`tests/mockd3d`)
for the shadows and AO, SSGI (`tests/ssgi`), the volumetrics (`tests/vol`) and TAA (`tests/taa`),
each alone and with every effect on, plus a combined test (`tests/combined_test.cpp`: the frame order
of all effects, one menu at a time, MSAA, refusal next to an old plugin); the effects' shaders run on
Windows' software renderer (WARP) over synthetic scenes; unit tests of the shared core (`tests/core`:
camera maths, shader constant tables, the menu engine) and of the vertex shader jitter; and, if you
have exported shaders (`re/shaders/dump`, `re/shaders/vs`), tests that run the soft shadow, AO, SSGI and
TAA rewriters over real game shaders. After editing an effect's HLSL (`src/shaders/ao.hlsl`,
`src/ssgi/shaders/ssgi.hlsl`, `src/vol/shaders/volumetrics.hlsl`, `src/taa/shaders/taa.hlsl`),
regenerate its `shaders_gen.h` with `tools/gen_shaders.c` (instructions at its top). See the comments
at the top of each file in `tests/` for the others.

`re/` holds the small scripts used to find the hook sites in the exe. They work on a disassembly you
generate yourself from your own copy of the game; none of the game's code or data is in this repo.

## How it works

- Shadow settings are applied with signature-scanned hooks on the cascade builder and the
  visibility-distance setter; the render list capacity patch and its guards are in `src/shadows.cpp`.
- Soft shadows (`src/softshadows.cpp`) rewrite the SM3 pixel shaders that sample the shadow atlas,
  replacing the game's 4-tap bilinear filter with an N x N box filter clamped to each cascade's tile.
  Contact hardening is a variant of its own: one read of the 5 x 5 texels around the sample finds
  the blockers and their mean distance above the receiver (via `g_ShadowNearFar`), the penumbra in
  metres becomes texels through the cascade's `g_ShadowScalesX`, and a box of that continuous
  width is filtered over the same texels.
- Ambient occlusion and contact shadows (`src/ao.cpp`, `src/shaders/ao.hlsl`) and SSGI (`src/ssgi`)
  share one shader rewrite (`src/litshader.cpp`): one dataflow analysis traces each opaque shader's
  baked indirect light and shadowed sun light from the ambient cube / world lightmap and the shadow
  map to where they are added together. AO gets them as two extra outputs; SSGI adds its bounce
  light to the indirect term after AO's copy is taken, so AO darkens only the game's baked light.
  After the opaque pass, GTAO and a short ray march towards the sun are computed from the game's own
  z/w target, blurred, and the occluded share of each light is subtracted from the scene, before
  SSGI traces the result and before the sky and transparents are drawn.
- `src/framedump.cpp` hooks D3D9 (through `Direct3DCreate9`, so it works under ReShade) to rewrite
  shaders as they are created and to produce the frame dump. It feeds every device call to the
  effects through `src/features.cpp` (SSGI in `src/ssgi`, volumetrics in `src/vol`, TAA in `src/taa`).
- `src/core` is what the effects share: shader constant tables (`shaderinfo`), the camera and its
  reprojection maths (`camera`), the main-pass and per-frame camera tracker (`scene`), the menu
  engine behind all four menus (`menu`), the fullscreen-pass plumbing (`fxpass`), and the half-res
  normals and depth that AO and SSGI both read, made once per frame (`geoprep`).
- Volumetrics (`src/vol`) read the camera, the sun shadow atlas's light-space matrix and cascade
  layout, the sun and the fog colour from the opaque draws' shader constants. After the sky they ray
  march the height fog at half resolution, looking each step up in the shadow atlas exactly as the
  game's shaders do, blur it, add quarter-resolution radial sun shafts, and composite as
  `scene x transmittance + scattered light + shafts`, never dimming the sky.
- TAA (`src/taa`) appends a sub-pixel jitter (`c255`) to every geometry vertex shader, resolves the
  scene against its reprojected history at the game's first read of the scene colour (after every
  other effect), and reports the jitter and history state to `src/core/scene`, which the other
  effects use for jitter-correct reconstruction and per-frame noise.
- `src/crashlog.cpp` logs the faulting address, stack and address-space use when the game crashes.

## Disclaimer

Not affiliated with or endorsed by Ubisoft. Assassin's Creed is a trademark of Ubisoft. This
repository contains no game code or assets; you need your own copy of the game.
