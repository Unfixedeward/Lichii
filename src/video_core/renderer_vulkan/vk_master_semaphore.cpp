// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#include <ranges>
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/nx2_fps_log.h"
#include "video_core/nx2_profile.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

constexpr u64 FENCE_RESERVE_SIZE = 8;

namespace {

// EDEN_NX2_GPU_WAIT_SPIN_US=<n>: poll the timeline semaphore for up to n microseconds before
// blocking in vkWaitSemaphores. A blocking wait adds the driver's and the OS's wake-up latency
// to every short wait, and Switch 2 titles wait for the host GPU hundreds of times a second.
s64 GpuWaitSpinUs() {
    static const s64 value = [] {
        const char* const text = std::getenv("EDEN_NX2_GPU_WAIT_SPIN_US");
        return text != nullptr ? static_cast<s64>(std::atoll(text)) : s64{0};
    }();
    return VideoCommon::Nx2FpsLog::g_spin_phase_on.load(std::memory_order_relaxed) ? value : 0;
}

// EDEN_NX2_GPU_WAIT_STATS=1: every 10 seconds, how long the waits for the host GPU took.
class GpuWaitStats {
public:
    static bool Enabled() {
        static const bool enabled = [] {
            const char* const text = std::getenv("EDEN_NX2_GPU_WAIT_STATS");
            return text != nullptr && text[0] == '1';
        }();
        return enabled;
    }

    void Note(std::chrono::nanoseconds elapsed, bool spun) {
        const u64 us = static_cast<u64>(elapsed.count() / 1000);
        const size_t bucket = us < 50 ? 0 : us < 200 ? 1 : us < 1000 ? 2 : us < 5000 ? 3 : 4;
        buckets[bucket].fetch_add(1, std::memory_order_relaxed);
        bucket_us[bucket].fetch_add(us, std::memory_order_relaxed);
        if (spun) {
            spin_hits.fetch_add(1, std::memory_order_relaxed);
        }
        const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        s64 last = last_log_ms.load(std::memory_order_relaxed);
        if (last == 0) {
            last_log_ms.compare_exchange_strong(last, now, std::memory_order_relaxed);
            return;
        }
        if (now - last < 10000 ||
            !last_log_ms.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
            return;
        }
        std::array<u64, 5> count;
        std::array<u64, 5> total;
        for (size_t i = 0; i < count.size(); ++i) {
            count[i] = buckets[i].exchange(0, std::memory_order_relaxed);
            total[i] = bucket_us[i].exchange(0, std::memory_order_relaxed);
        }
        const double seconds = static_cast<double>(now - last) / 1000.0;
        LOG_INFO(Render_Vulkan,
                 "GPU waits per second: <50us {:.0f} ({:.1f} ms), <200us {:.0f} ({:.1f} ms), "
                 "<1ms {:.0f} ({:.1f} ms), <5ms {:.0f} ({:.1f} ms), longer {:.0f} ({:.1f} ms); "
                 "ended while polling {:.0f}",
                 count[0] / seconds, total[0] / seconds / 1000.0, count[1] / seconds,
                 total[1] / seconds / 1000.0, count[2] / seconds, total[2] / seconds / 1000.0,
                 count[3] / seconds, total[3] / seconds / 1000.0, count[4] / seconds,
                 total[4] / seconds / 1000.0,
                 spin_hits.exchange(0, std::memory_order_relaxed) / seconds);
    }

private:
    std::array<std::atomic<u64>, 5> buckets{};
    std::array<std::atomic<u64>, 5> bucket_us{};
    std::atomic<u64> spin_hits{};
    std::atomic<s64> last_log_ms{};
};

GpuWaitStats gpu_wait_stats;

} // Anonymous namespace

