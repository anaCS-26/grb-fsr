#!/bin/bash
# Builds the release zip: out/GRB-FSR-<version>.zip with dinput8.dll (run build-win.sh first), the ini, AMD's two
# signed FidelityFX DLLs and their license, and INSTALL.txt. Needs python on PATH (for the zip).
#   tools/package.sh <folder with amd_fidelityfx_loader_dx12.dll and amd_fidelityfx_upscaler_dx12.dll>
# (FidelityFX SDK 2.4: Kits/FidelityFX/signedbin/)
set -e
cd "$(dirname "$0")/.."
FFX="${1:?usage: tools/package.sh <FidelityFX signedbin folder>}"
VER=$(sed -n 's/^#define MOD_VERSION "\(.*\)"/\1/p' src/common.h)
STAGE=build/package
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp out/dinput8.dll config/GRB_FSR.ini "$STAGE/"
cp "$FFX/amd_fidelityfx_loader_dx12.dll" "$FFX/amd_fidelityfx_upscaler_dx12.dll" "$STAGE/"
cp third_party/ffx/license.md "$STAGE/AMD-FidelityFX-license.md"
cp tools/INSTALL.txt "$STAGE/INSTALL.txt"
sed -i 's/\r*$/\r/' "$STAGE/INSTALL.txt" "$STAGE/GRB_FSR.ini" # Notepad-friendly line ends
ZIP="out/GRB-FSR-$VER.zip"
rm -f "$ZIP"
(cd "$STAGE" && python -m zipfile -c "../../$ZIP" *)
ls -la "$ZIP"
python -m zipfile -l "$ZIP"
