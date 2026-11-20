#!/bin/sh
# Offline tests (no game or D3D9 GPU needed). Usage: ./run_tests.sh [path-to-llvm-mingw/bin]
#  1. camera / reprojection maths (gimath_test);
#  2. the SSGI passes on D3D11's software renderer (WARP) over synthetic scenes (ssgi_warp_test);
#  3. the shader rewrite on the game's real pixel shaders, exported by AC2ShadowOverhaul's F8 frame
#     dump (../../re/shaders/dump, or set SHADERS=dir; skipped if absent). With AC2ShadowOverhaul's
#     sources in ../../src its rewrites are stacked with ours in both orders;
#  4. end to end: the built .asi in a fake AssassinsCreedIIGame.exe over a mock d3d9.dll, alone and
#     (if ../../dist/scripts/AC2ShadowOverhaul.asi exists) with AC2ShadowOverhaul in both load orders.
set -e
cd "$(dirname "$0")"
TC="${1:-$LLVM_MINGW}"
[ -z "$TC" ] && [ -d ../../re/tools/llvm-mingw-20261104-msvcrt-x86_64/bin ] && TC=../../re/tools/llvm-mingw-20261104-msvcrt-x86_64/bin
[ -n "$TC" ] && TC="$TC/"
CXX="${TC}i686-w64-mingw32-clang++"
FLAGS="-O2 -std=c++17 -Wall -fuse-ld=lld -static"

sh ../build.sh "${1:-}" >/dev/null

echo "== camera / reprojection maths"
$CXX $FLAGS -o gimath_test.exe gimath_test.cpp ../src/gimath.cpp
./gimath_test.exe

echo "== SSGI passes on WARP"
$CXX $FLAGS -o ssgi_warp_test.exe ssgi_warp_test.cpp ../src/gimath.cpp -ld3d11
./ssgi_warp_test.exe

SHADERS="${SHADERS:-../../re/shaders/dump}"
if [ -d "$SHADERS" ]; then
	echo "== shader rewrite on the game's shaders ($SHADERS)"
	if [ -f ../../src/aoshader.cpp ] && [ -f ../../src/softshadows.cpp ]; then
		$CXX $FLAGS -Wno-unused -DWITH_OVERHAUL -o gishader_test.exe gishader_test.cpp overhaul_glue.cpp ../src/gishader.cpp ../src/common.cpp
	else
		$CXX $FLAGS -o gishader_test.exe gishader_test.cpp ../src/gishader.cpp ../src/common.cpp
	fi
	./gishader_test.exe "$SHADERS" | tail -1
else
	echo "== shader rewrite test skipped (no exported shaders found)"
fi

echo "== end to end (mock d3d9)"
mkdir -p e2e/scripts
$CXX -O2 -w -fuse-ld=lld -static -shared -Wl,--kill-at -o e2e/d3d9.dll mockd3d/mock_d3d9.cpp
$CXX $FLAGS -o e2e/AssassinsCreedIIGame.exe e2e_test.cpp ../src/gimath.cpp -ld3d9
cp ../dist/scripts/AC2SSGI.asi ../AC2SSGI.ini e2e/scripts/
(cd e2e && rm -f scripts/*.log && ./AssassinsCreedIIGame.exe AC2SSGI.asi | tail -1)
OVERHAUL=../../dist/scripts/AC2ShadowOverhaul.asi
if [ -f "$OVERHAUL" ]; then
	cp "$OVERHAUL" e2e/scripts/
	cp ../../AC2ShadowOverhaul.ini e2e/scripts/ 2>/dev/null || true
	echo "== end to end with AC2ShadowOverhaul (it loads first, then the other way round)"
	(cd e2e && ./AssassinsCreedIIGame.exe AC2ShadowOverhaul.asi AC2SSGI.asi | tail -1)
	(cd e2e && ./AssassinsCreedIIGame.exe AC2SSGI.asi AC2ShadowOverhaul.asi | tail -1)
else
	echo "== end to end with AC2ShadowOverhaul skipped ($OVERHAUL not built)"
fi
