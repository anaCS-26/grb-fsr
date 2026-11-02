#!/bin/bash
# Native Windows build (Git Bash / MSYS2): LLVM clang + lld against the MSYS2 mingw-w64 headers and
# import libraries, no static CRT. Same flags as build.sh.
#   SYSROOT  prefix with include/ and lib/ (default: /c/msys64/mingw64)
#   clang, clang++ and ld.lld must be on PATH (any recent LLVM, e.g. the one bundled with the Swift toolchain)
set -e
cd "$(dirname "$0")"
SYS="${SYSROOT:-/c/msys64/mingw64}"
mkdir -p build out
CXXFLAGS="--target=x86_64-w64-windows-gnu -nostdinc -isystem $SYS/include -isystem $(clang -print-resource-dir)/include -O2 -std=c++17 \
  -fno-exceptions -fno-rtti -fno-asynchronous-unwind-tables -ffreestanding -fno-math-errno -fno-stack-protector \
  -mno-stack-arg-probe -fno-threadsafe-statics -Wall -Wno-unused-function -Wno-missing-braces \
  -Wno-unknown-pragmas -Wno-unused-variable -Wno-unused-but-set-variable -D_WIN32_WINNT=0x0A00 -DUNICODE -D_UNICODE"
OBJS=""
for f in src/*.cpp; do
  o=build/$(basename "${f%.cpp}").o
  clang++ $CXXFLAGS -c "$f" -o "$o"
  OBJS="$OBJS $o"
done
# local fabs/sqrt/floor: MSYS2's msvcrt import lib would otherwise need runtime pseudo-relocations
clang --target=x86_64-w64-windows-gnu -O2 -ffreestanding -fno-math-errno -c tools/mathshim.c -o build/mathshim.o
ld.lld -m i386pep --shared --entry DllMainCRTStartup --subsystem windows --dynamicbase --nxcompat --high-entropy-va \
  -o out/dinput8.dll $OBJS build/mathshim.o src/exports.def -L$SYS/lib -lkernel32 -luser32 -ladvapi32 -lmsvcrt \
  --out-implib build/dinput8.lib
ls -la out/dinput8.dll
