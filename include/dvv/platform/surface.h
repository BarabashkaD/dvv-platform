#pragma once

// Vulkan surfaces for native windows (spec §10.4). Vulkan C types only, without prototypes
// (VK_NO_PROTOTYPES): every function comes through vkGetInstanceProcAddr. Results stay VkResult;
// dvv-gpu converts them to dvv::Error in one place.

#include <expected>
#include <span>

#include <vulkan/vulkan_core.h>

#include "dvv/platform/native_window.h"

namespace dvv::platform {

// {"VK_KHR_surface", "VK_KHR_win32_surface"} or {"VK_KHR_surface", "VK_KHR_xlib_surface"}.
[[nodiscard]] std::span<const char* const> surfaceInstanceExtensions(WindowSystem system) noexcept;
// Through vkGetInstanceProcAddr(instance, "vkCreate...SurfaceKHR"). Errors:
// VK_ERROR_EXTENSION_NOT_PRESENT (function unavailable: extension not enabled, or a window system
// this build lacks), VK_ERROR_INITIALIZATION_FAILED (monostate or null handles), else the driver's.
// The caller destroys the surface with vkDestroySurfaceKHR before the window.
[[nodiscard]] std::expected<VkSurfaceKHR, VkResult> createSurface(
    VkInstance instance, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
    const NativeWindow& window) noexcept;

}  // namespace dvv::platform
