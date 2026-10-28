#pragma once

// Per-platform surface creators behind createSurface() (surface.cpp), which has checked the window
// system, the instance and the function already. Only the build's own window system is declared.

#include <expected>

#include <vulkan/vulkan_core.h>

#include "dvv/platform/native_window.h"

namespace dvv::platform::detail {

// VK_ERROR_INITIALIZATION_FAILED for null handles, VK_ERROR_EXTENSION_NOT_PRESENT when the instance
// does not offer the creation function, else the driver's result.
#if defined(_WIN32)
[[nodiscard]] std::expected<VkSurfaceKHR, VkResult> createWin32Surface(
    VkInstance instance, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
    const Win32Window& window) noexcept;
#else
[[nodiscard]] std::expected<VkSurfaceKHR, VkResult> createXlibSurface(
    VkInstance instance, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
    const XlibWindow& window) noexcept;
#endif

}  // namespace dvv::platform::detail
