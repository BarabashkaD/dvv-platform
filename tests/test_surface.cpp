#include <doctest/doctest.h>

#include <utility>
#include <variant>

#include "dvv/platform/native_window.h"
#include "dvv/platform/surface.h"
#include "dvv/platform/testing/test_window.h"
#include "surface_support.h"

namespace {

using dvv::platform::NativeWindow;
using dvv::platform::test::VulkanInstance;
using dvv::platform::testing::TestWindow;

VulkanInstance surfaceInstance() {
    auto instance = VulkanInstance::create(
        dvv::platform::surfaceInstanceExtensions(dvv::platform::defaultWindowSystem()));
    REQUIRE(instance.has_value());
    return std::move(*instance);
}

TestWindow testWindow() {
    auto window = TestWindow::create({.width = 320, .height = 240});
    REQUIRE(window.has_value());
    return std::move(*window);
}

}  // namespace

TEST_CASE("a surface is created for a test window and destroyed") {
    const VulkanInstance instance = surfaceInstance();
    const TestWindow window = testWindow();
    const auto surface = dvv::platform::createSurface(
        instance.handle(), instance.getInstanceProcAddr(), window.native());
    REQUIRE(surface.has_value());
    CHECK(*surface != VK_NULL_HANDLE);
    instance.destroySurface(*surface);  // before the window goes
}

TEST_CASE("a window with null handles is rejected") {
    const VulkanInstance instance = surfaceInstance();
    const TestWindow window = testWindow();
    const NativeWindow native = window.native();
#if defined(_WIN32)
    const auto& real = std::get<dvv::platform::Win32Window>(native);
    const NativeWindow no_hwnd{
        dvv::platform::Win32Window{.hinstance = real.hinstance, .hwnd = nullptr}};
    const auto surface =
        dvv::platform::createSurface(instance.handle(), instance.getInstanceProcAddr(), no_hwnd);
    REQUIRE_FALSE(surface.has_value());
    CHECK(surface.error() == VK_ERROR_INITIALIZATION_FAILED);
#else
    const auto& real = std::get<dvv::platform::XlibWindow>(native);
    const NativeWindow no_window{dvv::platform::XlibWindow{.display = real.display, .window = 0}};
    const auto without_window =
        dvv::platform::createSurface(instance.handle(), instance.getInstanceProcAddr(), no_window);
    REQUIRE_FALSE(without_window.has_value());
    CHECK(without_window.error() == VK_ERROR_INITIALIZATION_FAILED);
    const NativeWindow no_display{
        dvv::platform::XlibWindow{.display = nullptr, .window = real.window}};
    const auto without_display =
        dvv::platform::createSurface(instance.handle(), instance.getInstanceProcAddr(), no_display);
    REQUIRE_FALSE(without_display.has_value());
    CHECK(without_display.error() == VK_ERROR_INITIALIZATION_FAILED);
#endif
}

TEST_CASE("an instance without the surface extensions cannot create surfaces") {
    auto created = VulkanInstance::create({});
    REQUIRE(created.has_value());
    const VulkanInstance& instance = *created;
    const TestWindow window = testWindow();
    const auto surface = dvv::platform::createSurface(
        instance.handle(), instance.getInstanceProcAddr(), window.native());
    REQUIRE_FALSE(surface.has_value());
    CHECK(surface.error() == VK_ERROR_EXTENSION_NOT_PRESENT);
}
