#!/bin/sh
# Offline tests (no game or GPU needed). Usage: ./run_tests.sh [path-to-llvm-mingw/bin]
#  1. end-to-end: loads the built .asi into a test exe with a fake d3d9.dll and renders frames
#     shaped like AC2's (both placements, fog off, and next to AC2 Shadow Overhaul if
#     ../../dist/scripts/AC2ShadowOverhaul.asi exists).
#  2. shaders: runs the real shaders on Windows' software renderer (WARP) over a synthetic town and
#     checks them against a CPU reference; writes warp_*.png to look at.
set -e
cd "$(dirname "$0")"
TC="${1:-$LLVM_MINGW}"
[ -z "$TC" ] && [ -d ../../re/tools/llvm-mingw-20261104-msvcrt-x86_64/bin ] && TC=../../re/tools/llvm-mingw-20261104-msvcrt-x86_64/bin
[ -n "$TC" ] && TC="$TC/"
CXX="${TC}i686-w64-mingw32-clang++"
FLAGS="-O2 -fuse-ld=lld -static"

sh ../build.sh "${1:-}" >/dev/null
mkdir -p e2e/scripts
cp ../dist/scripts/AC2Volumetrics.asi e2e/scripts/
$CXX $FLAGS -w -shared -Wl,--kill-at -o e2e/d3d9.dll mockd3d/mock_d3d9.cpp
$CXX $FLAGS -o e2e/AssassinsCreedIIGame.exe e2e_test.cpp -ld3d9
echo "== end-to-end: placement 0"
(cd e2e && ./AssassinsCreedIIGame.exe)
echo "== end-to-end: placement 1"
(cd e2e && ./AssassinsCreedIIGame.exe placement1)
echo "== end-to-end: fog off"
(cd e2e && ./AssassinsCreedIIGame.exe fogoff)
if [ -f ../../dist/scripts/AC2ShadowOverhaul.asi ]; then
	echo "== end-to-end: next to AC2 Shadow Overhaul"
	cp ../../dist/scripts/AC2ShadowOverhaul.asi e2e/scripts/
	(cd e2e && ./AssassinsCreedIIGame.exe overhaul)
	rm -f e2e/scripts/AC2ShadowOverhaul.asi
else
	echo "== end-to-end next to AC2 Shadow Overhaul skipped (../../dist/scripts/AC2ShadowOverhaul.asi not built)"
fi

echo "== shaders on WARP"
$CXX $FLAGS -o warp_test.exe warp_test.cpp -ld3d11
./warp_test.exe
