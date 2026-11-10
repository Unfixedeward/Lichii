// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Intel XeSS Super Resolution. XeSS takes image views: the inputs in SHADER_READ_ONLY_OPTIMAL,
// the output in GENERAL, and records compute work into our command buffer. It isn't thread safe,
// so every call into it, creation and destruction included, runs on the Vulkan worker thread.
// Initialization may upload XeSS's network through the queue, so it holds the submit lock.

#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>
#include <utility>

#include <boost/container/static_vector.hpp>

#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_xess_upscaler.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/xess_runtime.h"

namespace Vulkan {
namespace {
constexpr VkFormat OUTPUT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat MASK_FORMAT = VK_FORMAT_R16_SFLOAT;
constexpr VkImageUsageFlags OUTPUT_USAGE = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr VkImageSubresourceRange COLOR_RANGE{
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

struct Preset {
    xess_quality_settings_t setting;
    std::string_view name;
};
constexpr std::array PRESETS{
    Preset{XESS_QUALITY_SETTING_AA, "Native AA"},
    Preset{XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS, "Ultra Quality Plus"},
    Preset{XESS_QUALITY_SETTING_ULTRA_QUALITY, "Ultra Quality"},
    Preset{XESS_QUALITY_SETTING_QUALITY, "Quality"},
    Preset{XESS_QUALITY_SETTING_BALANCED, "Balanced"},
    Preset{XESS_QUALITY_SETTING_PERFORMANCE, "Performance"},
    Preset{XESS_QUALITY_SETTING_ULTRA_PERFORMANCE, "Ultra Performance"},
};

VkImageMemoryBarrier Barrier(VkImage image, VkImageSubresourceRange range, VkAccessFlags src_access,
                             VkAccessFlags dst_access, VkImageLayout old_layout,
                             VkImageLayout new_layout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = range,
    };
}

VkImageMemoryBarrier Barrier(const XessUpscaler::Image& image, VkAccessFlags src_access,
                             VkAccessFlags dst_access, VkImageLayout old_layout,
                             VkImageLayout new_layout) {
    VkImageSubresourceRange range = image.range;
    range.aspectMask = image.barrier_aspect;
    return Barrier(image.image, range, src_access, dst_access, old_layout, new_layout);
}

xess_vk_image_view_info ViewInfo(const XessUpscaler::Image& image, VkExtent2D size) {
    return xess_vk_image_view_info{
        .imageView = image.view,
        .image = image.image,
        .subresourceRange = image.range,
        .format = image.format,
        .width = size.width,
        .height = size.height,
    };
}

void Message(const char* message, xess_logging_level_t level) {
    if (level >= XESS_LOGGING_LEVEL_ERROR) {
        LOG_ERROR(Render_Vulkan, "XeSS: {}", message);
    } else {
        LOG_WARNING(Render_Vulkan, "XeSS: {}", message);
    }
}

/// EDEN_NX2_XESS_JITTER / EDEN_NX2_XESS_MOTION: flipx, flipy or flipxy multiply that axis by -1.
std::array<f32, 2> AxisScale(const char* variable) {
    const char* const value = std::getenv(variable);
    const std::string_view text = value ? value : "";
    const bool x = text == "flipx" || text == "flipxy";
    const bool y = text == "flipy" || text == "flipxy";
    return {x ? -1.0f : 1.0f, y ? -1.0f : 1.0f};
}

bool Contains(xess_2d_t min, xess_2d_t max, VkExtent2D size) {
    return size.width >= min.x && size.width <= max.x && size.height >= min.y &&
           size.height <= max.y;
}
} // Anonymous namespace

bool XessUpscaler::IsWanted(const Device& device) {
    return device.IsXessReady() && XessRuntime::Wanted();
}

XessUpscaler::XessUpscaler(const Device& device_, Scheduler& scheduler_,
                           MemoryAllocator& memory_allocator_)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_},
      api{XessRuntime::Get()}, jitter_scale{AxisScale("EDEN_NX2_XESS_JITTER")},
      motion_scale{AxisScale("EDEN_NX2_XESS_MOTION")} {
    if (api == nullptr) {
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }
    if (jitter_scale != std::array{1.0f, 1.0f} || motion_scale != std::array{1.0f, 1.0f}) {
        LOG_INFO(Render_Vulkan, "Intel XeSS: jitter scale ({}, {}), motion scale ({}, {})",
                 jitter_scale[0], jitter_scale[1], motion_scale[0], motion_scale[1]);
    }
}

XessUpscaler::~XessUpscaler() {
    if (output_image) {
        // Recorded evaluations may still use the context; it goes away on the worker too.
        scheduler.Record([this](vk::CommandBuffer) {
            if (context != nullptr) {
                api->destroy_context(context);
                context = nullptr;
            }
        });
        scheduler.Finish();
    }
}