MasterSemaphore::MasterSemaphore(const Device& device_) : device(device_) {
    if (!device.HasTimelineSemaphore()) {
        static constexpr VkFenceCreateInfo fence_ci{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0};
        free_queue.resize(FENCE_RESERVE_SIZE);
        std::ranges::generate(free_queue,
                              [&] { return device.GetLogical().CreateFence(fence_ci); });
        wait_thread = std::jthread([this](std::stop_token token) { WaitThread(token); });
        return;
    }

    static constexpr VkSemaphoreTypeCreateInfo semaphore_type_ci{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = nullptr,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    static constexpr VkSemaphoreCreateInfo semaphore_ci{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &semaphore_type_ci,
        .flags = 0,
    };
    semaphore = device.GetLogical().CreateSemaphore(semaphore_ci);

    if (!Settings::values.renderer_debug) {
        return;
    }
    // Validation layers have a bug where they fail to track resource usage when using timeline
    // semaphores and synchronizing with GetSemaphoreCounterValue. To workaround this issue, have
    // a separate thread waiting for each timeline semaphore value.
    debug_thread = std::jthread([this](std::stop_token stop_token) {
        u64 counter = 0;
        while (!stop_token.stop_requested()) {
            if (semaphore.Wait(counter, 10'000'000)) {
                ++counter;
            }
        }
    });
}

MasterSemaphore::~MasterSemaphore() = default;

void MasterSemaphore::Refresh() {
    if (!semaphore) {
        // If we don't support timeline semaphores, there's nothing to refresh
        return;
    }

    u64 this_tick{};
    u64 counter{};
    do {
        this_tick = gpu_tick.load(std::memory_order_acquire);
        counter = semaphore.GetCounter();
        if (counter < this_tick) {
            return;
        }
    } while (!gpu_tick.compare_exchange_weak(this_tick, counter, std::memory_order_release,
                                             std::memory_order_relaxed));
}

void MasterSemaphore::Wait(u64 tick) {
    if (!semaphore) {
        // Fast check: already reached the requested tick?
        if (gpu_tick.load(std::memory_order_acquire) >= tick) {
            return;
        }

        const VideoCommon::Nx2Profile::Scope scope{VideoCommon::Nx2Profile::Zone::GpuWait};
        u64 last_tick = gpu_tick.load(std::memory_order_relaxed);
        while (gpu_tick.load(std::memory_order_acquire) < tick) {
            gpu_tick.wait(last_tick, std::memory_order_acquire);
            last_tick = gpu_tick.load(std::memory_order_relaxed);
        }
        return;
    }

    // No need to wait if the GPU is ahead of the tick
    if (IsFree(tick)) {
        return;
    }

    // Update the GPU tick and try again
    Refresh();

    if (IsFree(tick)) {
        return;
    }

    // If none of the above is hit, fallback to a regular wait
    const VideoCommon::Nx2Profile::Scope scope{VideoCommon::Nx2Profile::Zone::GpuWait};
    const bool stats = GpuWaitStats::Enabled();
    const s64 spin_us = GpuWaitSpinUs();
    const auto start = stats || spin_us > 0 ? std::chrono::steady_clock::now()
                                             : std::chrono::steady_clock::time_point{};
    bool spun = false;
    if (spin_us > 0) {
        const auto deadline = start + std::chrono::microseconds{spin_us};
        do {
            std::this_thread::yield();
            Refresh();
            spun = IsFree(tick);
        } while (!spun && std::chrono::steady_clock::now() < deadline);
    }
    if (!spun) {
        try {
            while (!semaphore.Wait(tick)) {
            }
        } catch (const vk::Exception& exception) {
            // The throw ends the process; say why first (a device loss reached nothing else
            // here).
            LOG_CRITICAL(Render_Vulkan, "Waiting for GPU tick {} failed: {}", tick,
                         exception.what());
            throw;
        }
        Refresh();
    }
    if (stats) {
        gpu_wait_stats.Note(std::chrono::steady_clock::now() - start, spun);
    }
}

VkResult MasterSemaphore::SubmitQueue(vk::CommandBuffer& cmdbuf, vk::CommandBuffer& upload_cmdbuf,
                                      VkSemaphore signal_semaphore, VkSemaphore wait_semaphore,
                                      u64 host_tick) {
    const VideoCommon::Nx2Profile::Scope scope{VideoCommon::Nx2Profile::Zone::QueueSubmit};
    if (semaphore) {
        return SubmitQueueTimeline(cmdbuf, upload_cmdbuf, signal_semaphore, wait_semaphore,
                                   host_tick);
    } else {
        return SubmitQueueFence(cmdbuf, upload_cmdbuf, signal_semaphore, wait_semaphore, host_tick);
    }
}

static constexpr VkPipelineStageFlags wait_stage_mask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

VkResult MasterSemaphore::SubmitQueueTimeline(vk::CommandBuffer& cmdbuf,
                                              vk::CommandBuffer& upload_cmdbuf,
                                              VkSemaphore signal_semaphore,
                                              VkSemaphore wait_semaphore, u64 host_tick) {
    const VkSemaphore timeline_semaphore = *semaphore;

    if (device.HasSynchronization2()) {
        const std::array<VkCommandBufferSubmitInfo, 2> cmdbuffer_infos{{
            {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext = nullptr,
                .commandBuffer = *upload_cmdbuf,
                .deviceMask = 0,
            },
            {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext = nullptr,
                .commandBuffer = *cmdbuf,
                .deviceMask = 0,
            },
        }};

        std::array<VkSemaphoreSubmitInfo, 2> signal_infos{{
            {
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .pNext = nullptr,
                .semaphore = timeline_semaphore,
                .value = host_tick,
                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .deviceIndex = 0,
            },
            {},
        }};
        u32 num_signal_semaphores = 1;
        if (signal_semaphore) {
            signal_infos[1] = VkSemaphoreSubmitInfo{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .pNext = nullptr,
                .semaphore = signal_semaphore,
                .value = 0,
                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .deviceIndex = 0,
            };
            num_signal_semaphores = 2;
        }

        const u32 num_wait_semaphores = wait_semaphore ? 1 : 0;
        const VkSemaphoreSubmitInfo wait_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = wait_semaphore,
            .value = 0,
            .stageMask = static_cast<VkPipelineStageFlags2>(wait_stage_mask),
            .deviceIndex = 0,
        };

        const VkSubmitInfo2 submit_info2{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .pNext = nullptr,
            .flags = 0,
            .waitSemaphoreInfoCount = num_wait_semaphores,
            .pWaitSemaphoreInfos = num_wait_semaphores ? &wait_info : nullptr,
            .commandBufferInfoCount = static_cast<u32>(cmdbuffer_infos.size()),
            .pCommandBufferInfos = cmdbuffer_infos.data(),
            .signalSemaphoreInfoCount = num_signal_semaphores,
            .pSignalSemaphoreInfos = signal_infos.data(),
        };
        return device.GetGraphicsQueue().Submit2(submit_info2);
    }

    const u32 num_signal_semaphores = signal_semaphore ? 2 : 1;
    const std::array signal_values{host_tick, u64(0)};
    const std::array signal_semaphores{timeline_semaphore, signal_semaphore};

    const std::array cmdbuffers{*upload_cmdbuf, *cmdbuf};

    const u32 num_wait_semaphores = wait_semaphore ? 1 : 0;
    // Pointers must be null when the count is zero (best-practices)
    const VkSemaphore* p_wait_sems =
        (num_wait_semaphores > 0) ? &wait_semaphore : nullptr;
    const VkPipelineStageFlags* p_wait_masks =
        (num_wait_semaphores > 0) ? &wait_stage_mask : nullptr;
    const VkSemaphore* p_signal_sems =
        (num_signal_semaphores > 0) ? signal_semaphores.data() : nullptr;
    const u64 wait_zero = 0; // dummy for binary wait
    const VkTimelineSemaphoreSubmitInfo timeline_si{
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreValueCount = num_wait_semaphores,
        .pWaitSemaphoreValues    = num_wait_semaphores ? &wait_zero : nullptr,
        .signalSemaphoreValueCount = num_signal_semaphores,
        .pSignalSemaphoreValues = signal_values.data(),
    };
    const VkSubmitInfo submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = &timeline_si,
        .waitSemaphoreCount = num_wait_semaphores,
        .pWaitSemaphores = p_wait_sems,
        .pWaitDstStageMask = p_wait_masks,
        .commandBufferCount = static_cast<u32>(cmdbuffers.size()),
        .pCommandBuffers = cmdbuffers.data(),
        .signalSemaphoreCount = num_signal_semaphores,
        .pSignalSemaphores = p_signal_sems,
    };

    return device.GetGraphicsQueue().Submit(submit_info);
}

