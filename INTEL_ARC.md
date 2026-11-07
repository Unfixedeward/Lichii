# Lichii for Intel Arc

This fork is upstream [Lichii](https://github.com/zortos293/Lichii) with fixes for Intel Arc GPUs,
made while getting The Legend of Zelda: Ocarina of Time (Switch 2) running on an Arc B580. The
`intel-arc` branch is the current upstream build plus these changes; on GPUs other than Intel it
behaves like upstream.

It's unofficial. Report problems with these builds in this fork's
[issues](https://github.com/Unfixedeward/Lichii/issues), not to upstream.

## Download and install

1. Open the [Releases](https://github.com/Unfixedeward/Lichii/releases) page and take the newest
   build. On Windows, the **PGO zip** is the one to try first; the standard zip is the fallback if
   it doesn't start.
2. Extract the whole zip somewhere and run `lichii.exe`.
3. Saves, settings, keys and shader caches are shared with other Lichii and Eden builds. Games, keys
   and firmware are not included: use only what you've legally dumped yourself.
4. The built-in updater offers new releases of this fork (not upstream ones, which lack the fixes).

Suggested settings: GPU Accuracy **Fast**, Async shaders **On**. In Ocarina's per-game settings,
try anti-aliasing **No sharpening**: it skips the game's sharpening network, the single heaviest
GPU job (about 6 ms per frame on a B580). Keep your Intel graphics driver up to date.

The first launch of a game builds its shaders, which takes a while. That's normal.

## What it changes on Intel

| Change | Why | Switch |
|---|---|---|
| Faster software tensor math | Ocarina's sharpening network uses the Switch 2's matrix instructions. Upstream runs them on the GPU's matrix units only with a 16x16x16 float16 shape, and otherwise falls back to software that reads other lanes with per-lane shuffles, which Intel's compiler turns into slow indirect register reads. This fork uses quad and subgroup broadcasts instead. On the Frizti fork this took a B580 from about 10 to 30+ fps. | `EDEN_NX2_HMMA_BROADCAST=0` turns it off |
| Counter reports wait for their fence at Fast/Low GPU accuracy | Based on upstream PR #337, limited to occlusion counters. Fixes white flashes on leaves and walls. | `EDEN_NX2_FENCE_REPORTS=0` off, `=2` also semaphore reports |
| Shader barriers kept on discrete Arc cards | Upstream removes them on every Intel GPU, a workaround for old integrated-GPU drivers. Integrated GPUs keep the workaround. | `EDEN_NX2_INTEL_STRIP_BARRIERS=1` removes them |
| Occlusion checks resolved on discrete Arc cards | Upstream skips them on Intel at Fast/Low accuracy, which draws glows and flares that should be hidden behind walls. | `EDEN_NX2_INTEL_SKIP_OCCLUSION=1` skips them |
| Optional native float16 on Intel | Off by default, as upstream. | `EDEN_NX2_INTEL_FP16=1` |
| ASTC textures with unset component types read correctly | Ocarina's sRGB ASTC 10x6 textures were read as plain pixels. | none |
| FPS log | The frame rate and the slowest frame every N seconds, in the log. Handy for comparing builds. | `EDEN_NX2_FPS_LOG=10` |
| Log of the cooperative matrix shapes your GPU offers | Shows whether the matrix units could run the tensor network natively. | none |

### Switches

Switches are `NAME=VALUE` lines, one per line, in `nx2_switches.txt`; `#` starts a comment. Put
your own in `%APPDATA%\eden\nx2_switches.txt` on Windows (create it if it's missing), which
overrides the `nx2_switches.txt` next to `lichii.exe` and survives updates. They're read when
Lichii starts.

## Building from source

The `intel-arc` branch builds like upstream; see [docs/Build.md](docs/Build.md). Releases are built
by `.github/workflows/release-binaries.yml` on every push to `intel-arc`, the same way as
upstream's: clang-cl with ThinLTO on Windows, plus a PGO variant trained on Ocarina of Time.

If you build on Windows with Visual Studio 2026 (MSVC 14.51) and clang-cl, add
`-D_USE_STD_VECTOR_ALGORITHMS=0` to the C and C++ flags: that version of the C++ library fails
with "static assertion failed: unexpected size" in `std::find` (microsoft/STL issue 6294).
Visual Studio 2022 doesn't need it.

## Credits

All the emulator work belongs to the Lichii, Eglantine, Eden, yuzu and Sudachi developers; see
[README.md](README.md#credits-and-license). The Intel Arc fixes are released under the same
license (GPL-3.0-or-later).
