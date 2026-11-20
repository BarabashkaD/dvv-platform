// Includes window_handle.h and nothing else: the header must stand alone.
#include "dvv/platform/window_handle.h"

bool dvvPlatformHandleIsEmpty() {
    return dvv::platform::WindowHandle{}.empty();
}
