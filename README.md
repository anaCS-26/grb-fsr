# GRB-FSR

AMD FSR 4 upscaling for **Tom Clancy's Ghost Recon Breakpoint** (DirectX 11).

**Download:** [Nexus Mods](https://www.nexusmods.com/ghostreconbreakpoint/mods/2243) or the [latest GitHub release](../../releases/latest).

GRB-FSR is based on [GRB-DLSS](https://github.com/umairBinMohammad/grb-dlss) by Umair Bin Mohammad, which replaces the game's TAA ("Temporal Injection") pass with an upscaler fed by the game's own low-resolution colour, depth, motion vectors and jitter. This project adds what's needed to run FSR 4 on AMD GPUs.

## Status

**Early testing (0.1.0).**

- **Built-in FSR (default):** the mod runs AMD's FidelityFX API ([SDK 2.4](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)) itself, on its own DX11→DX12 bridge (`src/bridge12.cpp`, `src/fsr.cpp`). No OptiScaler needed. FSR gets the game's real camera near/far/FOV. Tested in-game on a Radeon RX 6700S with FSR 4.1.2: sharp image, no errors over a full session.
- The menu (Home) picks the FSR version (newest, or FSR 4 / 3 / 2) and FSR sharpness. On the RX 6700S at 1080p with a 50% render scale, FSR 4.1.2 costs about 2.2 ms per frame and FSR 3.1.6 about 1 ms (`tools/bridge_test.sh`).
- The mod also runs on AMD GPUs through upstream's NGX path. Upstream GRB-DLSS can't: its context hooks are lost there (see [AMD fix](#amd-fix)). Paired with [OptiScaler](https://github.com/optiscaler/OptiScaler) (`Backend=ngx`), the mod's DLSS calls become FSR 4.

## Requirements

- An FSR 4 capable AMD GPU (RX 6000 series or newer) and a current Adrenalin driver. FSR 4.1 for RX 6000 needs Adrenalin 26.10.1 or newer.
- Ghost Recon Breakpoint on **DirectX 11** (`GRB.exe`, not Vulkan). In Steam, choose the DX11 entry when you press Play.
- In the game's video settings: **Anti-Aliasing on, Temporal Injection on**. The mod hooks the TAA pass, so it does nothing without TAA.

## Install

1. Download `GRB-FSR-<version>.zip` from [Nexus Mods](https://www.nexusmods.com/ghostreconbreakpoint/mods/2243) or the [latest release](../../releases/latest) and copy everything in it (`dinput8.dll`, `GRB_FSR.ini`, AMD's `amd_fidelityfx_loader_dx12.dll` and `amd_fidelityfx_upscaler_dx12.dll`, and AMD's license) next to `GRB.exe`.
2. Remove or rename OptiScaler (`winmm.dll`, `dxgi.dll`, ...) if it is installed.
3. Start the game. The mod's menu (**Home**) shows `FSR 4.1.2` (or `3.1.x` on GPUs without FSR 4), and `GRB_FSR.log` shows `FSR context ... using 4.1.2`.

To build the zip yourself: build `dinput8.dll` (see [Build](#build)), then run `tools/package.sh <FidelityFX SDK 2.4 Kits/FidelityFX/signedbin folder>`. The two AMD DLLs are AMD's signed binaries, redistributed under AMD's license in [`third_party/ffx/license.md`](third_party/ffx/license.md).

Upgrading from GRB-DLSS or an earlier GRB-FSR build: the settings file is now `GRB_FSR.ini` (and the log `GRB_FSR.log`). The old `GRB_DLSS.ini` is no longer read.

## Install (with OptiScaler)

1. Copy `dinput8.dll` and `GRB_FSR.ini` next to `GRB.exe`, and set `Backend=ngx` under `[Upscaler]` in `GRB_FSR.ini`.
2. Install OptiScaler as `winmm.dll` in the same folder. Tested with 0.9.4 and with the v10 nightly of 2026-11-01.
3. In `OptiScaler.ini`, set:
   - **0.9.4:** `Dx11Upscaler=fsr31_12`, `UpscalerIndex=0`, `Fsr4ForceEnableInt8=true`
   - **v10 nightly:** `Dx11Upscaler=ffx_12`
   - **Both:** `Dxgi=false` (no GPU spoofing needed), and `Fsr4EnableWatermark=true` while you check that it works.
4. Start the game. The FSR watermark should read `FSR4…` (on RX 6000: `FSR4-i8-2 … Source: Driver`), not `FSR3`. NVIDIA's `nvngx_dlss.dll` is **not** needed.

In game: **Home** opens the mod's menu (render scale, FSR version and sharpness), **End** switches between the upscaler and the game's TAA, **Page Up/Down** change the render scale. On laptops without these keys they are usually Fn + arrow keys. OptiScaler's own menu defaults to **Insert**; set `ShortcutKey` in `OptiScaler.ini` if your keyboard has none.

> Breakpoint is an always-online game. The mod only touches rendering, but use it at your own risk, especially in PvP (Ghost War).

## AMD fix

AMD drivers don't implement D3D11 driver command lists, so the d3d11 runtime emulates deferred contexts. Each context gets its own copy of a static per-flavor function table inside `d3d11.dll`. While the runtime batches state changes, it rewrites entries of that copy with "amortized" wrappers (`TID3D11DeviceContext_Draw_Amortized` and so on), which erases per-context vtable hooks. GRB-DLSS therefore saw the TAA shaders being created but never a TAA draw.

The wrappers always dispatch through the static table, and the copy is refilled from it. So the mod now also hooks the static table each context was built from, and forwards to that table's original entries. See `RtFindTable` in [`src/mod.cpp`](src/mod.cpp).

## Build

Cross-build from Linux with `build.sh` (clang, lld, mingw-w64 headers). Or build natively on Windows from Git Bash with `build-win.sh`, which needs LLVM `clang`/`ld.lld` on `PATH` and MSYS2's mingw-w64 headers (default `SYSROOT=/c/msys64/mingw64`). Output: `out/dinput8.dll`.

## Configuration

See [`config/GRB_FSR.ini`](config/GRB_FSR.ini). Every key is documented inline. Most changes apply in-game when the file is saved.

## Credits

- [GRB-DLSS](https://github.com/umairBinMohammad/grb-dlss) by Umair Bin Mohammad: the TAA replacement, render-scale patch, in-game menu and everything else this builds on.
- [OptiScaler](https://github.com/optiscaler/OptiScaler), the first FSR 4 path for this mod, and the model for the bridge's shared fence.
- AMD FSR / FidelityFX SDK: the API headers in `third_party/ffx` (MIT) and the signed runtime DLLs. DejaVu fonts, used for the menu.

## License

MIT. See [LICENSE](LICENSE).
