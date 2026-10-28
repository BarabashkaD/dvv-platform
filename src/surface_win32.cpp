#include <windows.h>
// vulkan_win32.h needs the Windows types (HINSTANCE, HWND, SECURITY_ATTRIBUTES).
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_win32.h>

#include "surface_detail.h"

namespace dvv::platform::detail {

std::expected<VkSurfaceKHR, VkResult> createWin32Surface(
    VkInstance instance, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
    const Win32Window& window) noexcept {
    if (window.hinstance == nullptr || window.hwnd == nullptr) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }
    const auto create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
        get_instance_proc_addr(instance, "vkCreateWin32SurfaceKHR"));
    if (create == nullptr) {
        return std::unexpected(VK_ERROR_EXTENSION_NOT_PRESENT);
    }
    const VkWin32SurfaceCreateInfoKHR info{.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                           .pNext = nullptr,
                                           .flags = 0,
                                           .hinstance = static_cast<HINSTANCE>(window.hinstance),
                                           .hwnd = static_cast<HWND>(window.hwnd)};
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const VkResult result = create(instance, &info, nullptr, &surface);
    if (result != VK_SUCCESS) {
        return std::unexpected(result);
    }
    return surface;
}

}  // namespace dvv::platform::detail
