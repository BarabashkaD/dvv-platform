#include <signal.h>

#include "crash_handler_posix.h"

namespace dvv::platform::detail {

// kill, raise and pthread_kill report si_code 0 or SI_USER (0x10001); the kernel's fault codes
// are small positive numbers.
bool signalWasSent(int signal, const siginfo_t& info) noexcept {
    return signal == SIGABRT || info.si_code == 0 || info.si_code >= SI_USER;
}

}  // namespace dvv::platform::detail
