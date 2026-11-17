#!/bin/sh
# Builds dist/scripts/AC2ShadowOverhaul.asi and dist/AC2Depth.addon32 with llvm-mingw (32-bit target).
# Usage: ./build.sh [path-to-llvm-mingw/bin]   (or set LLVM_MINGW; falls back to whatever is on PATH)
set -e
cd "$(dirname "$0")"
TC="${1:-$LLVM_MINGW}"
[ -n "$TC" ] && TC="$TC/"
CXX="${TC}i686-w64-mingw32-clang++"
CC="${TC}i686-w64-mingw32-clang"

"$CC" -fsyntax-only tools/vtbl_check.c

mkdir -p dist/scripts
"$CXX" -shared -O2 -std=c++17 -fms-extensions -fasm-blocks -Wall -Wno-unused-function \
	-DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS \
	-fuse-ld=lld -static -static-libgcc -static-libstdc++ -s \
	-o dist/scripts/AC2ShadowOverhaul.asi src/main.cpp src/common.cpp src/shadows.cpp src/framedump.cpp src/crashlog.cpp src/softshadows.cpp src/tuning.cpp
"$CXX" -shared -O2 -std=c++17 -fms-extensions -Wall -Wno-unused-function -Wno-ignored-attributes -Wno-missing-braces \
	-DWIN32_LEAN_AND_MEAN -DNOMINMAX -fuse-ld=lld -static -static-libgcc -static-libstdc++ -s \
	-o dist/AC2Depth.addon32 addon/ac2_depth.cpp
cp addon/reshade/LICENSE.md dist/ReShade-API-LICENSE.md
cp AC2ShadowOverhaul.ini dist/scripts/
cp README.md LICENSE dist/
ls -l dist dist/scripts