bool XessUpscaler::InitializeOnWorker(VkExtent2D render_size, VkExtent2D upscale_size) {
    std::scoped_lock lock{scheduler.submit_mutex};
    if (context == nullptr) {
        const xess_result_t created =
            api->vk_create_context(device.GetInstance(), device.GetPhysical(),
                                   *device.GetLogical(), &context);
        if (created != XESS_RESULT_SUCCESS) {
            LOG_ERROR(Render_Vulkan, "Intel XeSS: xessVKCreateContext failed ({}); using FSR",
                      XessRuntime::ResultName(created));
            context = nullptr;
            return false;
        }
        api->set_logging_callback(context, XESS_LOGGING_LEVEL_WARNING, Message);
        if (api->is_optimal_driver(context) == XESS_RESULT_WARNING_OLD_DRIVER) {
            LOG_WARNING(Render_Vulkan,
                        "Intel XeSS: the GPU driver is older than XeSS wants; update it for the "
                        "best quality and speed");
        }
        xess_version_t xefx{};
        if (api->get_xefx_version(context, &xefx) == XESS_RESULT_SUCCESS && xefx.major != 0) {
            LOG_INFO(Render_Vulkan, "Intel XeSS: Intel XeFX {}.{}.{} (XMX path)", xefx.major,
                     xefx.minor, xefx.patch);
        }
    }

    // The preset whose input range holds the render size, the closest optimal size first.
    const xess_2d_t output{upscale_size.width, upscale_size.height};
    const Preset* chosen = nullptr;
    u64 best = std::numeric_limits<u64>::max();
    for (const Preset& preset : PRESETS) {
        xess_2d_t optimal{};
        xess_2d_t min{};
        xess_2d_t max{};
        if (api->get_optimal_input_resolution(context, &output, preset.setting, &optimal, &min,
                                              &max) != XESS_RESULT_SUCCESS ||
            !Contains(min, max, render_size)) {
            continue;
        }
        const u64 distance =
            static_cast<u64>(std::abs(static_cast<s64>(optimal.x) - render_size.width)) +
            static_cast<u64>(std::abs(static_cast<s64>(optimal.y) - render_size.height));
        if (distance < best) {
            best = distance;
            chosen = &preset;
        }
    }
    if (chosen == nullptr) {
        LOG_ERROR(Render_Vulkan, "Intel XeSS: no preset takes {}x{} -> {}x{}; using FSR",
                  render_size.width, render_size.height, upscale_size.width,
                  upscale_size.height);
        return false;
    }

    // Tonemapped colour, standard depth, render-size motion without the jitter, a mask.
    const xess_vk_init_params_t params{
        .outputResolution = output,
        .qualitySetting = chosen->setting,
        .initFlags = XESS_INIT_FLAG_LDR_INPUT_COLOR | XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK,
        .creationNodeMask = 0,
        .visibleNodeMask = 0,
        .tempBufferHeap = VK_NULL_HANDLE,
        .bufferHeapOffset = 0,
        .tempTextureHeap = VK_NULL_HANDLE,
        .textureHeapOffset = 0,
        .pipelineCache = VK_NULL_HANDLE,
    };
    const xess_result_t result = api->vk_init(context, &params);
    if (result != XESS_RESULT_SUCCESS) {
        LOG_ERROR(Render_Vulkan, "Intel XeSS: xessVKInit {}x{} -> {}x{} ({}) failed ({}); using FSR",
                  render_size.width, render_size.height, upscale_size.width,
                  upscale_size.height, chosen->name, XessRuntime::ResultName(result));
        return false;
    }
    api->set_jitter_scale(context, jitter_scale[0], jitter_scale[1]);
    api->set_velocity_scale(context, motion_scale[0], motion_scale[1]);
    LOG_INFO(Render_Vulkan, "Intel XeSS: {}x{} -> {}x{} ({} preset)", render_size.width,
             render_size.height, upscale_size.width, upscale_size.height, chosen->name);
    return true;
}

bool XessUpscaler::Initialize(VkExtent2D render_size, VkExtent2D upscale_size) {
    initialized = false;
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, render_size, upscale_size](vk::CommandBuffer) {
        initialized = InitializeOnWorker(render_size, upscale_size);
    });
    scheduler.Finish();
    return initialized;
}

void XessUpscaler::CreateOutput(VkExtent2D upscale_size) {
    output_image = memory_allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = OUTPUT_FORMAT,
        .extent = {upscale_size.width, upscale_size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = OUTPUT_USAGE,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    output_view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = *output_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = OUTPUT_FORMAT,
        .components{},
        .subresourceRange = COLOR_RANGE,
    });
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([image = *output_image](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                               Barrier(image, COLOR_RANGE, 0,
                                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                       VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL));
    });
}

