#include "dvv/platform/window_handle.h"

namespace dvv::platform {

WindowSystem defaultWindowSystem() noexcept {
    return WindowSystem::Xlib;
}

}  // namespace dvv::platform
