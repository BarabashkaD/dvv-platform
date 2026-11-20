#include "dvv/platform/window_handle.h"

namespace dvv::platform {

WindowSystem defaultWindowSystem() noexcept {
    return WindowSystem::Metal;
}

}  // namespace dvv::platform
