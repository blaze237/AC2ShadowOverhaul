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
- **Ambient occlusion.** Darkens the game's ambient and bounce light in corners, creases, under
  ledges and around characters, worked out from the scene depth every frame (GTAO, at half
  resolution). It is applied inside the game's own lighting, so sunlight, fog, the sky and
  transparent effects are left alone - unlike a post-process AO, which darkens all of them.
  Radius and strength are in the ini and the tuning menu; F5 switches it on and off.
- **Crash fix.** The game keeps shadow casters in fixed-size lists with no bounds check, so long
  shadow distances crashed it. The lists are enlarged, and anything that still doesn't fit is
  skipped instead of corrupting memory.
- **Live tuning.** An optional in-game menu (top right) changes every setting on the fly. Number
  keys select a setting and `+` / `-` change it; changes are saved to the ini.
- **Before/after hotkeys.** F7 switches the distance tweaks between your settings and vanilla; F6
  switches soft shadows on and off; F5 switches ambient occlusion on and off.
- **Safe by design.** Every patch is found by byte signature and is skipped if the exe doesn't
  match. Shaders it doesn't recognise are left untouched. If the game crashes,
  `AC2ShadowOverhaul.log` records where (logging is off by default: set `[Debug] EnableLogging=1`
  when reporting a problem).

## Install

1. You need an ASI loader (Ultimate ASI Loader as `dinput8.dll`, as used by EaglePatch+).
2. Copy `scripts\AC2ShadowOverhaul.asi` and `scripts\AC2ShadowOverhaul.ini` into the game's
   `scripts` folder.
3. If you used this mod under an older name (`AC2Graphics` or `AC2Shadows`), delete the old `.asi`
   and `.ini`. The new one stays inactive while an old copy is present, and says so in its log.
4. It works alongside EaglePatch+ but doesn't depend on it. EaglePatch+ is strongly recommended
   for the shadow resolution boost below.

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

## Hotkeys

All configurable in the ini.

| Key | Action |
|---|---|
| F7 | distance tweaks: your settings / vanilla |
| F6 | soft shadows: on / the game's filter |
| F5 | ambient occlusion: on / off |
| F9 | show/hide the tuning menu (needs `[DebugMenu] Enabled=1`); 1-9 select, `+`/`-` change |
| F8 | frame dump (diagnostic, `[Debug] FrameDump=1`) |

## Building

A 32-bit build is needed. The project uses [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)
(any release with an `i686-w64-mingw32` target):

```sh
./build.sh /path/to/llvm-mingw/bin     # or set LLVM_MINGW, or put the toolchain on PATH
```

This writes `dist/scripts/AC2ShadowOverhaul.asi` (+ ini).

## Tests

`tests/run_tests.sh [path-to-llvm-mingw/bin]` builds the mod and runs the offline tests, which need
neither the game nor a GPU: an end-to-end test with a fake `d3d9.dll` (shader rewrite, hotkeys, menu,
ini saving, the AO passes), the AO shaders run on Windows' software renderer (WARP) over a synthetic
scene, and, if you have exported shaders, tests that run the soft shadow and AO rewriters over real
game shaders. After editing `src/shaders/ao.hlsl`, regenerate `src/shaders_gen.h` with
`tools/gen_shaders.c` (instructions at its top). See the comments at the top of each file in `tests/` for the others.

`re/` holds the small scripts used to find the hook sites in the exe. They work on a disassembly you
generate yourself from your own copy of the game; none of the game's code or data is in this repo.

## How it works

- Shadow settings are applied with signature-scanned hooks on the cascade builder and the
  visibility-distance setter; the render list capacity patch and its guards are in `src/shadows.cpp`.
- Soft shadows (`src/softshadows.cpp`) rewrite the SM3 pixel shaders that sample the shadow atlas,
  replacing the game's 4-tap bilinear filter with an N x N box filter clamped to each cascade's tile.
- Ambient occlusion (`src/aoshader.cpp`, `src/ao.cpp`, `src/shaders/ao.hlsl`): each shader of the
  main opaque pass gets an extra output holding its indirect light (traced through the shader from
  the ambient cube and the world lightmap to where the sun term is added). After the opaque pass,
  GTAO is computed from the game's own z/w target, blurred, and the occluded share of that indirect
  light is subtracted from the scene, before the sky and transparents are drawn.
- `src/framedump.cpp` hooks D3D9 (through `Direct3DCreate9`, so it works under ReShade) to rewrite
  shaders as they are created and to produce the F8 frame dump.
- `src/crashlog.cpp` logs the faulting address, stack and address-space use when the game crashes.

## Disclaimer

Not affiliated with or endorsed by Ubisoft. Assassin's Creed is a trademark of Ubisoft. This
repository contains no game code or assets; you need your own copy of the game.
