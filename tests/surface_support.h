#pragma once

#include <span>

#include <vulkan/vulkan_core.h>

#include "dvv/base/error.h"

namespace dvv::platform::test {

// A Vulkan instance for the surface tests, made through the system's Vulkan loader
// ("vulkan-1.dll" with LibrarySearch::SystemDirectory, or "libvulkan.so.1"). The loader stays
// loaded for the rest of the process (keepLoaded()), as in dvv-gpu.
class VulkanInstance {
public:
    // Errc::Unsupported when the loader is missing, or vkCreateInstance returns
    // VK_ERROR_INCOMPATIBLE_DRIVER or VK_ERROR_EXTENSION_NOT_PRESENT; Errc::Internal otherwise.
    [[nodiscard]] static Result<VulkanInstance> create(std::span<const char* const> extensions);
    VulkanInstance(VulkanInstance&& other) noexcept;
    VulkanInstance& operator=(VulkanInstance&& other) noexcept;
    VulkanInstance(const VulkanInstance&) = delete;
    VulkanInstance& operator=(const VulkanInstance&) = delete;
    ~VulkanInstance();  // vkDestroyInstance

    [[nodiscard]] VkInstance handle() const noexcept { return instance_; }
    [[nodiscard]] PFN_vkGetInstanceProcAddr getInstanceProcAddr() const noexcept {
        return get_instance_proc_addr_;
    }
    void destroySurface(VkSurfaceKHR surface) const noexcept;  // vkDestroySurfaceKHR

private:
    VulkanInstance(VkInstance instance, PFN_vkGetInstanceProcAddr get_instance_proc_addr) noexcept;
    void release() noexcept;

    VkInstance instance_ = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr_ = nullptr;
    PFN_vkDestroyInstance destroy_instance_ = nullptr;
    PFN_vkDestroySurfaceKHR destroy_surface_ = nullptr;
};

}  // namespace dvv::platform::test
