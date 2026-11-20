#include <signal.h>

#include "crash_handler_posix.h"

namespace dvv::platform::detail {

// kill reports SI_USER (0), tgkill and raise SI_TKILL (-6), sigqueue SI_QUEUE (-1): a sent signal
// has si_code <= 0; the kernel's own codes are positive (SI_KERNEL included).
bool signalWasSent(int signal, const siginfo_t& info) noexcept {
    return signal == SIGABRT || info.si_code <= 0;
}

}  // namespace dvv::platform::detail
