#include "crash_handler_posix.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "crash_handler_detail.h"
#include "dvv/base/local_time.h"
#include "dvv/base/log.h"
#include "dvv/base/utf8.h"

namespace dvv::platform::detail {

namespace {

constexpr std::size_t FOLDER_CAPACITY = 1024;  // a longer crash folder path: the handler is off
constexpr std::size_t NAME_CAPACITY = 64;      // app and process: 1 to 63 bytes
// <folder>/<stem>.dvvlog.part: the stem is the process, 15 bytes of time, a pid and three dashes.
constexpr std::size_t PATH_CAPACITY = FOLDER_CAPACITY + 128;
constexpr std::size_t LINE_CAPACITY = NAME_CAPACITY + PATH_CAPACITY + 64;
constexpr std::size_t WRITE_BUFFER_BYTES = std::size_t{64} * 1024;
constexpr std::size_t ALTERNATE_STACK_BYTES = std::size_t{256} * 1024;
constexpr std::size_t SIGNAL_COUNT = std::size(CRASH_SIGNALS);
// A thread that crashes while another one writes waits this long for it, in 1 ms turns.
constexpr int WAIT_TURNS = 120'000;

// Process infrastructure, set once by installBackend. Trivially destructible at namespace scope,
// so it is never destroyed: a crash in a static destructor still finds it. The buffers live here,
// not on a stack, because the crashing thread may have little stack left.
struct CrashState {
    char folder[FOLDER_CAPACITY];  // NUL-terminated native bytes
    char app[NAME_CAPACITY];
    char process[NAME_CAPACITY];
    std::size_t appLength;
    std::size_t processLength;
    std::int64_t offsetSeconds;  // local time minus UTC at install
    const dvv::Logger* logger;
    std::atomic<int> owner;  // 1 once a thread has taken the crash
    std::atomic<int> done;   // 1 once the owner has restored the previous actions
    int file;                // the snapshot file while it is written
    bool writeFailed;
    std::size_t buffered;
    char buffer[WRITE_BUFFER_BYTES];
    char path[PATH_CAPACITY];      // <folder>/<stem>.dvvlog, NUL-terminated
    char partPath[PATH_CAPACITY];  // <folder>/<stem>.dvvlog.part, the file while it is written
    char line[LINE_CAPACITY];
    struct sigaction previous[SIGNAL_COUNT];
    alignas(16) char alternateStack[ALTERNATE_STACK_BYTES];
};
static_assert(std::is_trivially_destructible_v<CrashState>);
static_assert(std::atomic<int>::is_always_lock_free);
constinit CrashState state{};

// write(2) of all `size` bytes, retrying EINTR and partial writes; false on any other failure.
bool writeAll(int file, const char* data, std::size_t size) noexcept {
    while (size > 0) {
        const ssize_t written = ::write(file, data, size);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        data += written;
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

// Writes the first `length` bytes of the line buffer to stderr; errors are ignored.
void writeLine(std::size_t length) noexcept {
    static_cast<void>(writeAll(STDERR_FILENO, state.line, length));
}

// Writes the buffered snapshot bytes to the file; a failure stops the file's writes.
void flushBuffer() noexcept {
    if (!state.writeFailed && !writeAll(state.file, state.buffer, state.buffered)) {
        state.writeFailed = true;
    }
    state.buffered = 0;
}

// The Logger's CrashWriter: copies into the 64 KiB buffer, writing it out each time it is full.
void writeSnapshotBytes(void* /*context*/, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const char*>(data);
    while (size > 0 && !state.writeFailed) {
        std::size_t room = WRITE_BUFFER_BYTES - state.buffered;
        if (room == 0) {
            flushBuffer();
            room = WRITE_BUFFER_BYTES;
            continue;
        }
        const std::size_t count = size < room ? size : room;
        for (std::size_t i = 0; i < count; ++i) {
            state.buffer[state.buffered + i] = bytes[i];
        }
        state.buffered += count;
        bytes += count;
        size -= count;
    }
}

// mkdir of every folder above the crash folder and of the folder itself; every error is ignored
// (EEXIST is the usual one): the open of the file reports a real failure.
void makeFolders() noexcept {
    for (std::size_t i = 1; i < FOLDER_CAPACITY && state.folder[i] != '\0'; ++i) {
        if (state.folder[i] == '/') {
            state.folder[i] = '\0';
            static_cast<void>(::mkdir(state.folder, 0700));
            state.folder[i] = '/';
        }
    }
    static_cast<void>(::mkdir(state.folder, 0700));
}

// <folder>/<stem>.dvvlog for the crash time into the path buffer and the same with ".part" into
// the part path buffer; false when they do not fit.
bool formatSnapshotPaths() noexcept {
    timespec now{};
    if (::clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return false;
    }
    const std::span<char> path(state.path);
    std::size_t length = 0;
    std::size_t folder_length = 0;
    while (folder_length < FOLDER_CAPACITY && state.folder[folder_length] != '\0') {
        ++folder_length;
    }
    if (!appendText(path, length, std::string_view(state.folder, folder_length)) ||
        !appendText(path, length, "/")) {
        return false;
    }
    const std::size_t stem =
        formatCrashStem(path.subspan(length), std::string_view(state.process, state.processLength),
                        static_cast<std::int64_t>(now.tv_sec) + state.offsetSeconds,
                        static_cast<std::uint64_t>(::getpid()));
    if (stem == 0) {
        return false;
    }
    length += stem;
    if (!appendText(path, length, ".dvvlog")) {
        return false;
    }
    std::size_t part_length = 0;
    return appendText(state.partPath, part_length, std::string_view(state.path, length)) &&
           appendText(state.partPath, part_length, ".part") &&
           appendText(state.partPath, part_length, std::string_view("\0", 1)) &&
           appendText(path, length, std::string_view("\0", 1));
}

// The crash snapshot to <stem>.dvvlog.part, renamed to <stem>.dvvlog once it is written (so the
// report of a process that starts meanwhile never reads it half written), then "<app>: crash log
// <path>" on stderr. Any failed step skips the rest of this file work.
void writeSnapshotFile() noexcept {
    if (state.logger == nullptr || !formatSnapshotPaths()) {
        return;
    }
    makeFolders();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open(2) is variadic
    state.file = ::open(state.partPath, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (state.file < 0) {
        return;
    }
    state.writeFailed = false;
    state.buffered = 0;
    state.logger->writeCrashSnapshot(&writeSnapshotBytes, nullptr);
    flushBuffer();
    const bool closed = ::close(state.file) == 0;
    state.file = -1;
    // Renamed also after a failed write: the next start reports a cut-short snapshot as well.
    const bool renamed = ::rename(state.partPath, state.path) == 0;
    if (state.writeFailed || !closed || !renamed) {
        return;
    }
    const std::span<char> line(state.line);
    std::size_t length = 0;
    std::size_t path_length = 0;
    while (state.path[path_length] != '\0') {
        ++path_length;
    }
    if (appendText(line, length, std::string_view(state.app, state.appLength)) &&
        appendText(line, length, ": crash log ") &&
        appendText(line, length, std::string_view(state.path, path_length)) &&
        appendText(line, length, "\n")) {
        writeLine(length);
    }
}

// Puts back the actions that were installed before this handler.
void restorePreviousActions() noexcept {
    for (std::size_t i = 0; i < SIGNAL_COUNT; ++i) {
        static_cast<void>(::sigaction(CRASH_SIGNALS[i], &state.previous[i], nullptr));
    }
}

// After the handler: a fault that repeats is left to run again under the restored action; any
// other signal is raised again, pending until the handler returns.
void passOn(int signal, const siginfo_t& info) noexcept {
    if (!faultRepeats(signal, info)) {
        static_cast<void>(::raise(signal));
    }
}

// The fatal-signal handler. Async-signal-safe calls only.
void onCrashSignal(int signal, siginfo_t* info, void* /*context*/) {
    const int saved_errno = errno;
    int expected = 0;
    if (!state.owner.compare_exchange_strong(expected, 1)) {
        // Another thread writes: wait for it, then let this signal take its course too.
        timespec turn{};
        turn.tv_nsec = 1'000'000;
        for (int i = 0; i < WAIT_TURNS && state.done.load() == 0; ++i) {
            static_cast<void>(::nanosleep(&turn, nullptr));
        }
        passOn(signal, *info);
        errno = saved_errno;
        return;
    }
    const std::span<char> line(state.line);
    std::size_t length = 0;
    if (appendText(line, length, std::string_view(state.app, state.appLength)) &&
        appendText(line, length, ": crash signal ") &&
        appendNumberAt(line, length, static_cast<std::uint64_t>(signal), 10, 1) &&
        appendText(line, length, "\n")) {
        writeLine(length);
    }
    writeSnapshotFile();
    restorePreviousActions();
    state.done.store(1);
    passOn(signal, *info);
    errno = saved_errno;
}

// The local-time offset in seconds: the local calendar fields of `now` against its UTC fields;
// 0 (UTC names) when either conversion fails.
std::int64_t localOffsetSeconds(std::chrono::system_clock::time_point now) {
    const std::optional<std::tm> local = dvv::localTime(now);
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    if (!local || ::gmtime_r(&seconds, &utc) == nullptr) {
        return 0;
    }
    std::int64_t days = local->tm_yday - utc.tm_yday;
    if (local->tm_year != utc.tm_year) {
        days = local->tm_year > utc.tm_year ? 1 : -1;  // across New Year
    }
    const std::int64_t hours = (days * 24) + local->tm_hour - utc.tm_hour;
    const std::int64_t minutes = (hours * 60) + local->tm_min - utc.tm_min;
    return (minutes * 60) + local->tm_sec - utc.tm_sec;
}

// Copies `text` and a NUL into `target`; false when it does not fit.
template <std::size_t N>
bool copyText(char (&target)[N], std::string_view text) noexcept {
    if (text.size() >= N) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        target[i] = text[i];
    }
    target[text.size()] = '\0';
    return true;
}

// Recurses until the stack overflows; `depth` never gets negative, so every call recurses, and
// adding frame[0] to the result keeps each call from being a tail call.
int overflowStack(int depth) {
    volatile char frame[4096];
    frame[0] = static_cast<char>(depth);
    if (depth >= 0) {
        return overflowStack(depth + 1) + frame[0];
    }
    return frame[0];
}

// An invalid memory access: a write through a null pointer the compiler cannot see as null.
void writeThroughNull() {
    int* volatile target = nullptr;
    *target = 1;
}

// Four threads fault at the same moment; the first one's snapshot must win.
void crashOnThreads() {
    std::atomic<bool> go{false};
    std::array<std::thread, 4> threads;
    for (std::thread& thread : threads) {
        thread = std::thread([&go] {
            while (!go.load()) {
            }
            writeThroughNull();
        });
    }
    go.store(true);
    for (std::thread& thread : threads) {
        thread.join();
    }
}

// Faults in its destructor when armed. Constant-initialized, so its destructor is registered
// before main() and runs after the destructors of every static created later.
struct ExitCrash {
    std::atomic<bool> armed{false};
    constexpr ExitCrash() = default;
    ExitCrash(const ExitCrash&) = delete;
    ExitCrash& operator=(const ExitCrash&) = delete;
    ExitCrash(ExitCrash&&) = delete;
    ExitCrash& operator=(ExitCrash&&) = delete;
    ~ExitCrash() {
        if (armed.load()) {
            writeThroughNull();
        }
    }
};
constinit ExitCrash exit_crash;

}  // namespace

bool faultRepeats(int signal, const siginfo_t& info) noexcept {
    const bool fault =
        signal == SIGSEGV || signal == SIGBUS || signal == SIGILL || signal == SIGFPE;
    return fault && !signalWasSent(signal, info);
}

BackendStatus installBackend(const CrashBackendSetup& setup) {
    if (!copyText(state.folder, dvv::pathText(setup.folder))) {
        return {.off = "the crash folder path is too long"};
    }
    // §4.9 step 2 limits both names to 63 bytes, so they fit; a name that did not would be empty.
    state.appLength = copyText(state.app, setup.app) ? setup.app.size() : 0;
    state.processLength = copyText(state.process, setup.process) ? setup.process.size() : 0;
    state.offsetSeconds = localOffsetSeconds(std::chrono::system_clock::now());
    state.logger = setup.logger;
    state.file = -1;

    // The installing thread's alternate stack, so a stack overflow can run the handler; kept when
    // the thread has one already.
    stack_t current{};
    if (::sigaltstack(nullptr, &current) != 0 || (current.ss_flags & SS_DISABLE) != 0) {
        stack_t alternate{};
        alternate.ss_sp = static_cast<void*>(state.alternateStack);
        alternate.ss_size = ALTERNATE_STACK_BYTES;
        alternate.ss_flags = 0;
        static_cast<void>(::sigaltstack(&alternate, nullptr));
    }

    struct sigaction action{};
    action.sa_sigaction = &onCrashSignal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (const int signal : CRASH_SIGNALS) {
        sigaddset(&action.sa_mask, signal);
    }
    for (std::size_t i = 0; i < SIGNAL_COUNT; ++i) {
        if (::sigaction(CRASH_SIGNALS[i], &action, &state.previous[i]) != 0) {
            const int error = errno;
            for (std::size_t set = 0; set < i; ++set) {
                static_cast<void>(::sigaction(CRASH_SIGNALS[set], &state.previous[set], nullptr));
            }
            return {.off = "sigaction failed: " + std::generic_category().message(error)};
        }
    }
    return {};
}

void crashNow(std::string_view mode) {
    if (mode == "1") {
        writeThroughNull();
    } else if (mode == "stack") {
        static_cast<void>(overflowStack(0));
    } else if (mode == "threads") {
        crashOnThreads();
    } else if (mode == "exit") {
        exit_crash.armed.store(true);  // the process faults after main(), in the static destructors
    } else if (mode == "abort") {
        std::abort();
    } else if (mode == "terminate") {
        std::terminate();
    }
}

}  // namespace dvv::platform::detail