VkResult MasterSemaphore::SubmitQueueFence(vk::CommandBuffer& cmdbuf,
                                           vk::CommandBuffer& upload_cmdbuf,
                                           VkSemaphore signal_semaphore, VkSemaphore wait_semaphore,
                                           u64 host_tick) {
    if (device.HasSynchronization2()) {
        const std::array<VkCommandBufferSubmitInfo, 2> cmdbuffer_infos{{
            {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext = nullptr,
                .commandBuffer = *upload_cmdbuf,
                .deviceMask = 0,
            },
            {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext = nullptr,
                .commandBuffer = *cmdbuf,
                .deviceMask = 0,
            },
        }};

        const u32 num_signal_semaphores = signal_semaphore ? 1 : 0;
        const VkSemaphoreSubmitInfo signal_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = signal_semaphore,
            .value = 0,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .deviceIndex = 0,
        };

        const u32 num_wait_semaphores = wait_semaphore ? 1 : 0;
        const VkSemaphoreSubmitInfo wait_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .semaphore = wait_semaphore,
            .value = 0,
            .stageMask = static_cast<VkPipelineStageFlags2>(wait_stage_mask),
            .deviceIndex = 0,
        };

        const VkSubmitInfo2 submit_info2{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .pNext = nullptr,
            .flags = 0,
            .waitSemaphoreInfoCount = num_wait_semaphores,
            .pWaitSemaphoreInfos = num_wait_semaphores ? &wait_info : nullptr,
            .commandBufferInfoCount = static_cast<u32>(cmdbuffer_infos.size()),
            .pCommandBufferInfos = cmdbuffer_infos.data(),
            .signalSemaphoreInfoCount = num_signal_semaphores,
            .pSignalSemaphoreInfos = num_signal_semaphores ? &signal_info : nullptr,
        };

        auto fence = GetFreeFence();
        auto result = device.GetGraphicsQueue().Submit2(submit_info2, *fence);

        if (result == VK_SUCCESS) {
            std::scoped_lock lock{wait_mutex};
            wait_queue.emplace(host_tick, std::move(fence));
            wait_cv.notify_one();
        }

        return result;
    }

    const u32 num_signal_semaphores = signal_semaphore ? 1 : 0;
    const u32 num_wait_semaphores = wait_semaphore ? 1 : 0;

    const VkSemaphore* p_wait_sems =
            (num_wait_semaphores > 0) ? &wait_semaphore : nullptr;
    const VkPipelineStageFlags* p_wait_masks =
        (num_wait_semaphores > 0) ? &wait_stage_mask : nullptr;
    const VkSemaphore* p_signal_sems =
        (num_signal_semaphores > 0) ? &signal_semaphore : nullptr;
    const std::array cmdbuffers{*upload_cmdbuf, *cmdbuf};

    const VkSubmitInfo submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = num_wait_semaphores,
        .pWaitSemaphores = p_wait_sems,
        .pWaitDstStageMask = p_wait_masks,
        .commandBufferCount = static_cast<u32>(cmdbuffers.size()),
        .pCommandBuffers = cmdbuffers.data(),
        .signalSemaphoreCount = num_signal_semaphores,
        .pSignalSemaphores = p_signal_sems,
    };

    auto fence = GetFreeFence();
    auto result = device.GetGraphicsQueue().Submit(submit_info, *fence);

    if (result == VK_SUCCESS) {
        std::scoped_lock lock{wait_mutex};
        wait_queue.emplace(host_tick, std::move(fence));
        wait_cv.notify_one();
    }

    return result;
}

void MasterSemaphore::WaitThread(std::stop_token token) {
    while (!token.stop_requested()) {
        u64 host_tick;
        vk::Fence fence;
        {
            std::unique_lock lock{wait_mutex};
            wait_cv.wait(lock, token, [this] { return !wait_queue.empty(); });
            if (token.stop_requested()) {
                return;
            }
            std::tie(host_tick, fence) = std::move(wait_queue.front());
            wait_queue.pop();
        }

        fence.Wait();
        fence.Reset();

        {
            std::scoped_lock lock{free_mutex};
            free_queue.push_front(std::move(fence));
            gpu_tick.store(host_tick, std::memory_order_release);
        }
        gpu_tick.notify_one();
    }
}

vk::Fence MasterSemaphore::GetFreeFence() {
    std::scoped_lock lock{free_mutex};
    if (free_queue.empty()) {
        static constexpr VkFenceCreateInfo fence_ci{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0};
        return device.GetLogical().CreateFence(fence_ci);
    }

    auto fence = std::move(free_queue.back());
    free_queue.pop_back();
    return fence;
}

} // namespace Vulkan
