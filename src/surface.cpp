#include "dvv/platform/surface.h"

#include <array>
#include <variant>

#include "surface_detail.h"

namespace dvv::platform {

namespace {

constexpr std::array<const char*, 2> WIN32_EXTENSIONS{VK_KHR_SURFACE_EXTENSION_NAME,
                                                      "VK_KHR_win32_surface"};
constexpr std::array<const char*, 2> XLIB_EXTENSIONS{VK_KHR_SURFACE_EXTENSION_NAME,
                                                     "VK_KHR_xlib_surface"};

}  // namespace

std::span<const char* const> surfaceInstanceExtensions(WindowSystem system) noexcept {
    switch (system) {
        case WindowSystem::Win32: return WIN32_EXTENSIONS;
        case WindowSystem::Xlib: return XLIB_EXTENSIONS;
    }
    return {};
}

std::expected<VkSurfaceKHR, VkResult> createSurface(
    VkInstance instance, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
    const NativeWindow& window) noexcept {
    const auto system = windowSystemOf(window);
    if (!system) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }
    if (*system != defaultWindowSystem()) {
        return std::unexpected(VK_ERROR_EXTENSION_NOT_PRESENT);  // this build lacks it
    }
    if (instance == VK_NULL_HANDLE || get_instance_proc_addr == nullptr) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }
#if defined(_WIN32)
    return detail::createWin32Surface(instance, get_instance_proc_addr,
                                      std::get<Win32Window>(window));
#else
    return detail::createXlibSurface(instance, get_instance_proc_addr,
                                     std::get<XlibWindow>(window));
#endif
}

}  // namespace dvv::platform
