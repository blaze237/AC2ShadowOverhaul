#!/bin/sh
# All offline tests (no game or Direct3D 9 GPU needed). Usage: tests/run_tests.sh [path-to-llvm-mingw/bin]
# Builds dist/scripts/AC2ShadowOverhaul.asi once (build.sh), then runs every suite and exits non-zero
# if any check failed:
#
#  Shared core (tests/core/)
#   - the camera maths (bit for bit what the effects computed before they shared it) and the
#     reprojection helpers; the constant table reader on our shaders and the game's (*); the menu
#     engine's keys, exclusive opening and saving
#  Shadows, soft shadows, ambient occlusion, contact shadows, frame dump (tests/)
#   - end to end (fdtest/): the .asi in a fake AssassinsCreedIIGame.exe over the mock d3d9.dll; the
#     shader rewrite, the F9 menu (frame dump, on/off switches, soft shadow width), ini saving
#   - the AO shaders on WARP (D3D11's software renderer, part of Windows) over a synthetic scene
#   - soft shadow and AO output rewrites on the game's real pixel shaders (*)
#  SSGI (tests/ssgi/)
#   - the SSGI passes on WARP; the rewrite on the game's shaders (*), also stacked on the soft
#     shadow and AO rewrites, and merged with the AO outputs as the plugin does it (oC2 = the baked
#     indirect light only, the bounce light still in the colour)
#   - end to end: SSGI alone, then with every effect on
#  Volumetric fog and sun shafts (tests/vol/)
#   - end to end: placement 0, placement 1 and fog off alone, then placement 0 and 1 with every
#     effect on; the shaders on WARP against a CPU reference
#  TAA (tests/taa/)
#   - the vertex shader jitter rewrite (synthetic shaders, and the game's own (*)); the TAA shaders on
#     WARP; the menu's keys and ini saving
#   - end to end: TAA alone, then with every effect on
#  Everything at once (combined_test.cpp, combined/)
#   - the frame order of all effects, one menu open at a time, MSAA, refusal next to an old plugin
#
# (*) needs the game's shaders exported by the F8 frame dump: pixel shaders (*.cso) in
#     ../re/shaders/dump (or set SHADERS=dir), vertex shaders (vs_*.cso) in ../re/shaders/vs (or set
#     VS_SHADERS=dir). Skipped if absent.
#
# The end-to-end tests share one mock d3d9.dll (mockd3d/), each in a folder of its own with the
# built .asi in scripts\ and an ini the test writes. A freshly linked AssassinsCreedIIGame.exe can
# crash once in Windows' app-compat shim (apphelp) on its first run: a test that crashes is run once
# more before it counts as failed.
cd "$(dirname "$0")" || exit 1
TC="${1:-$LLVM_MINGW}"
[ -z "$TC" ] && [ -d ../../re/tools/llvm-mingw-20261104-msvcrt-x86_64/bin ] && TC=../../re/tools/llvm-mingw-20261104-msvcrt-x86_64/bin
[ -z "$TC" ] && [ -d ../re/tools/llvm-mingw-20261104-msvcrt-x86_64/bin ] && TC=../re/tools/llvm-mingw-20261104-msvcrt-x86_64/bin
[ -n "$TC" ] && TC="$(cd "$TC" && pwd)"
TCARG="$TC"
[ -n "$TC" ] && TC="$TC/"
CXX="${TC}i686-w64-mingw32-clang++"
OLDFLAGS="-O2 -I../src -fuse-ld=lld -static"
FLAGS="-O2 -std=c++17 -Wall -Wno-unused-function -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -fms-extensions -fasm-blocks -I. -I../src -fuse-ld=lld -static"
# The feature suites build from their own folders.
SUB="-O2 -std=c++17 -Wall -Wno-unused-function -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -fms-extensions -fasm-blocks -I.. -I../../src -fuse-ld=lld -static"
TESTS="$(pwd)"
# The shared core, for tests that link an effect's code (paths from a suite's folder).
CORE="../../src/core/shaderinfo.cpp ../../src/core/camera.cpp ../../src/core/scene.cpp ../../src/core/menu.cpp ../../src/core/fxpass.cpp ../../src/core/geoprep.cpp"
OUT="$TESTS/run_tests.out"

# A Windows path for a folder (the tests are Windows programs), or "" if it doesn't exist.
winpath() { (cd "$1" 2>/dev/null && (pwd -W 2>/dev/null || pwd)) ; }
SHADERS="$(winpath "${SHADERS:-../re/shaders/dump}")"
VS_SHADERS="$(winpath "${VS_SHADERS:-../re/shaders/vs}")"

FAILED=""
PASSED=0
fail() {
	FAILED="$FAILED
  - $1"
}

