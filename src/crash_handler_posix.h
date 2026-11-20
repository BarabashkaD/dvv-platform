// src/crash_handler_posix.h (private; macOS and Linux)
#pragma once

#include <signal.h>  // siginfo_t is POSIX's, not <csignal>'s

namespace dvv::platform::detail {
// The fatal signals the handler takes: SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT.
inline constexpr int CRASH_SIGNALS[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT};
// True when the signal was sent (kill, raise, abort, pthread_kill). SIGABRT is always sent.
// Linux: si_code <= 0 (crash_handler_linux.cpp). macOS: si_code 0 or at least SI_USER, 0x10001
// (crash_handler_macos.cpp).
[[nodiscard]] bool signalWasSent(int signal, const siginfo_t& info) noexcept;
// True when returning from the handler runs the faulting instruction again: SIGSEGV, SIGBUS,
// SIGILL or SIGFPE that was not sent. False for SIGTRAP (x86-64's int3 leaves the PC after the
// instruction, so a return would run on), SIGABRT and every sent signal: those are raised again.
// Shared (crash_handler_posix.cpp).
[[nodiscard]] bool faultRepeats(int signal, const siginfo_t& info) noexcept;
}  // namespace dvv::platform::detail
