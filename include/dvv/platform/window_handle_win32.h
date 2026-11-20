#pragma once

#include <cstdint>
#include <optional>

#include "dvv/platform/window_handle.h"

namespace dvv::platform {

struct Win32Window {  // HINSTANCE, HWND
    void* hinstance = nullptr;
    void* hwnd = nullptr;
};

// The handle of a Win32 window. inline, not constexpr: a pointer-to-integer cast is not allowed
// in a constant expression.
[[nodiscard]] inline WindowHandle windowHandleOf(const Win32Window& window) noexcept {
    return WindowHandle::make(WindowSystem::Win32,
                              reinterpret_cast<std::uintptr_t>(window.hinstance),
                              reinterpret_cast<std::uintptr_t>(window.hwnd));
}

// The Win32 window of a handle; nullopt for an empty handle or another system's handle.
[[nodiscard]] inline std::optional<Win32Window> win32WindowOf(const WindowHandle& handle) noexcept {
    if (handle.system() != WindowSystem::Win32) {
        return std::nullopt;
    }
    const WindowHandle::Words words = handle.words();
    // NOLINTBEGIN(performance-no-int-to-ptr): native handles stored as words
    return Win32Window{.hinstance = reinterpret_cast<void*>(words[0]),
                       .hwnd = reinterpret_cast<void*>(words[1])};
    // NOLINTEND(performance-no-int-to-ptr)
}

}  // namespace dvv::platform