# run NAME DIR [FILTER] -- COMMAND...: runs COMMAND in DIR, shows its output (through the sed script
# FILTER if given), and records a failure for a non-zero exit. Crashes (exit >= 128) are rerun once.
run() {
	name="$1"
	dir="$2"
	shift 2
	filter=""
	if [ "$1" != "--" ]; then
		filter="$1"
		shift
	fi
	shift
	echo "== $name"
	(cd "$dir" && "$@") >"$OUT" 2>&1
	st=$?
	if [ $st -ge 128 ]; then
		echo "   (crashed with status $st; running it once more)"
		(cd "$dir" && "$@") >"$OUT" 2>&1
		st=$?
	fi
	if [ -n "$filter" ]; then sed -n "$filter" "$OUT"; else cat "$OUT"; fi
	if [ $st -ne 0 ]; then
		fail "$name (exit status $st)"
	else
		PASSED=$((PASSED + 1))
	fi
	return $st
}

# build NAME COMMAND...: a compile step; a failure is recorded like a failed test.
build() {
	name="$1"
	shift
	if ! "$@"; then
		fail "$name: build failed"
		return 1
	fi
}

# e2e_dir DIR: a fresh end-to-end folder with the mock d3d9.dll and the built .asi.
e2e_dir() {
	rm -rf "$1"
	mkdir -p "$1/scripts"
	cp mockd3d/d3d9.dll "$1/"
	cp ../dist/scripts/AC2ShadowOverhaul.asi "$1/scripts/"
}

echo "== building dist/scripts/AC2ShadowOverhaul.asi"
if ! (cd .. && sh build.sh "$TCARG" >/dev/null); then
	echo "build.sh failed"
	exit 1
fi
build "mock d3d9.dll" $CXX -O2 -w -fuse-ld=lld -static -shared -Wl,--kill-at -o mockd3d/d3d9.dll mockd3d/mock_d3d9.cpp || exit 1

# ---- Shared core -----------------------------------------------------------------------------------

cd core
build "camera test" $CXX $SUB -o camera_test.exe camera_test.cpp ../../src/core/camera.cpp &&
	run "core: camera / reprojection maths" . -- ./camera_test.exe
build "constant table test" $CXX $SUB -o shaderinfo_test.exe shaderinfo_test.cpp ../../src/core/shaderinfo.cpp &&
	run "core: constant tables (our shaders and the game's)" . -- ./shaderinfo_test.exe "$SHADERS" "$VS_SHADERS"
build "menu engine test" $CXX $SUB -o menu_test.exe menu_test.cpp ../../src/core/menu.cpp ../../src/common.cpp &&
	run "core: menu engine (keys, exclusive opening, saving)" . -- ./menu_test.exe
cd ..

# ---- Shadows, soft shadows, AO, contact shadows, frame dump ---------------------------------------

