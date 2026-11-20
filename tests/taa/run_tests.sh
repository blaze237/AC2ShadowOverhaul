#!/bin/sh
# Offline tests (no game or Direct3D 9 GPU needed). Usage: tests/run_tests.sh [path-to-llvm-mingw/bin]
#  1. vertex shader jitter rewrite: synthetic shaders, plus the game's own if exported by
#     AC2ShadowOverhaul's F8 frame dump (vs_*.cso in ../re/shaders, or set SHADERS=dir)
#  2. the TAA shaders on WARP (Windows' software renderer) over a synthetic scene
#  3. the in-game menu's keys and ini saving, with a fake keyboard
#  4. end-to-end through the real hooks on a mock d3d9.dll; again with AC2ShadowOverhaul.asi
#     loaded alongside if it can be found (../../dist/scripts, or set SO_ASI=path)
set -e
cd "$(dirname "$0")"
TC="${1:-$LLVM_MINGW}"
[ -z "$TC" ] && [ -d ../../re/tools/llvm-mingw-20261104-msvcrt-x86_64/bin ] && TC=../../re/tools/llvm-mingw-20261104-msvcrt-x86_64/bin
[ -n "$TC" ] && TC="$(cd "$TC" && pwd)"
TCARG="$TC"
[ -n "$TC" ] && TC="$TC/"
CXX="${TC}i686-w64-mingw32-clang++"
FLAGS="-O2 -std=c++17 -Wall -Wno-unused-function -DNOMINMAX -fuse-ld=lld -static"

(cd .. && sh build.sh "$TCARG" >/dev/null)

echo "== vertex shader jitter rewrite"
$CXX $FLAGS -o vsjitter_test.exe vsjitter_test.cpp ../src/vsjitter.cpp -ld3d9
./vsjitter_test.exe "${SHADERS:-../re/shaders}" | grep -vE "^    |rewritten$"

echo "== TAA shaders on WARP"
$CXX $FLAGS -o taa_warp_test.exe taa_warp_test.cpp ../src/taa.cpp ../src/common.cpp ../src/vsjitter.cpp -ld3d11
./taa_warp_test.exe

echo "== in-game menu (input and saving)"
$CXX $FLAGS -o menu_test.exe menu_test.cpp ../src/taa.cpp ../src/common.cpp ../src/vsjitter.cpp
./menu_test.exe

echo "== end-to-end on a mock d3d9.dll"
mkdir -p e2e/scripts
rm -f e2e/scripts/*
cp ../dist/scripts/AC2TAA.asi e2e/scripts/
cp ../AC2TAA.ini e2e/scripts/
$CXX $FLAGS -shared -Wno-dll-attribute-on-redeclaration -Wno-missing-exception-spec -Wl,--kill-at -o e2e/d3d9.dll mock/mock_d3d9.cpp
$CXX $FLAGS -o e2e/AssassinsCreedIIGame.exe e2e_test.cpp -ld3d9
(cd e2e && ./AssassinsCreedIIGame.exe)

SO="${SO_ASI:-../../dist/scripts/AC2ShadowOverhaul.asi}"
if [ -f "$SO" ]; then
	echo "== end-to-end with AC2ShadowOverhaul.asi ($SO)"
	cp "$SO" e2e/scripts/
	cp "$(dirname "$SO")/AC2ShadowOverhaul.ini" e2e/scripts/ 2>/dev/null || true
	(cd e2e && ./AssassinsCreedIIGame.exe)
	echo "== the same, loaded in the opposite order"
	(cd e2e && E2E_REVERSE=1 ./AssassinsCreedIIGame.exe)
else
	echo "== AC2ShadowOverhaul.asi not found; chained test skipped"
fi
