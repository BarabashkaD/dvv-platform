// The crash test's child (spec §8, tests/crash/run-crash-tests.cmake):
//   dvv_platform_crash_child <process> [<app>]
// installs the crash handler as <process> (app "dvv-crash-test" unless named) with a sink that
// prints each record's message to stdout, then crashes as DVV_PLATFORM_TEST_CRASH asks: a mode of
// crashIfRequested, "sink" (a fault inside the sink, synchronous logger), "chain" (macOS, Linux: a
// fault under the child's own SIGSEGV handler, installed before the crash handler) or "hang"
// (macOS, Linux: a fault while stderr is a full pipe, so the handler hangs at its first line and
// its 2 s time limit must end the process). Without the variable it only installs, which reports
// earlier crashes, and exits 0.
#include <atomic>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

#include "dvv/base/environment.h"
#include "dvv/base/log.h"
#include "dvv/platform/crash_handler.h"
#if !defined(_WIN32)
#include "crash_handler_posix.h"
#endif

namespace {

// An invalid memory access: a write through a null pointer the compiler cannot see as null.
void writeThroughNull() {
    int* volatile target = nullptr;
    *target = 1;
}

// Prints each record's message. Faults on the "sink" marker; blocks forever on any other mode's
// marker, so the log thread never releases the marker before the crash and it is always in the
// crash snapshot.
class ChildSink final : public dvv::ILogSink {
public:
    explicit ChildSink(std::string blocking_marker)
        : blocking_marker_(std::move(blocking_marker)) {}

    void write(const dvv::LogRecord& record, std::string_view /*line*/) override {
        static_cast<void>(std::fwrite(record.message.data(), 1, record.message.size(), stdout));
        static_cast<void>(std::fputc('\n', stdout));
        static_cast<void>(std::fflush(stdout));
        if (record.message == "crash child marker sink") {
            writeThroughNull();
        } else if (!blocking_marker_.empty() && record.message == blocking_marker_) {
            never_set_.wait(false);
        }
    }

private:
    std::string blocking_marker_;
    std::atomic<bool> never_set_{false};
};

#if !defined(_WIN32)
// The handler installed before the crash handler: it runs after it, when the fault repeats.
void previousHandler(int /*signal*/) {
    constexpr std::string_view TEXT = "previous handler ran\n";
    static_cast<void>(::write(STDOUT_FILENO, TEXT.data(), TEXT.size()));
    ::_exit(42);
}

// Makes stderr a pipe that is full and never read, so the crash handler's first write to it blocks
// for good. The read end stays open (a closed one would end the write with EPIPE).
bool fillStderr() {
    int ends[2] = {-1, -1};
    if (::pipe(ends) != 0) {
        return false;
    }
    // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg): fcntl(2) is variadic
    const int flags = ::fcntl(ends[1], F_GETFL);
    if (flags < 0 || ::fcntl(ends[1], F_SETFL, flags | O_NONBLOCK) != 0) {
        return false;
    }
    const char filler[1024] = {};
    while (::write(ends[1], filler, sizeof filler) > 0) {
    }
    while (::write(ends[1], filler, 1) > 0) {  // the room a whole block no longer fits in
    }
    if (::fcntl(ends[1], F_SETFL, flags) != 0) {
        return false;
    }
    // NOLINTEND(cppcoreguidelines-pro-type-vararg)
    return ::dup2(ends[1], STDERR_FILENO) == STDERR_FILENO;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        static_cast<void>(
            std::fputs("usage: dvv_platform_crash_child <process> [<app>]\n", stderr));
        return 2;
    }
    const std::string process = argv[1];
    const std::string app = argc > 2 ? argv[2] : "dvv-crash-test";
    const std::optional<std::string> mode = dvv::getEnvironment("DVV_PLATFORM_TEST_CRASH");

    std::string blocking_marker;  // every mode's marker but that of "sink", which faults instead
    if (mode && *mode != "sink") {
        blocking_marker = "crash child marker " + *mode;
    }
    dvv::Logger::instance().addSink(std::make_shared<ChildSink>(std::move(blocking_marker)),
                                    dvv::LogLevel::Info);

    if (mode == "chain") {
#if defined(_WIN32)
        static_cast<void>(std::fputs("chain is POSIX only\n", stdout));
        return 2;
#else
        struct sigaction action{};
        action.sa_handler = previousHandler;
        static_cast<void>(sigemptyset(&action.sa_mask));
        if (sigaction(SIGSEGV, &action, nullptr) != 0) {
            std::perror("sigaction");
            return 2;
        }
#endif
    }
    if (mode == "hang") {
#if defined(_WIN32)
        static_cast<void>(std::fputs("hang is POSIX only\n", stdout));
        return 2;
#else
        dvv::platform::detail::setCrashTimeLimit(2);
#endif
    }

    dvv::platform::installCrashHandler({.app = app, .process = process});
    if (!mode) {
        return 0;
    }
    if (*mode != "sink") {
        dvv::Logger::instance().start();
    }
    DVV_LOG_INFO("crash child marker {}", *mode);
    if (*mode == "chain") {
        writeThroughNull();
    } else if (*mode == "hang") {
#if !defined(_WIN32)
        if (!fillStderr()) {
            std::perror("fillStderr");
            return 2;
        }
        writeThroughNull();
#endif
    } else {
        dvv::platform::crashIfRequested("DVV_PLATFORM_TEST_CRASH");
    }
    return 0;  // mode "exit" crashes after this, in the static destructors
}
