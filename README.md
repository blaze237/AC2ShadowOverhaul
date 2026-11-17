# AC2 Shadow Overhaul

Better sun shadows for **Assassin's Creed II** (the UPlay / Ubisoft Connect `AssassinsCreedIIGame.exe`).

Assassin's Creed II cuts its shadows off a short distance from the camera. Parts of buildings only
start casting shadows once you get close, which shows up as a line sweeping across the city as
shadows pop in. Push the shadows out to fix that and you hit two more problems: the shadows turn
blocky, and past a point the game crashes. AC2 Shadow Overhaul fixes all three.

## Features

- **No more shadow pop-in line.** Pushes out the distance at which buildings and smaller objects
  cast shadows, along with the sun shadow cascades and their fade-out. Every part has its own
  multiplier.
- **Shadows out to the horizon.** The sun cascades can reach up to 20x the vanilla distance.
- **Soft shadows.** The game blends shadow edges across a single shadow-map texel, which looks
  blocky once the shadows are stretched. This widens that blend to 2-4 texels (up to 25 samples)
  by rewriting the game's shadow shaders as they load. The cascades stay seam-free.
- **Crash fix.** The game keeps shadow casters in fixed-size lists with no bounds check, so long
  shadow distances crashed it. The lists are enlarged, and anything that still doesn't fit is
  skipped instead of corrupting memory.
- **Live tuning.** An optional in-game menu (top right) changes every setting on the fly. Number
  keys select a setting and `+` / `-` change it; changes are saved to the ini.
- **Before/after hotkeys.** F7 switches the distance tweaks between your settings and vanilla; F6
  switches soft shadows on and off.
- **Safe by design.** Every patch is found by byte signature and is skipped if the exe doesn't
  match. Shaders it doesn't recognise are left untouched. If the game crashes,
  `AC2ShadowOverhaul.log` records where.

## Install

1. You need an ASI loader (Ultimate ASI Loader as `dinput8.dll`, as used by EaglePatch+).
2. Copy `scripts\AC2ShadowOverhaul.asi` and `scripts\AC2ShadowOverhaul.ini` into the game's
   `scripts` folder.
3. If you used this mod under an older name (`AC2Graphics` or `AC2Shadows`), delete the old `.asi`
   and `.ini`. The new one stays inactive while an old copy is present, and says so in its log.
4. It works alongside EaglePatch+ but doesn't depend on it.

## Presets

Copy a row's values into `AC2ShadowOverhaul.ini`. The shipped values are **Balanced**.

| Preset | Distance | Splits | Casters | Detail | Width | Good for |
|---|---|---|---|---|---|---|
| Light | 2.0 | 1.0 | 2.0 | 1.0 | 2 | fixing the pop-in line at almost no cost |
| **Balanced** | 3.0 | 1.5 | 4.0 | 2.0 | 3 | everyday play: no pop-in, soft but tidy |
| Crisp | 4.0 | 1.0 | 4.0 | 2.0 | 2 | sharp shadows near you, long far shadows |
| Long View | 6.0 | 6.0 | 10.0 | 10.0 | 4 | rooftops: shadows to the horizon, very soft |
| Max | 10.0 | 4.0 | 20.0 | 15.0 | 4 | screenshots; heavy on the CPU |

Distance = `ShadowDistanceScale`, Splits = `ShadowCascadeSplitScale`, Casters =
`ShadowCasterDistanceScale`, Detail = `DetailDrawDistanceScale`, Width = `[SoftShadows] FilterWidth`.

- For every preset except Light, set `ImproveShadowMapResolution=1` in EaglePatch+'s ini. It gives
  the shadow map 4x the resolution, which the larger distances need to stay sharp.
- With Max, check the log for "Render lists full". If it appears, some distant shadows are being
  skipped; lower Casters/Detail a little.

## Hotkeys

All configurable in the ini.

| Key | Action |
|---|---|
| F7 | distance tweaks: your settings / vanilla |
| F6 | soft shadows: on / the game's filter |
| F9 | show/hide the tuning menu (needs `[DebugMenu] Enabled=1`); 1-6 select, `+`/`-` change |
| F8 | frame dump (diagnostic, `[Debug] FrameDump=1`) |

