#include "dvv/platform/window_handle.h"

namespace dvv::platform {

std::string_view windowSystemName(WindowSystem system) noexcept {
    switch (system) {
        case WindowSystem::Win32: return "Win32";
        case WindowSystem::Xlib: return "Xlib";
        case WindowSystem::Metal: return "Metal";
    }
    return {};
}

}  // namespace dvv::platform
