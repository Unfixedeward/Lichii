// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Intel XeSS Super Resolution through Intel's libxess.dll (XeSS SDK 3.0.2), loaded at run time.
// The DLL is Intel's binary: the release packages and the local build script put it next to
// lichii.exe; %APPDATA%\lichii\xess\ is searched too. Only a DLL signed by Intel Corporation loads.

#pragma once

#include <string_view>

#include "video_core/vulkan_common/vulkan_wrapper.h"

// After vulkan.h, which the wrapper includes with the platform macros it needs.
#include "xess/xess_vk.h"

namespace Vulkan::XessRuntime {

/// Intel's pinned runtime (XeSS SDK 3.0.2).
inline constexpr std::string_view DLL_NAME = "libxess.dll";
inline constexpr std::string_view SDK_VERSION = "3.0.2";

struct Api {
    decltype(&xessGetVersion) get_version{};
    decltype(&xessGetIntelXeFXVersion) get_xefx_version{};
    decltype(&xessGetOptimalInputResolution) get_optimal_input_resolution{};
    decltype(&xessDestroyContext) destroy_context{};
    decltype(&xessSetLoggingCallback) set_logging_callback{};
    decltype(&xessIsOptimalDriver) is_optimal_driver{};
    decltype(&xessSetJitterScale) set_jitter_scale{};
    decltype(&xessSetVelocityScale) set_velocity_scale{};
    decltype(&xessVKGetRequiredInstanceExtensions) vk_get_required_instance_extensions{};
    decltype(&xessVKGetRequiredDeviceExtensions) vk_get_required_device_extensions{};
    decltype(&xessVKGetRequiredDeviceFeatures) vk_get_required_device_features{};
    decltype(&xessVKCreateContext) vk_create_context{};
    decltype(&xessVKInit) vk_init{};
    decltype(&xessVKExecute) vk_execute{};
};

/// Whether XeSS is asked for: the "Use Intel XeSS" setting or EDEN_NX2_XESS=1.
[[nodiscard]] bool Wanted();

/// Intel's runtime, loaded once and kept for the process, or null when it's missing, unsigned,
/// lacks an export, or outside Windows (the log says which).
[[nodiscard]] const Api* Get();

[[nodiscard]] std::string_view ResultName(xess_result_t result);

} // namespace Vulkan::XessRuntime
