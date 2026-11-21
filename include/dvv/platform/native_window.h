#pragma once

// Deprecated: include window_handle.h, pixel_size.h and the per-OS header; removed in 1.0.0.

#include <optional>
#include <variant>

#include "dvv/platform/pixel_size.h"
#include "dvv/platform/window_handle.h"
#include "dvv/platform/window_handle_metal.h"
#include "dvv/platform/window_handle_win32.h"
#include "dvv/platform/window_handle_xlib.h"

namespace dvv::platform {

using NativeWindow = std::variant<std::monostate, Win32Window, XlibWindow, MetalWindow>;
// nullopt for std::monostate.
[[nodiscard]] std::optional<WindowSystem> windowSystemOf(const NativeWindow& window) noexcept;
// The handle of a variant (empty for std::monostate). The bridge for dvv-gpu 0.3.x.
[[nodiscard]] WindowHandle windowHandleOf(const NativeWindow& window) noexcept;

}  // namespace dvv::platform
