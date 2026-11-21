#pragma once

#include <cstdint>
#include <optional>

#include "dvv/platform/window_handle.h"

namespace dvv::platform {

struct MetalWindow {  // CAMetalLayer*
    void* layer = nullptr;
};

// The handle of a Metal window: the layer, second word 0. inline, not constexpr: a
// pointer-to-integer cast is not allowed in a constant expression.
[[nodiscard]] inline WindowHandle windowHandleOf(const MetalWindow& window) noexcept {
    return WindowHandle::make(WindowSystem::Metal, reinterpret_cast<std::uintptr_t>(window.layer),
                              0);
}

// The Metal window of a handle; nullopt for an empty handle or another system's handle.
[[nodiscard]] inline std::optional<MetalWindow> metalWindowOf(const WindowHandle& handle) noexcept {
    if (handle.system() != WindowSystem::Metal) {
        return std::nullopt;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr): a native handle stored as a word
    return MetalWindow{.layer = reinterpret_cast<void*>(handle.words()[0])};
}

}  // namespace dvv::platform
