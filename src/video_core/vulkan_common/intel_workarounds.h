// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

namespace Vulkan::IntelWorkarounds {

/// Reads a 0/1 switch from the environment (or nx2_switches.txt); `fallback` when unset.
inline bool EnvFlag(const char* env_name, bool fallback) {
    const char* const value = std::getenv(env_name);
    if (value != nullptr && *value != '\0') {
        return *value != '0';
    }
    return fallback;
}

/// Whether an Intel-only workaround is active. The workarounds were written for older Intel
/// integrated-GPU drivers, so they stay on for integrated GPUs and are off for discrete Arc GPUs,
/// whose drivers handle the original behaviour. The environment variable overrides that choice:
/// 1 turns the workaround on, 0 turns it off.
///   EDEN_NX2_INTEL_STRIP_BARRIERS  removes barriers inside shader control flow
///   EDEN_NX2_INTEL_SKIP_OCCLUSION  draws instead of resolving occlusion-query comparisons
inline bool Enabled(const char* env_name, bool is_integrated) {
    return EnvFlag(env_name, is_integrated);
}

} // namespace Vulkan::IntelWorkarounds
