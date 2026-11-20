#!/bin/sh
# Builds dist/scripts/AC2ShadowOverhaul.asi with llvm-mingw (32-bit target).
# Usage: ./build.sh [path-to-llvm-mingw/bin]   (or set LLVM_MINGW; falls back to the copy in
# ../re/tools, then to whatever is on PATH)
# After editing an effect's HLSL, regenerate its shaders_gen.h (see tools/gen_all_shaders.sh).
set -e
cd "$(dirname "$0")"
TC="${1:-$LLVM_MINGW}"
[ -z "$TC" ] && [ -d ../re/tools/llvm-mingw-20261104-msvcrt-x86_64/bin ] && TC=../re/tools/llvm-mingw-20261104-msvcrt-x86_64/bin
[ -n "$TC" ] && TC="$TC/"
CXX="${TC}i686-w64-mingw32-clang++"
CC="${TC}i686-w64-mingw32-clang"

"$CC" -fsyntax-only tools/vtbl_check.c

SRC="src/main.cpp src/common.cpp src/shadows.cpp src/framedump.cpp src/crashlog.cpp src/softshadows.cpp src/tuning.cpp
	src/ao.cpp src/aoshader.cpp src/features.cpp
	src/core/shaderinfo.cpp src/core/camera.cpp src/core/scene.cpp src/core/menu.cpp
	src/ssgi/gi.cpp src/ssgi/gishader.cpp src/ssgi/menu.cpp
	src/vol/volumetrics.cpp src/vol/menu.cpp
	src/taa/taa.cpp src/taa/vsjitter.cpp src/taa/menu.cpp"

mkdir -p dist/scripts
"$CXX" -shared -O2 -std=c++17 -fms-extensions -fasm-blocks -Wall -Wno-unused-function \
	-DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS -Isrc \
	-fuse-ld=lld -static -static-libgcc -static-libstdc++ -s \
	-o dist/scripts/AC2ShadowOverhaul.asi $SRC
cp AC2ShadowOverhaul.ini dist/scripts/
cp README.md LICENSE dist/
ls -l dist dist/scripts