mkdir -p fdtest/scripts
rm -rf fdtest/scripts/*.log fdtest/scripts/AC2ShadowOverhaul_FrameDump_*
cp ../dist/scripts/AC2ShadowOverhaul.asi fdtest/scripts/
cp mockd3d/d3d9.dll fdtest/
if build "shadow end to end" $CXX $OLDFLAGS -o fdtest/AssassinsCreedIIGame.exe framedump_test.cpp -ld3d9; then
	# It prints yes / NO per check and always exits 0.
	if run "shadows: end to end (mock d3d9)" fdtest -- ./AssassinsCreedIIGame.exe && grep -qwE "NO|NOT" "$OUT"; then
		PASSED=$((PASSED - 1))
		fail "shadows: end to end (a check printed NO)"
	fi
fi
if build "AO WARP test" $CXX $OLDFLAGS -o ao_warp_test.exe ao_warp_test.cpp -ld3d11; then
	run "shadows: AO shaders on WARP" . -- ./ao_warp_test.exe
	run "shadows: AO shaders on WARP (pitched camera)" . -- ./ao_warp_test.exe pitched
fi
if [ -n "$SHADERS" ]; then
	build "soft shadow shader test" $CXX $OLDFLAGS -o softshadow_test.exe softshadow_test.cpp &&
		run "shadows: soft shadow rewrite on the game's shaders" . '$p' -- ./softshadow_test.exe "$SHADERS"
	build "AO output shader test" $CXX $OLDFLAGS -o aoshader_test.exe aoshader_test.cpp &&
		run "shadows: AO output rewrite on the game's shaders" . '$p' -- ./aoshader_test.exe "$SHADERS"
else
	echo "== shadows: soft shadow / AO output shader tests skipped (no exported shaders in ../re/shaders/dump)"
fi

# ---- SSGI ------------------------------------------------------------------------------------------

cd ssgi
build "SSGI WARP test" $CXX $SUB -o ssgi_warp_test.exe ssgi_warp_test.cpp ../../src/core/camera.cpp -ld3d11 &&
	run "SSGI: passes on WARP" . -- ./ssgi_warp_test.exe
if [ -n "$SHADERS" ]; then
	build "gishader test" $CXX $SUB -Wno-unused -DWITH_OVERHAUL -o gishader_test.exe gishader_test.cpp ../../src/litshader.cpp \
		../../src/softshadows.cpp ../../src/common.cpp ../../src/core/shaderinfo.cpp &&
		run "SSGI: rewrite on the game's shaders (alone, stacked, and merged with the AO outputs)" . '$p' -- ./gishader_test.exe "$SHADERS"
else
	echo "== SSGI: shader rewrite test skipped (no exported shaders in ../re/shaders/dump)"
fi
if build "SSGI end to end" $CXX $SUB -o e2e_test.exe e2e_test.cpp ../../src/core/camera.cpp -ld3d9; then
	(cd .. && e2e_dir ssgi/e2e)
	mv e2e_test.exe e2e/AssassinsCreedIIGame.exe
	run "SSGI: end to end, alone" e2e -- ./AssassinsCreedIIGame.exe
	run "SSGI: end to end, every effect on" e2e -- ./AssassinsCreedIIGame.exe all
fi
cd ..

# ---- Volumetric fog and sun shafts -----------------------------------------------------------------

cd vol
if build "volumetrics end to end" $CXX $SUB -o e2e_test.exe e2e_test.cpp -ld3d9; then
	(cd .. && e2e_dir vol/e2e)
	mv e2e_test.exe e2e/AssassinsCreedIIGame.exe
	run "volumetrics: end to end, placement 0, alone" e2e -- ./AssassinsCreedIIGame.exe
	run "volumetrics: end to end, placement 1, alone" e2e -- ./AssassinsCreedIIGame.exe placement1
	run "volumetrics: end to end, fog off, alone" e2e -- ./AssassinsCreedIIGame.exe fogoff
	run "volumetrics: end to end, placement 0, every effect on" e2e -- ./AssassinsCreedIIGame.exe all
	run "volumetrics: end to end, placement 1, every effect on" e2e -- ./AssassinsCreedIIGame.exe all placement1
fi
build "volumetrics WARP test" $CXX $SUB -o warp_test.exe warp_test.cpp -ld3d11 &&
	run "volumetrics: shaders on WARP" . -- ./warp_test.exe
cd ..

# ---- TAA -------------------------------------------------------------------------------------------

cd taa
if build "vsjitter test" $CXX $SUB -o vsjitter_test.exe vsjitter_test.cpp ../../src/taa/vsjitter.cpp ../../src/core/shaderinfo.cpp -ld3d9; then
	if [ -n "$VS_SHADERS" ]; then
		run "TAA: vertex shader jitter rewrite (synthetic and the game's shaders)" . '/^    \|rewritten$/!p' -- ./vsjitter_test.exe "$VS_SHADERS"
	else
		run "TAA: vertex shader jitter rewrite (synthetic shaders; no exported vertex shaders in ../re/shaders/vs)" . \
			'/^    \|rewritten$/!p' -- ./vsjitter_test.exe ./no-shaders
	fi
fi
build "TAA WARP test" $CXX $SUB -o taa_warp_test.exe taa_warp_test.cpp ../../src/core/camera.cpp -ld3d11 &&
	run "TAA: shaders on WARP" . -- ./taa_warp_test.exe
build "TAA menu test" $CXX $SUB -o menu_test.exe menu_test.cpp ../../src/taa/taa.cpp ../../src/common.cpp ../../src/taa/vsjitter.cpp \
	$CORE &&
	run "TAA: menu (input and saving)" . -- ./menu_test.exe
if build "TAA end to end" $CXX $SUB -o e2e_test.exe e2e_test.cpp -ld3d9; then
	(cd .. && e2e_dir taa/e2e)
	mv e2e_test.exe e2e/AssassinsCreedIIGame.exe
	run "TAA: end to end, alone" e2e -- ./AssassinsCreedIIGame.exe
	run "TAA: end to end, every effect on" e2e -- ./AssassinsCreedIIGame.exe all
fi
cd ..

# ---- Everything at once ----------------------------------------------------------------------------

if build "combined end to end" $CXX $FLAGS -o combined_test.exe combined_test.cpp -ld3d9; then
	e2e_dir combined
	mv combined_test.exe combined/AssassinsCreedIIGame.exe
	for mode in order menus msaa refusal; do
		run "combined: $mode" combined -- ./AssassinsCreedIIGame.exe $mode
	done
fi

rm -f "$OUT"
echo
if [ -n "$FAILED" ]; then
	echo "FAILED:$FAILED"
	echo "($PASSED test runs passed)"
	exit 1
fi
echo "ALL TESTS PASSED ($PASSED test runs)"
exit 0
