// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>

#include "common/common_types.h"
#include "common/logging.h"

/// EDEN_NX2_FPS_LOG=<seconds>: every that many seconds, the game's frame rate and its slowest
/// frame over the period, in the log. For comparing settings on one machine.
namespace VideoCommon::Nx2FpsLog {

/// EDEN_NX2_AB=1: the logger turns the test switches that read this (EDEN_NX2_ACQUIRE_EQUAL,
/// EDEN_NX2_GPU_WAIT_SPIN_US) off and on in alternate periods and says which in each line, to
/// compare the two in one session.
inline std::atomic<bool> g_ab_phase_on{true};

class Logger {
public:
    static Logger& Get() {
        static Logger logger;
        return logger;
    }

    /// Once per game frame (GPU::RendererFrameEndNotify).
    void OnFrame() {
        if (period.count() == 0) {
            return;
        }
        std::scoped_lock lock{mutex};
        const Clock::time_point now = Clock::now();
        if (!started) {
            started = true;
            period_start = now;
            last_frame = now;
            return;
        }
        worst = (std::max)(worst, now - last_frame);
        last_frame = now;
        ++frames;
        if (now - period_start < period) {
            return;
        }
        const double seconds = std::chrono::duration<double>(now - period_start).count();
        const bool phase_on = g_ab_phase_on.load(std::memory_order_relaxed);
        LOG_INFO(Render, "FPS: {:.1f} over {:.0f} s, slowest frame {:.1f} ms{}",
                 static_cast<double>(frames) / seconds, seconds,
                 std::chrono::duration<double, std::milli>(worst).count(),
                 !ab_enabled ? "" : phase_on ? " [test switches on]" : " [test switches off]");
        if (ab_enabled) {
            g_ab_phase_on.store(!phase_on, std::memory_order_relaxed);
        }
        period_start = now;
        frames = 0;
        worst = {};
    }

private:
    using Clock = std::chrono::steady_clock;

    Logger() {
        const char* const value = std::getenv("EDEN_NX2_FPS_LOG");
        if (value != nullptr && *value != '\0') {
            period = std::chrono::seconds{std::strtoul(value, nullptr, 10)};
        }
        const char* const ab = std::getenv("EDEN_NX2_AB");
        ab_enabled = period.count() != 0 && ab != nullptr && ab[0] == '1';
    }

    bool ab_enabled{};

    std::mutex mutex;
    std::chrono::seconds period{};
    bool started{};
    Clock::time_point period_start{};
    Clock::time_point last_frame{};
    u64 frames{};
    Clock::duration worst{};
};

} // namespace VideoCommon::Nx2FpsLog