## AC2Depth.addon32 (optional, for ReShade with add-on support)

A separate companion add-on, not needed for the shadow mod.

- Gives ReShade the game's own scene depth (includes the player; no flickering between depth
  buffers) and renders ReShade effects after the game's post-processing but before the HUD, so
  ambient occlusion and other effects don't draw over the HUD.
- Install: put `AC2Depth.addon32` next to the game exe (the same folder as ReShade's `d3d9.dll`).
- In the ReShade overlay, Add-ons tab: untick "Generic Depth" (this add-on replaces it).
- Depth settings (ReShade "Edit global preprocessor definitions"):
  ```
  RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=0
  RESHADE_DEPTH_INPUT_IS_REVERSED=1      (the add-on stores reversed depth, see below)
  RESHADE_DEPTH_INPUT_IS_LOGARITHMIC=0
  RESHADE_DEPTH_LINEARIZATION_FAR_PLANE  (tune with DisplayDepth.fx, start at 1000)
  ```
  The game keeps depth in a 32-bit float texture, which many GPUs can't filter in D3D9 (ReShade
  then reads it as black). The add-on copies it every frame into a 16-bit float texture as
  `1 - depth`, which every GPU can filter and which keeps distant depth precise.
- **RenoDX HDR:** with RenoDX's HDR output on, the image before the HUD is not yet in the HDR
  format ReShade's effects expect, and comes out washed out. Set this in `ReShade.ini` (the
  add-on adds it on first run) to render at the end of the frame instead; depth still works, but
  effects then also apply under the HUD:
  ```
  [AC2Depth]
  RenderBeforeHUD=0
  ```
  The setting is re-read when you press Reload in the ReShade overlay.
- `ReShade.log` shows, when it is working: `AC2 Depth: using scene depth texture ...`,
  `AC2 Depth: created ... reversed depth copy.` and (only with `RenderBeforeHUD=1`)
  `AC2 Depth: rendering effects before the HUD ...`. It also logs the GPU, whether it can filter
  float textures, and the back buffer format. Without HUD (menus, loading) ReShade renders at the
  end of the frame as usual.

## Building

A 32-bit build is needed. The project uses [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)
(any release with an `i686-w64-mingw32` target):

```sh
./build.sh /path/to/llvm-mingw/bin     # or set LLVM_MINGW, or put the toolchain on PATH
```

This writes `dist/scripts/AC2ShadowOverhaul.asi` (+ ini) and `dist/AC2Depth.addon32`.

## Tests

`tests/run_tests.sh [path-to-llvm-mingw/bin]` builds the mod and runs the offline tests, which need
neither the game nor a GPU: an end-to-end test with a fake `d3d9.dll` (shader rewrite, hotkeys, menu,
ini saving) and, if you have exported shaders, a test that runs the soft shadow rewriter over real
game shaders. See the comments at the top of each file in `tests/` for the others.

`re/` holds the small scripts used to find the hook sites in the exe. They work on a disassembly you
generate yourself from your own copy of the game; none of the game's code or data is in this repo.

## How it works

- Shadow settings are applied with signature-scanned hooks on the cascade builder and the
  visibility-distance setter; the render list capacity patch and its guards are in `src/shadows.cpp`.
- Soft shadows (`src/softshadows.cpp`) rewrite the SM3 pixel shaders that sample the shadow atlas,
  replacing the game's 4-tap bilinear filter with an N x N box filter clamped to each cascade's tile.
- `src/framedump.cpp` hooks D3D9 (through `Direct3DCreate9`, so it works under ReShade) to rewrite
  shaders as they are created and to produce the F8 frame dump.
- `src/crashlog.cpp` logs the faulting address, stack and address-space use when the game crashes.

## Disclaimer

Not affiliated with or endorsed by Ubisoft. Assassin's Creed is a trademark of Ubisoft. This
repository contains no game code or assets; you need your own copy of the game.
