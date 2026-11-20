// window_handle.h comes first, before any other header, so this file shows that it stands alone;
// the per-OS headers hold no OS header, so all three compile on every OS. Nothing else from dvv is
// included here. clang-format would regroup these below doctest and the standard headers.
// clang-format off
#include "dvv/platform/window_handle.h"
#include "dvv/platform/window_handle_metal.h"
#include "dvv/platform/window_handle_win32.h"
#include "dvv/platform/window_handle_xlib.h"
// clang-format on

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace {

using dvv::platform::MetalWindow;
using dvv::platform::Win32Window;
using dvv::platform::WindowHandle;
using dvv::platform::WindowSystem;
using dvv::platform::XlibWindow;

// A fake native pointer. From a std::uintptr_t, not an int literal: an int cast to a pointer is
// C4312 under MSVC's /WX.
void* fakePointer(std::uintptr_t value) {
    return reinterpret_cast<void*>(value);
}

}  // namespace

TEST_SUITE("window_handle") {
    TEST_CASE("an empty handle") {
        const WindowHandle h{};
        CHECK(h.empty());
        CHECK(h.system() == std::nullopt);
        CHECK(h.words() == WindowHandle::Words{0, 0});
    }

    TEST_CASE("make keeps the system and the words") {
        const auto h = WindowHandle::make(WindowSystem::Xlib, 1, 2);
        CHECK_FALSE(h.empty());
        CHECK(h.system() == WindowSystem::Xlib);
        CHECK(h.words() == WindowHandle::Words{1, 2});
    }

    TEST_CASE("a non-empty handle may hold null words") {
        const auto h = WindowHandle::make(WindowSystem::Win32, 0, 0);
        CHECK_FALSE(h.empty());
        CHECK(h != WindowHandle{});
        CHECK(dvv::platform::win32WindowOf(h).has_value());
    }

    TEST_CASE("a handle is trivially copyable and compares by value") {
        static_assert(std::is_trivially_copyable_v<WindowHandle>);
        const auto h = WindowHandle::make(WindowSystem::Metal, 5, 0);
        CHECK(h == WindowHandle::make(WindowSystem::Metal, 5, 0));
        CHECK(h != WindowHandle::make(WindowSystem::Metal, 6, 0));
        CHECK(h != WindowHandle::make(WindowSystem::Xlib, 5, 0));
    }

    TEST_CASE("each per-OS header round-trips its struct") {
        const Win32Window win32{.hinstance = fakePointer(0x10), .hwnd = fakePointer(0x20)};
        const WindowHandle win32_handle = dvv::platform::windowHandleOf(win32);
        CHECK(win32_handle.system() == WindowSystem::Win32);
        const std::optional<Win32Window> win32_back = dvv::platform::win32WindowOf(win32_handle);
        REQUIRE(win32_back.has_value());
        CHECK(win32_back->hinstance == win32.hinstance);
        CHECK(win32_back->hwnd == win32.hwnd);

        // The widest XID the platform's unsigned long holds: 48 bits on LP64, 32 bits on LLP64.
        const auto xid = static_cast<unsigned long>(
            std::min<std::uint64_t>(0xFFFF'FFFF'FFFF, std::numeric_limits<unsigned long>::max()));
        const XlibWindow xlib{.display = fakePointer(0x30), .window = xid};
        const WindowHandle xlib_handle = dvv::platform::windowHandleOf(xlib);
        CHECK(xlib_handle.system() == WindowSystem::Xlib);
        const std::optional<XlibWindow> xlib_back = dvv::platform::xlibWindowOf(xlib_handle);
        REQUIRE(xlib_back.has_value());
        CHECK(xlib_back->display == xlib.display);
        CHECK(xlib_back->window == xid);

        const MetalWindow metal{.layer = fakePointer(0x40)};
        const WindowHandle metal_handle = dvv::platform::windowHandleOf(metal);
        CHECK(metal_handle.system() == WindowSystem::Metal);
        CHECK(metal_handle.words() == WindowHandle::Words{0x40, 0});
        const std::optional<MetalWindow> metal_back = dvv::platform::metalWindowOf(metal_handle);
        REQUIRE(metal_back.has_value());
        CHECK(metal_back->layer == metal.layer);
    }

    TEST_CASE("reading another system's handle gives nullopt") {
        const WindowHandle win32 = WindowHandle::make(WindowSystem::Win32, 1, 2);
        const WindowHandle xlib = WindowHandle::make(WindowSystem::Xlib, 1, 2);
        const WindowHandle metal = WindowHandle::make(WindowSystem::Metal, 1, 0);
        CHECK_FALSE(dvv::platform::xlibWindowOf(win32).has_value());
        CHECK_FALSE(dvv::platform::metalWindowOf(win32).has_value());
        CHECK_FALSE(dvv::platform::win32WindowOf(xlib).has_value());
        CHECK_FALSE(dvv::platform::metalWindowOf(xlib).has_value());
        CHECK_FALSE(dvv::platform::win32WindowOf(metal).has_value());
        CHECK_FALSE(dvv::platform::xlibWindowOf(metal).has_value());

        CHECK_FALSE(dvv::platform::win32WindowOf(WindowHandle{}).has_value());
        CHECK_FALSE(dvv::platform::xlibWindowOf(WindowHandle{}).has_value());
        CHECK_FALSE(dvv::platform::metalWindowOf(WindowHandle{}).has_value());
    }
}
