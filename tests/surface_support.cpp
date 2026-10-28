#include "surface_support.h"

#include <cstdint>
#include <format>
#include <utility>

#include "dvv/platform/dynamic_library.h"

namespace dvv::platform::test {

namespace {

#if defined(_WIN32)
constexpr const char* LOADER_NAME = "vulkan-1.dll";
#else
constexpr const char* LOADER_NAME = "libvulkan.so.1";
#endif

Result<PFN_vkGetInstanceProcAddr> loadLoader() {
    auto library = DynamicLibrary::open(LOADER_NAME, LibrarySearch::SystemDirectory);
    if (!library) {
        return std::unexpected(std::move(library).error());
    }
    const auto get_instance_proc_addr =
        library->function<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    if (get_instance_proc_addr == nullptr) {
        return makeError(Errc::Unsupported,
                         std::format("'{}' has no vkGetInstanceProcAddr", LOADER_NAME));
    }
    library->keepLoaded();
    return get_instance_proc_addr;
}

}  // namespace

Result<VulkanInstance> VulkanInstance::create(std::span<const char* const> extensions) {
    DVV_TRY(const auto get_instance_proc_addr, loadLoader());
    const auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(
        get_instance_proc_addr(VK_NULL_HANDLE, "vkCreateInstance"));
    if (create_instance == nullptr) {
        return makeError(Errc::Internal, "the Vulkan loader has no vkCreateInstance");
    }
    const VkApplicationInfo application{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                        .pNext = nullptr,
                                        .pApplicationName = "dvv_platform_surface_tests",
                                        .applicationVersion = 0,
                                        .pEngineName = "dvv",
                                        .engineVersion = 0,
                                        .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .pApplicationInfo = &application,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data()};
    VkInstance instance = VK_NULL_HANDLE;
    const VkResult result = create_instance(&info, nullptr, &instance);
    if (result == VK_ERROR_INCOMPATIBLE_DRIVER || result == VK_ERROR_EXTENSION_NOT_PRESENT) {
        return makeError(Errc::Unsupported,
                         std::format("no Vulkan driver with the surface extensions "
                                     "(vkCreateInstance returned {})",
                                     static_cast<int>(result)));
    }
    if (result != VK_SUCCESS) {
        return makeError(Errc::Internal,
                         std::format("vkCreateInstance returned {}", static_cast<int>(result)));
    }
    return VulkanInstance(instance, get_instance_proc_addr);
}

VulkanInstance::VulkanInstance(VkInstance instance,
                               PFN_vkGetInstanceProcAddr get_instance_proc_addr) noexcept
    : instance_(instance),
      get_instance_proc_addr_(get_instance_proc_addr),
      destroy_instance_(reinterpret_cast<PFN_vkDestroyInstance>(
          get_instance_proc_addr(instance, "vkDestroyInstance"))),
      // nullptr when VK_KHR_surface is not enabled; then no surface can exist.
      destroy_surface_(reinterpret_cast<PFN_vkDestroySurfaceKHR>(
          get_instance_proc_addr(instance, "vkDestroySurfaceKHR"))) {}

VulkanInstance::VulkanInstance(VulkanInstance&& other) noexcept
    : instance_(std::exchange(other.instance_, VK_NULL_HANDLE)),
      get_instance_proc_addr_(std::exchange(other.get_instance_proc_addr_, nullptr)),
      destroy_instance_(std::exchange(other.destroy_instance_, nullptr)),
      destroy_surface_(std::exchange(other.destroy_surface_, nullptr)) {}

VulkanInstance& VulkanInstance::operator=(VulkanInstance&& other) noexcept {
    if (this != &other) {
        release();
        instance_ = std::exchange(other.instance_, VK_NULL_HANDLE);
        get_instance_proc_addr_ = std::exchange(other.get_instance_proc_addr_, nullptr);
        destroy_instance_ = std::exchange(other.destroy_instance_, nullptr);
        destroy_surface_ = std::exchange(other.destroy_surface_, nullptr);
    }
    return *this;
}

VulkanInstance::~VulkanInstance() {
    release();
}

void VulkanInstance::destroySurface(VkSurfaceKHR surface) const noexcept {
    if (surface != VK_NULL_HANDLE && destroy_surface_ != nullptr) {
        destroy_surface_(instance_, surface, nullptr);
    }
}

void VulkanInstance::release() noexcept {
    if (instance_ != VK_NULL_HANDLE && destroy_instance_ != nullptr) {
        destroy_instance_(instance_, nullptr);
    }
    instance_ = VK_NULL_HANDLE;
}

}  // namespace dvv::platform::test
