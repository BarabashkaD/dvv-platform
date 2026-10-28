#include <doctest/doctest.h>

#include <optional>
#include <string_view>

#include "dvv/platform/native_window.h"
#include "dvv/platform/surface.h"

namespace {

using dvv::platform::NativeWindow;
using dvv::platform::PixelSize;
using dvv::platform::Win32Window;
using dvv::platform::WindowSystem;
using dvv::platform::XlibWindow;

}  // namespace

TEST_CASE("window systems have names and a default") {
    CHECK(dvv::platform::windowSystemName(WindowSystem::Win32) == "Win32");
    CHECK(dvv::platform::windowSystemName(WindowSystem::Xlib) == "Xlib");
#if defined(_WIN32)
    CHECK(dvv::platform::defaultWindowSystem() == WindowSystem::Win32);
#else
    CHECK(dvv::platform::defaultWindowSystem() == WindowSystem::Xlib);
#endif
}

TEST_CASE("windowSystemOf names the window's alternative") {
    CHECK(dvv::platform::windowSystemOf(NativeWindow{}) == std::nullopt);
    CHECK(dvv::platform::windowSystemOf(NativeWindow{Win32Window{}}) == WindowSystem::Win32);
    CHECK(dvv::platform::windowSystemOf(NativeWindow{XlibWindow{}}) == WindowSystem::Xlib);
}

TEST_CASE("a pixel size is empty when a side is 0") {
    CHECK(PixelSize{}.empty());
    CHECK(PixelSize{.width = 1, .height = 0}.empty());
    CHECK(PixelSize{.width = 0, .height = 1}.empty());
    CHECK_FALSE(PixelSize{.width = 1, .height = 1}.empty());
    CHECK(PixelSize{.width = 2, .height = 3} == PixelSize{.width = 2, .height = 3});
    CHECK(PixelSize{.width = 2, .height = 3} != PixelSize{.width = 3, .height = 2});
}

TEST_CASE("each window system needs VK_KHR_surface and its own extension") {
    const auto win32 = dvv::platform::surfaceInstanceExtensions(WindowSystem::Win32);
    REQUIRE(win32.size() == 2);
    CHECK(std::string_view(win32[0]) == "VK_KHR_surface");
    CHECK(std::string_view(win32[1]) == "VK_KHR_win32_surface");
    const auto xlib = dvv::platform::surfaceInstanceExtensions(WindowSystem::Xlib);
    REQUIRE(xlib.size() == 2);
    CHECK(std::string_view(xlib[0]) == "VK_KHR_surface");
    CHECK(std::string_view(xlib[1]) == "VK_KHR_xlib_surface");
}

TEST_CASE("createSurface rejects an empty window and a window system this build lacks") {
    const auto empty = dvv::platform::createSurface(VK_NULL_HANDLE, nullptr, NativeWindow{});
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error() == VK_ERROR_INITIALIZATION_FAILED);

    // Non-null fake handles: only the window system decides, the handles are never used.
#if defined(_WIN32)
    const NativeWindow other{XlibWindow{.display = reinterpret_cast<void*>(1), .window = 1}};
    const NativeWindow own{
        Win32Window{.hinstance = reinterpret_cast<void*>(1), .hwnd = reinterpret_cast<void*>(1)}};
#else
    const NativeWindow other{
        Win32Window{.hinstance = reinterpret_cast<void*>(1), .hwnd = reinterpret_cast<void*>(1)}};
    const NativeWindow own{XlibWindow{.display = reinterpret_cast<void*>(1), .window = 1}};
#endif
    const auto lacking = dvv::platform::createSurface(VK_NULL_HANDLE, nullptr, other);
    REQUIRE_FALSE(lacking.has_value());
    CHECK(lacking.error() == VK_ERROR_EXTENSION_NOT_PRESENT);

    const auto no_instance = dvv::platform::createSurface(VK_NULL_HANDLE, nullptr, own);
    REQUIRE_FALSE(no_instance.has_value());
    CHECK(no_instance.error() == VK_ERROR_INITIALIZATION_FAILED);
}
