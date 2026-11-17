#!/bin/sh
# Offline tests (no game or GPU needed, except where noted). Usage: ./run_tests.sh [path-to-llvm-mingw/bin]
#  1. mock end-to-end test: loads the built .asi into a test exe with a fake d3d9.dll, then checks the
#     shader rewrite, the F6 toggle, the debug menu keys and the ini saving.
#  2. soft shadow shader test: runs the rewriter over real shadow shaders exported by the F8 frame dump
#     (copy the dump's *_shaders folder to ../re/shaders/dump, or set SHADERS=dir). Skipped if absent.
set -e
cd "$(dirname "$0")"
TC="${1:-$LLVM_MINGW}"
[ -n "$TC" ] && TC="$TC/"
CXX="${TC}i686-w64-mingw32-clang++"
FLAGS="-O2 -fuse-ld=lld -static"

sh ../build.sh "${1:-}" >/dev/null
mkdir -p fdtest/scripts
cp ../dist/scripts/AC2ShadowOverhaul.asi fdtest/scripts/
$CXX $FLAGS -shared -Wl,--kill-at -o fdtest/d3d9.dll mockd3d/mock_d3d9.cpp
$CXX $FLAGS -o fdtest/AssassinsCreedIIGame.exe framedump_test.cpp -ld3d9
echo "== mock end-to-end test"
(cd fdtest && rm -f scripts/*.log && ./AssassinsCreedIIGame.exe)

SHADERS="${SHADERS:-../re/shaders/dump}"
if [ -d "$SHADERS" ]; then
	echo "== soft shadow shader test ($SHADERS)"
	$CXX $FLAGS -I../src -o softshadow_test.exe softshadow_test.cpp
	./softshadow_test.exe "$SHADERS" | tail -2
else
	echo "== soft shadow shader test skipped (no exported shaders found)"
fi
