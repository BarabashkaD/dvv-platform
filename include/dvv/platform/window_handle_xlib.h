#pragma once

#include <cstdint>
#include <optional>

#include "dvv/platform/window_handle.h"

namespace dvv::platform {

struct XlibWindow {  // Display*, XID
    void* display = nullptr;
    unsigned long window = 0;
};

// Holds on LP64 (8 <= 8) and LLP64 (4 <= 8), so the header compiles in every test.
static_assert(sizeof(unsigned long) <= sizeof(std::uintptr_t));

// The handle of an Xlib window: the Display* through reinterpret_cast, the XID through
// static_cast. inline, not constexpr: a pointer-to-integer cast is not allowed in a constant
// expression.
[[nodiscard]] inline WindowHandle windowHandleOf(const XlibWindow& window) noexcept {
    return WindowHandle::make(WindowSystem::Xlib, reinterpret_cast<std::uintptr_t>(window.display),
                              static_cast<std::uintptr_t>(window.window));
}

// The Xlib window of a handle; nullopt for an empty handle or another system's handle. The XID
// comes back through an explicit static_cast<unsigned long>, so MSVC's C4244 stays quiet.
[[nodiscard]] inline std::optional<XlibWindow> xlibWindowOf(const WindowHandle& handle) noexcept {
    if (handle.system() != WindowSystem::Xlib) {
        return std::nullopt;
    }
    const WindowHandle::Words words = handle.words();
    return XlibWindow{.display = reinterpret_cast<void*>(words[0]),
                      .window = static_cast<unsigned long>(words[1])};
}

}  // namespace dvv::platform
