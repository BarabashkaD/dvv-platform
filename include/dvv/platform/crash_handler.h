#pragma once

#include <string>
#include <string_view>

namespace dvv::platform {

struct CrashHandlerOptions {
    // The app's data folder name, 1 to 63 bytes of UTF-8 that userDataDir accepts: crash files go
    // to <dvv::paths::userDataDir(app)>/crashes; the stderr lines start with "<app>: ".
    std::string app;
    // This executable's name in the crash file names: 1 to 63 ASCII letters, digits, '.', '_' or
    // '-' (e.g. "rawreel-cli"). Files: <process>-<yyyymmdd-hhmmss>-<pid>.dmp (Windows) and .dvvlog.
    std::string process;
};

// Installs the process's crash handler. Call it once, after the log sinks exist (it logs; in
// either logger mode). A later call does nothing.
//
// On a crash it writes, in this order and with no logger call, lock or allocation:
// - Windows (an unhandled exception): "<app>: crash exception 0x<8 hex digits>" on stderr; the
//   logger's crash snapshot (dvv::Logger::writeCrashSnapshot) to <stem>.dvvlog, then "<app>:
//   crash log <path>"; a minidump <stem>.dmp, then "<app>: crash dump <path>"; then it ends the
//   process with the exception code.
// - macOS and Linux (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP or SIGABRT): "<app>: crash signal
//   <number>" on stderr; the crash snapshot to <stem>.dvvlog, then "<app>: crash log <path>"; then
//   the signal takes its course under the action that was installed before (by default the
//   system's crash report on macOS, a core dump where Linux enables them).
// The time in <stem> is the crash time. The crashes folder and every missing folder above it are
// created at the first crash, so a run without a crash creates nothing.
//
// At the call it also logs, at Info, the crash snapshots that earlier runs of the same `process`
// left in the folder (at most 8, oldest first): one line naming the file, then each decoded
// record. A reported <stem>.dvvlog is renamed to <stem>.reported.dvvlog, so each is reported once,
// also when two processes start at the same moment.
//
// When it cannot install, it logs one Warn "crash handler is off: <reason>" and the process runs
// without it. Install it last among the process's fatal-signal handlers (macOS, Linux): one
// installed later replaces it; one installed earlier runs after it. Throws only std::bad_alloc.
void installCrashHandler(const CrashHandlerOptions& options);

// Test hook: reads the environment variable `variable` and crashes that way: "1" an access
// violation (Windows) or an invalid memory access (macOS, Linux) on this thread; "stack" a stack
// overflow on this thread (Windows: exits 99 with "<app>: stack guarantee missing" when
// installCrashHandler's stack guarantee is not in effect on this thread); "threads" four threads
// fault at once; "exit" a fault in a static destructor after main() returns; "abort" std::abort().
// Any other value, or no variable, returns.
void crashIfRequested(std::string_view variable);

}  // namespace dvv::platform
