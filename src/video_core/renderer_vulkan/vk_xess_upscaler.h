// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Intel XeSS Super Resolution through Intel's runtime (vulkan_common/xess_runtime.h). Same inputs
// as the Ocarina FSR 3.1 path, which takes over when XeSS is off, missing or fails.

#pragma once

#include <array>
#include <atomic>
#include <optional>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

struct _xess_context_handle_t;

namespace Vulkan {

class Device;
class MemoryAllocator;
class Scheduler;

namespace XessRuntime {
struct Api;
}

class XessUpscaler {
public:
    struct Image {
        VkImage image;
        VkImageView view;                  ///< A 2D view of one level and layer, one aspect.
        VkFormat format;
        VkImageSubresourceRange range;     ///< The view's level, layer and aspect.
        VkImageAspectFlags barrier_aspect; ///< Every aspect of the image's format.
    };

    struct Inputs {
        Image color;          ///< Tonemapped (LDR) colour at the render size.
        Image depth;          ///< Standard (not reversed) device depth at the render size.
        Image motion_vectors; ///< Motion to the previous frame, in render pixels.
        std::optional<Image> responsive; ///< 0-1 at the render size; 1 ignores the history.
        VkExtent2D render_size;
        VkExtent2D upscale_size;
        bool reset;
        std::array<f32, 2> jitter{}; ///< Offset the scene was drawn with, in render pixels.
    };

    /// Whether XeSS is turned on and the device was created with what it needs.
    [[nodiscard]] static bool IsWanted(const Device& device);

    /// Throws vk::Exception when Intel's runtime isn't loaded.
    explicit XessUpscaler(const Device& device, Scheduler& scheduler,
                          MemoryAllocator& memory_allocator);
    ~XessUpscaler();

    /// Records one evaluation into the output image (RGBA16F at the upscale size, GENERAL
    /// layout). False when XeSS can't run (the caller then uses FSR from now on).
    bool Upscale(const Inputs& inputs);

    [[nodiscard]] bool Failed() const noexcept {
        return failed.load();
    }
    [[nodiscard]] VkImage OutputImage() const noexcept {
        return *output_image;
    }
    [[nodiscard]] VkImageView OutputView() const noexcept {
        return *output_view;
    }

private:
    /// (Re)initializes XeSS for these sizes on the Vulkan worker thread and waits for it.
    bool Initialize(VkExtent2D render_size, VkExtent2D upscale_size);
    /// Worker thread: every XeSS call runs there, since XeSS isn't thread safe.
    bool InitializeOnWorker(VkExtent2D render_size, VkExtent2D upscale_size);
    void CreateOutput(VkExtent2D upscale_size);
    void CreateBlankMask(VkExtent2D render_size);

    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& memory_allocator;
    const XessRuntime::Api* api{};
    _xess_context_handle_t* context{}; ///< Used on the worker thread only.
    std::atomic<bool> initialized{};   ///< Set on the worker, read after Scheduler::Finish.
    VkExtent2D render_extent{};
    VkExtent2D upscale_extent{};
    std::array<f32, 2> jitter_scale{1.0f, 1.0f};
    std::array<f32, 2> motion_scale{1.0f, 1.0f};
    vk::Image output_image;
    vk::ImageView output_view;
    vk::Image blank_mask; ///< R16F zeros at the render size, for frames without a mask
    vk::ImageView blank_mask_view;
    std::atomic<bool> failed{};
};

} // namespace Vulkan
