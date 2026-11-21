#include "dvv/platform/native_window.h"

#include <variant>

namespace dvv::platform {

// holds_alternative and get_if instead of std::visit, which may throw std::bad_variant_access.
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

WindowHandle windowHandleOf(const NativeWindow& window) noexcept {
    if (const auto* const win32 = std::get_if<Win32Window>(&window)) {
        return windowHandleOf(*win32);
    }
    if (const auto* const xlib = std::get_if<XlibWindow>(&window)) {
        return windowHandleOf(*xlib);
    }
    if (const auto* const metal = std::get_if<MetalWindow>(&window)) {
        return windowHandleOf(*metal);
    }
    return {};
}

}  // namespace dvv::platform
