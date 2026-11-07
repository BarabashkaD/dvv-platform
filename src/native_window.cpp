#include "dvv/platform/native_window.h"

#include <variant>

namespace dvv::platform {

std::string_view windowSystemName(WindowSystem system) noexcept {
    switch (system) {
        case WindowSystem::Win32: return "Win32";
        case WindowSystem::Xlib: return "Xlib";
        case WindowSystem::Metal: return "Metal";
    }
    return {};
}

WindowSystem defaultWindowSystem() noexcept {
#if defined(_WIN32)
    return WindowSystem::Win32;
#elif defined(__APPLE__)
    return WindowSystem::Metal;
#else
    return WindowSystem::Xlib;
#endif
}

// holds_alternative instead of std::visit, which may throw std::bad_variant_access.
std::optional<WindowSystem> windowSystemOf(const NativeWindow& window) noexcept {
    if (std::holds_alternative<Win32Window>(window)) {
        return WindowSystem::Win32;
    }
    if (std::holds_alternative<XlibWindow>(window)) {
        return WindowSystem::Xlib;
    }
    if (std::holds_alternative<MetalWindow>(window)) {
        return WindowSystem::Metal;
    }
    return std::nullopt;
}

}  // namespace dvv::platform
