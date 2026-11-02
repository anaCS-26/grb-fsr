# GRB-FSR

AMD FSR 4 upscaling for **Tom Clancy's Ghost Recon Breakpoint** (DirectX 11).

GRB-FSR is based on [GRB-DLSS](https://github.com/umairBinMohammad/grb-dlss) by Umair Bin Mohammad, which replaces the game's TAA ("Temporal Injection") pass with an upscaler fed by the game's own low-resolution colour, depth, motion vectors and jitter. This project adds what's needed to run FSR 4 on AMD GPUs.

## Status

**Work in progress. There is no release yet.**

- **Working now:** the mod runs on AMD GPUs. Upstream GRB-DLSS can't: its context hooks are lost there (see [AMD fix](#amd-fix)). Paired with [OptiScaler](https://github.com/optiscaler/OptiScaler), the mod's DLSS calls become FSR 4. Tested on a Radeon RX 6700S, where OptiScaler uses the driver's FSR 4.1.2 RDNA 2 model.
- **In testing:** a built-in FSR backend (`Backend=fsr`), no OptiScaler needed. The mod runs AMD's FidelityFX API ([SDK 2.4](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)) on its own DX11→DX12 bridge (`src/bridge12.cpp`, `src/fsr.cpp`), and gives FSR the game's real camera near/far/FOV. The offline test (`tools/bridge_test.sh`) passes on an RX 6700S with FSR 4.1.2. It has not been tested in-game yet.

## Requirements

- An FSR 4 capable AMD GPU (RX 6000 series or newer) and a current Adrenalin driver. FSR 4.1 for RX 6000 needs Adrenalin 26.10.1 or newer.
- Ghost Recon Breakpoint on **DirectX 11** (`GRB.exe`, not Vulkan). In Steam, choose the DX11 entry when you press Play.
- In the game's video settings: **Anti-Aliasing on, Temporal Injection on**. The mod hooks the TAA pass, so it does nothing without TAA.

## Install (built-in FSR, in testing)

1. Build `dinput8.dll` (see [Build](#build)) and copy it and `config/GRB_DLSS.ini` next to `GRB.exe`.
2. From the FidelityFX SDK 2.4 (`Kits/FidelityFX/signedbin/`), copy `amd_fidelityfx_loader_dx12.dll` and `amd_fidelityfx_upscaler_dx12.dll` to the same folder. These are AMD's signed DLLs; see AMD's license in [`third_party/ffx/license.md`](third_party/ffx/license.md).
3. In `GRB_DLSS.ini`, set `Backend=fsr` under `[Upscaler]`. Remove or rename OptiScaler (`winmm.dll`, `dxgi.dll`, ...) if it is installed.
4. `GRB_DLSS.log` should show `FSR context ... using 4.1.2` (or `3.1.x` on GPUs without FSR 4).

## Install (with OptiScaler)

1. Build `dinput8.dll` (see [Build](#build)) and copy it and `config/GRB_DLSS.ini` next to `GRB.exe`.
2. Install OptiScaler as `winmm.dll` in the same folder. Tested with 0.9.4 and with the v10 nightly of 2026-11-01.
3. In `OptiScaler.ini`, set:
   - **0.9.4:** `Dx11Upscaler=fsr31_12`, `UpscalerIndex=0`, `Fsr4ForceEnableInt8=true`
   - **v10 nightly:** `Dx11Upscaler=ffx_12`
   - **Both:** `Dxgi=false` (no GPU spoofing needed), and `Fsr4EnableWatermark=true` while you check that it works.
4. Start the game. The FSR watermark should read `FSR4…` (on RX 6000: `FSR4-i8-2 … Source: Driver`), not `FSR3`. NVIDIA's `nvngx_dlss.dll` is **not** needed.

In game: **Home** opens the mod's menu (render scale), **End** switches between the upscaler and the game's TAA, **Page Up/Down** change the render scale. On laptops without these keys they are usually Fn + arrow keys. OptiScaler's own menu defaults to **Insert**; set `ShortcutKey` in `OptiScaler.ini` if your keyboard has none.

> Breakpoint is an always-online game. The mod only touches rendering, but use it at your own risk, especially in PvP (Ghost War).

## AMD fix

AMD drivers don't implement D3D11 driver command lists, so the d3d11 runtime emulates deferred contexts. Each context gets its own copy of a static per-flavor function table inside `d3d11.dll`. While the runtime batches state changes, it rewrites entries of that copy with "amortized" wrappers (`TID3D11DeviceContext_Draw_Amortized` and so on), which erases per-context vtable hooks. GRB-DLSS therefore saw the TAA shaders being created but never a TAA draw.

The wrappers always dispatch through the static table, and the copy is refilled from it. So the mod now also hooks the static table each context was built from, and forwards to that table's original entries. See `RtFindTable` in [`src/mod.cpp`](src/mod.cpp).

## Build

Cross-build from Linux with `build.sh` (clang, lld, mingw-w64 headers). Or build natively on Windows from Git Bash with `build-win.sh`, which needs LLVM `clang`/`ld.lld` on `PATH` and MSYS2's mingw-w64 headers (default `SYSROOT=/c/msys64/mingw64`). Output: `out/dinput8.dll`.

## Configuration

See [`config/GRB_DLSS.ini`](config/GRB_DLSS.ini). Every key is documented inline. Most changes apply in-game when the file is saved.

## Credits

- [GRB-DLSS](https://github.com/umairBinMohammad/grb-dlss) by Umair Bin Mohammad: the TAA replacement, render-scale patch, in-game menu and everything else this builds on.
- [OptiScaler](https://github.com/optiscaler/OptiScaler), the first FSR 4 path for this mod, and the model for the bridge's shared fence.
- AMD FSR / FidelityFX SDK: the API headers in `third_party/ffx` (MIT) and the signed runtime DLLs. DejaVu fonts, used for the menu.

## License

MIT. See [LICENSE](LICENSE).
