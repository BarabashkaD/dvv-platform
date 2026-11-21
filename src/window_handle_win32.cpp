#include "dvv/platform/window_handle.h"

namespace dvv::platform {

WindowSystem defaultWindowSystem() noexcept {
    return WindowSystem::Win32;
}

}  // namespace dvv::platform
