# Intel XeSS SDK headers

`inc/xess/xess.h` and `inc/xess/xess_vk.h` from Intel's XeSS SDK
(https://github.com/intel/xess, commit de0fb9c1c510661c571164e1418ceca8101dab69), under the MIT
licence in `LICENSE.txt`. The API matches SDK 3.0.2.

Lichii only uses them to call Intel's `libxess.dll` through function pointers
(`src/video_core/vulkan_common/xess_runtime.cpp`). The DLL itself is not part of this repository: it
is Intel's binary under the Intel Simplified Software License, placed next to `lichii.exe` by the
release packages and the local build script, with Intel's licence beside it.
