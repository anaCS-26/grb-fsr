#!/bin/bash
# Builds and runs the offline DX11 -> DX12 bridge test (Git Bash on Windows, same toolchain as build-win.sh,
# plus MSYS2's mingw-w64 g++ for the CRT). Arguments: [adapter index] [fsr <folder with the FidelityFX DLLs>]
set -e
cd "$(dirname "$0")/.."
SYS="${SYSROOT:-/c/msys64/mingw64}"
mkdir -p build out
clang++ --target=x86_64-w64-windows-gnu -nostdinc -isystem $SYS/include -isystem $(clang -print-resource-dir)/include \
  -O2 -std=c++17 -fno-exceptions -fno-rtti -D_WIN32_WINNT=0x0A00 -DUNICODE -D_UNICODE -c src/bridge12.cpp -o build/bridge12_test.o
clang++ --target=x86_64-w64-windows-gnu -nostdinc -isystem $SYS/include -isystem $(clang -print-resource-dir)/include \
  -O2 -std=c++17 -fno-exceptions -fno-rtti -D_WIN32_WINNT=0x0A00 -DUNICODE -D_UNICODE -c src/fsr.cpp -o build/fsr_test.o
clang++ --target=x86_64-w64-windows-gnu -nostdinc -isystem $SYS/include -isystem $(clang -print-resource-dir)/include \
  -O2 -std=c++17 -fno-exceptions -fno-rtti -D_WIN32_WINNT=0x0A00 -DUNICODE -D_UNICODE -c tools/bridge_test.cpp -o build/bridge_test.o
PATH="$SYS/bin:$PATH" g++ -o out/bridge_test.exe build/bridge_test.o build/bridge12_test.o build/fsr_test.o -ld3d11
PATH="$SYS/bin:$PATH" ./out/bridge_test.exe "$@"