void XessUpscaler::CreateBlankMask(VkExtent2D render_size) {
    blank_mask = memory_allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = MASK_FORMAT,
        .extent = {render_size.width, render_size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    blank_mask_view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = *blank_mask,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = MASK_FORMAT,
        .components{},
        .subresourceRange = COLOR_RANGE,
    });
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([image = *blank_mask](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0,
                               Barrier(image, COLOR_RANGE, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                       VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
        cmdbuf.ClearColorImage(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VkClearColorValue{},
                               COLOR_RANGE);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                               Barrier(image, COLOR_RANGE, VK_ACCESS_TRANSFER_WRITE_BIT,
                                       VK_ACCESS_SHADER_READ_BIT,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    });
}

bool XessUpscaler::Upscale(const Inputs& in) {
    if (failed.load()) {
        return false;
    }
    const bool new_render = !blank_mask || in.render_size.width != render_extent.width ||
                            in.render_size.height != render_extent.height;
    const bool new_upscale = !output_image || in.upscale_size.width != upscale_extent.width ||
                             in.upscale_size.height != upscale_extent.height;
    const bool new_sizes = new_render || new_upscale;
    if (new_sizes) {
        if (output_image) {
            scheduler.Finish(); // XeSS re-initializes only once its work has completed
        }
        if (new_upscale) {
            CreateOutput(in.upscale_size);
        }
        if (new_render) {
            CreateBlankMask(in.render_size);
        }
        render_extent = in.render_size;
        upscale_extent = in.upscale_size;
        if (!Initialize(in.render_size, in.upscale_size)) {
            failed = true;
            return false;
        }
    }

    const Image output{*output_image, *output_view, OUTPUT_FORMAT, COLOR_RANGE,
                       VK_IMAGE_ASPECT_COLOR_BIT};
    const Image mask = in.responsive.value_or(
        Image{*blank_mask, *blank_mask_view, MASK_FORMAT, COLOR_RANGE, VK_IMAGE_ASPECT_COLOR_BIT});
    xess_vk_execute_params_t params{};
    params.colorTexture = ViewInfo(in.color, in.render_size);
    params.velocityTexture = ViewInfo(in.motion_vectors, in.render_size);
    params.depthTexture = ViewInfo(in.depth, in.render_size);
    params.responsivePixelMaskTexture = ViewInfo(mask, in.render_size);
    params.outputTexture = ViewInfo(output, in.upscale_size);
    params.jitterOffsetX = in.jitter[0];
    params.jitterOffsetY = in.jitter[1];
    params.exposureScale = 1.0f;
    params.resetHistory = (in.reset || new_sizes) ? 1 : 0;
    params.inputWidth = in.render_size.width;
    params.inputHeight = in.render_size.height;

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, in, params](vk::CommandBuffer cmdbuf) {
        constexpr VkAccessFlags WRITES =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        // The inputs stay in GENERAL otherwise; XeSS reads them as sampled images.
        boost::container::static_vector<VkImageMemoryBarrier, 5> to_read{
            Barrier(in.color, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.depth, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.motion_vectors, WRITES, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(params.outputTexture.image, COLOR_RANGE, VK_ACCESS_SHADER_READ_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL),
        };
        if (in.responsive) {
            to_read.push_back(Barrier(*in.responsive, WRITES, VK_ACCESS_SHADER_READ_BIT,
                                      VK_IMAGE_LAYOUT_GENERAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
        }
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, to_read);

        const xess_result_t result = api->vk_execute(context, *cmdbuf, &params);
        if (result != XESS_RESULT_SUCCESS && !failed.exchange(true)) {
            LOG_ERROR(Render_Vulkan, "Intel XeSS: xessVKExecute failed ({}); using FSR",
                      XessRuntime::ResultName(result));
        }

        boost::container::static_vector<VkImageMemoryBarrier, 5> back{
            Barrier(in.color, VK_ACCESS_SHADER_READ_BIT, 0,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.depth, VK_ACCESS_SHADER_READ_BIT, 0,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.motion_vectors, VK_ACCESS_SHADER_READ_BIT, 0,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(params.outputTexture.image, COLOR_RANGE, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL),
        };
        if (in.responsive) {
            back.push_back(Barrier(*in.responsive, VK_ACCESS_SHADER_READ_BIT, 0,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   VK_IMAGE_LAYOUT_GENERAL));
        }
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, {}, {}, back);
    });
    return true;
}

} // namespace Vulkan
