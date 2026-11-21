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
// The time limit of a crash in seconds (0: the default, 120): the handler arms alarm() with it at
// its start, and when SIGALRM comes first the process ends by the crash signal under its default
// action; a thread that waits for the crashing one waits at most 5 s longer, then does the same.
// The crash test's child sets a short one. Shared (crash_handler_posix.cpp).
void setCrashTimeLimit(unsigned seconds) noexcept;
}  // namespace dvv::platform::detail
