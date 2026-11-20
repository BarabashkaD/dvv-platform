#pragma once

#include <string>
#include <string_view>

namespace dvv::platform {

struct CrashHandlerOptions {
    // The app's data folder name, 1 to 63 bytes of UTF-8 that userDataDir accepts: crash files go
    // to <dvv::paths::userDataDir(app)>/crashes; the stderr lines start with "<app>: ".
    std::string app;
    // This executable's name in the crash file names: 1 to 63 ASCII letters, digits, '.', '_' or
    // '-', not starting with '.' (e.g. "rawreel-cli"). Files: <process>-<yyyymmdd-hhmmss>-<pid>.dmp
    // (Windows) and .dvvlog.
    std::string process;
};

// Installs the process's crash handler. Call it once, after the log sinks exist (it logs; in
// either logger mode). A later call does nothing.
//
// On a crash it writes, in this order and with no logger call, lock or allocation:
// - Windows (an unhandled exception, or abort()): "<app>: crash exception 0x<8 hex digits>" (or
//   "<app>: crash abort") on stderr; the logger's crash snapshot (dvv::Logger::writeCrashSnapshot)
//   to <stem>.dvvlog, then "<app>: crash log <path>"; a minidump <stem>.dmp, then "<app>: crash
//   dump <path>"; then it ends the process with the exception code (3 for abort()).
// - macOS and Linux (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP or SIGABRT): "<app>: crash signal
//   <number>" on stderr; the crash snapshot to <stem>.dvvlog, then "<app>: crash log <path>"; then
//   the signal takes its course under the action that was installed before (by default the
//   system's crash report on macOS, a core dump where Linux enables them).
// A crash cannot keep the process alive: on macOS and Linux the handler arms alarm() for 120 s
// at its start (SIGALRM's action and an earlier alarm are put back after it), and when it fires
// (a blocked stderr, a hung file system) the process ends by the crash signal under its default
// action; on Windows the crashing thread waits at most 120 s for the dump. A second crashing
// thread waits for the first one at most 125 s (macOS, Linux) or 130 s (Windows), then ends the
// process itself the same way.
// The snapshot is written as <stem>.dvvlog.part and renamed to <stem>.dvvlog once written, so a
// .dvvlog file is never half written; a crash inside the handler can leave the .part file, which
// is never reported. The time in <stem> is the crash time. The crashes folder and every missing
// folder above it are created at the first crash, so a run without a crash creates nothing.
//
// At the call it also logs, at Info, the crash snapshots that earlier runs of the same `process`
// left in the folder (at most 8, oldest first): one line naming the file, then each decoded
// record. A reported <stem>.dvvlog is renamed to <stem>.reported.dvvlog, so each is reported once,
// also when two processes start at the same moment. One that cannot be renamed (a read-only
// folder) is not read; it gets one Info line "an earlier run left a crash log that cannot be
// renamed (<path>): <reason>".
//
// When it cannot install, it logs one Warn "crash handler is off: <reason>" and the process runs
// without it. On Windows, when it can install but cannot write minidumps (dbghelp.dll or its
// MiniDumpWriteDump missing, or no dump thread), it logs one Warn "crash dumps are off: <reason>"
// and stays on: a crash writes the stderr lines and <stem>.dvvlog, and no <stem>.dmp. Install it
// last among the process's fatal-signal handlers (macOS, Linux): one installed later replaces it;
// one installed earlier runs after it. On Windows it replaces an earlier SIGABRT handler, which
// then never runs, and one installed later replaces it; each CRT instance has its own signal
// table, so an abort() in a module with its own static CRT does not reach it. With the Debug CRT,
// abort() first shows the CRT's message box and waits for it, unless the app calls
// _set_abort_behavior(0, _WRITE_ABORT_MSG). Throws only std::bad_alloc.
void installCrashHandler(const CrashHandlerOptions& options);

// Test hook: reads the environment variable `variable` and crashes that way: "1" an access
// violation (Windows) or an invalid memory access (macOS, Linux) on this thread; "stack" a stack
// overflow on this thread (Windows: exits 99 when installCrashHandler's stack guarantee is not in
// effect on this thread, first writing "<app>: stack guarantee missing" on stderr when the handler
// is on); "threads" four threads fault at once; "exit" a fault in a static destructor after main()
// returns; "abort" std::abort(); "terminate" std::terminate(). Any other value, or no variable,
// returns.
void crashIfRequested(std::string_view variable);

}  // namespace dvv::platform
