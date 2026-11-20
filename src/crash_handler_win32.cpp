#include <array>
#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format off
#include <dbghelp.h>  // after windows.h
// clang-format on

#include "crash_handler_detail.h"
#include "dvv/base/log.h"
#include "dvv/platform/dynamic_library.h"

namespace dvv::platform::detail {

namespace {

using WriteDump = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                                PMINIDUMP_CALLBACK_INFORMATION);

constexpr std::size_t FOLDER_CAPACITY = 1024;  // a longer crash folder path: the handler is off
constexpr std::size_t NAME_CAPACITY = 64;      // app and process: 1 to 63 bytes
// <process>-<yyyymmdd-hhmmss>-<pid>: at most 63 + 1 + 8 + 1 + 6 + 1 + 10 bytes.
constexpr std::size_t STEM_CAPACITY = 128;
// <folder>\<stem>.dvvlog.part, .dvvlog or .dmp and a NUL.
constexpr std::size_t PATH_CAPACITY = FOLDER_CAPACITY + STEM_CAPACITY;
constexpr std::size_t UTF8_CAPACITY = 4 * PATH_CAPACITY;
constexpr std::size_t LINE_CAPACITY = NAME_CAPACITY + UTF8_CAPACITY + 32;
constexpr std::size_t WRITE_BUFFER_BYTES = std::size_t{64} * 1024;
// The dump thread's own stack (Microsoft's advice for MiniDumpWriteDump, which needs more stack
// than a stack overflow leaves).
constexpr SIZE_T DUMP_THREAD_STACK_BYTES = SIZE_T{256} * 1024;
// How long the crashing thread waits for the dump thread before it ends the process anyway.
constexpr DWORD DUMP_TIMEOUT_MS = 120'000;
// How long a thread that crashes while another one owns the crash waits for that one to end the
// process, before it ends it itself (the owner hangs when it writes on its own thread).
constexpr DWORD WAITER_TIMEOUT_MS = DUMP_TIMEOUT_MS + 10'000;
// Stack that installBackend reserves on its calling thread for the stack-overflow path.
constexpr ULONG OVERFLOW_STACK_GUARANTEE = 64 * 1024;
// Exit code of crashNow("stack") when that guarantee is missing.
constexpr UINT GUARANTEE_MISSING_EXIT_CODE = 99;
// Exit code of a process that abort() ends: the CRT's abort code.
constexpr UINT ABORT_EXIT_CODE = 3;

// Process infrastructure, set once by installBackend. Trivially destructible at namespace scope,
// so it is never destroyed: a crash in a static destructor or at DLL detach still finds it. The
// buffers live here, not on a stack, because the crashing thread may have no stack left.
struct CrashState {
    wchar_t folder[FOLDER_CAPACITY];  // <userDataDir(app)>\crashes, NUL-terminated
    std::size_t folderLength;
    std::size_t rootLength;   // the folder's root ("C:\"): no folder of the chain is made there
    char app[NAME_CAPACITY];  // UTF-8, the stderr lines' prefix
    std::size_t appLength;
    char exe[NAME_CAPACITY];  // the process name (ASCII), the file names' prefix
    std::size_t exeLength;
    const dvv::Logger* logger;
    WriteDump writeDump;
    HANDLE request;                // auto-reset event: the filter wakes the dump thread
    HANDLE done;                   // manual-reset event: the dump thread has finished
    HANDLE thread;                 // the dump thread, created by installBackend (its own stack)
    DWORD dumpThreadId;            // its id
    std::atomic<DWORD> owner;      // the first crashing thread (0: none yet); later ones wait
    EXCEPTION_POINTERS* pointers;  // of the owner, for the dump thread; nullptr for abort()
    DWORD faultingThreadId;
    HANDLE file;  // the snapshot file while it is written
    bool writeFailed;
    std::size_t buffered;
    char buffer[WRITE_BUFFER_BYTES];
    char stem[STEM_CAPACITY];
    std::size_t stemLength;
    wchar_t path[PATH_CAPACITY];      // <folder>\<stem>.dvvlog or .dmp, NUL-terminated
    wchar_t partPath[PATH_CAPACITY];  // <folder>\<stem>.dvvlog.part, the snapshot while written
    char utf8[UTF8_CAPACITY];
    char line[LINE_CAPACITY];
};
static_assert(std::is_trivially_destructible_v<CrashState>);
static_assert(std::atomic<DWORD>::is_always_lock_free);
constinit CrashState state{};

// WriteFile of all `size` bytes; false on a failure.
bool writeAll(HANDLE file, const char* data, std::size_t size) noexcept {
    while (size > 0) {
        DWORD written = 0;
        if (WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) == FALSE ||
            written == 0) {
            return false;
        }
        data += written;
        size -= written;
    }
    return true;
}

// Writes the first `length` bytes of the line buffer to stderr; errors are ignored.
void writeLine(std::size_t length) noexcept {
    static_cast<void>(writeAll(GetStdHandle(STD_ERROR_HANDLE), state.line, length));
}

// Writes the buffered snapshot bytes to the file; a failure stops the file's writes.
void flushBuffer(CrashState& crash) noexcept {
    if (!crash.writeFailed && !writeAll(crash.file, crash.buffer, crash.buffered)) {
        crash.writeFailed = true;
    }
    crash.buffered = 0;
}

// The Logger's CrashWriter (context: the state): copies into the 64 KiB buffer, writing it out
// each time it is full.
void writeSnapshotBytes(void* context, const void* data, std::size_t size) {
    CrashState& crash = *static_cast<CrashState*>(context);
    const auto* bytes = static_cast<const char*>(data);
    while (size > 0 && !crash.writeFailed) {
        std::size_t room = WRITE_BUFFER_BYTES - crash.buffered;
        if (room == 0) {
            flushBuffer(crash);
            room = WRITE_BUFFER_BYTES;
            continue;
        }
        const std::size_t count = size < room ? size : room;
        for (std::size_t i = 0; i < count; ++i) {
            crash.buffer[crash.buffered + i] = bytes[i];
        }
        crash.buffered += count;
        bytes += count;
        size -= count;
    }
}

// "<app>: crash exception 0x<8 hex digits>" on stderr, from the owner's exception record, or
// "<app>: crash abort" without one.
void writeCauseLine() noexcept {
    const EXCEPTION_RECORD* record =
        state.pointers != nullptr ? state.pointers->ExceptionRecord : nullptr;
    const std::span<char> line(state.line);
    std::size_t length = 0;
    bool fits = appendText(line, length, std::string_view(state.app, state.appLength));
    if (record != nullptr) {
        fits = fits && appendText(line, length, ": crash exception 0x") &&
               appendNumberAt(line, length, record->ExceptionCode, 16, 8);
    } else {
        fits = fits && appendText(line, length, ": crash abort");
    }
    if (fits && appendText(line, length, "\n")) {
        writeLine(length);
    }
}

// CreateDirectoryW of every folder above the crash folder (after its root) and of the folder
// itself; every error is ignored (ERROR_ALREADY_EXISTS is the usual one): the CREATE_NEW of a file
// reports a real failure.
void makeFolders() noexcept {
    for (std::size_t i = state.rootLength; i < state.folderLength; ++i) {
        const wchar_t c = state.folder[i];
        if (c == L'\\' || c == L'/') {
            state.folder[i] = L'\0';
            static_cast<void>(CreateDirectoryW(state.folder, nullptr));
            state.folder[i] = c;
        }
    }
    static_cast<void>(CreateDirectoryW(state.folder, nullptr));
}

// "<process>-<yyyymmdd-hhmmss>-<pid>" for the crash time (local) into the stem buffer; false
// when it does not fit.
bool formatStem() noexcept {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    const std::span<char> stem(state.stem);
    std::size_t length = 0;
    const bool fits =
        appendText(stem, length, std::string_view(state.exe, state.exeLength)) &&
        appendText(stem, length, "-") && appendNumberAt(stem, length, time.wYear, 10, 4) &&
        appendNumberAt(stem, length, time.wMonth, 10, 2) &&
        appendNumberAt(stem, length, time.wDay, 10, 2) && appendText(stem, length, "-") &&
        appendNumberAt(stem, length, time.wHour, 10, 2) &&
        appendNumberAt(stem, length, time.wMinute, 10, 2) &&
        appendNumberAt(stem, length, time.wSecond, 10, 2) && appendText(stem, length, "-") &&
        appendNumberAt(stem, length, GetCurrentProcessId(), 10, 1);
    state.stemLength = fits ? length : 0;
    return fits;
}

// <folder>\<stem><extension> and a NUL into `path`; false when it does not fit.
bool formatPath(std::span<wchar_t> path, std::string_view extension) noexcept {
    std::size_t length = 0;
    const auto append = [&](wchar_t c) {
        if (length >= path.size()) {
            return false;
        }
        path[length] = c;
        ++length;
        return true;
    };
    for (std::size_t i = 0; i < state.folderLength; ++i) {
        if (!append(state.folder[i])) {
            return false;
        }
    }
    if (!append(L'\\')) {
        return false;
    }
    // The stem and the extension are ASCII.
    for (std::size_t i = 0; i < state.stemLength; ++i) {
        if (!append(static_cast<wchar_t>(state.stem[i]))) {
            return false;
        }
    }
    for (const char c : extension) {
        if (!append(static_cast<wchar_t>(c))) {
            return false;
        }
    }
    return append(L'\0');
}

// "<app>: <what> <path>" on stderr, with the path buffer in UTF-8.
void writePathLine(std::string_view what) noexcept {
    const int converted = WideCharToMultiByte(CP_UTF8, 0, state.path, -1, state.utf8,
                                              static_cast<int>(UTF8_CAPACITY), nullptr, nullptr);
    if (converted <= 0) {
        return;
    }
    const std::span<char> line(state.line);
    std::size_t length = 0;
    // `converted` counts the NUL.
    if (appendText(line, length, std::string_view(state.app, state.appLength)) &&
        appendText(line, length, ": ") && appendText(line, length, what) &&
        appendText(line, length, " ") &&
        appendText(line, length,
                   std::string_view(state.utf8, static_cast<std::size_t>(converted) - 1)) &&
        appendText(line, length, "\n")) {
        writeLine(length);
    }
}

// The logger's crash snapshot to <stem>.dvvlog.part, renamed to <stem>.dvvlog once it is written
// (so the report of a process that starts meanwhile never sees it half written), then "<app>:
// crash log <path>". A failure skips the rest of this file only.
void writeSnapshotFile() noexcept {
    if (state.logger == nullptr || !formatPath(state.partPath, ".dvvlog.part") ||
        !formatPath(state.path, ".dvvlog")) {
        return;
    }
    state.file = CreateFileW(state.partPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (state.file == INVALID_HANDLE_VALUE) {
        return;
    }
    state.writeFailed = false;
    state.buffered = 0;
    state.logger->writeCrashSnapshot(&writeSnapshotBytes, &state);
    flushBuffer(state);
    const bool closed = CloseHandle(state.file) != FALSE;
    state.file = INVALID_HANDLE_VALUE;
    // Renamed also after a failed write: the next start reports a cut-short snapshot as well.
    // Without MOVEFILE_REPLACE_EXISTING the move never replaces another file.
    const bool renamed = MoveFileExW(state.partPath, state.path, 0) != FALSE;
    if (!state.writeFailed && closed && renamed) {
        writePathLine("crash log");
    }
}

// The minidump of the owner's exception to <stem>.dmp, then "<app>: crash dump <path>"; for
// abort() (no exception pointers) a dump of every thread with no exception stream. A failure skips
// the rest of this file only. Nothing without MiniDumpWriteDump (installBackend's "crash dumps are
// off").
void writeDumpFile() noexcept {
    if (state.writeDump == nullptr || !formatPath(state.path, ".dmp")) {
        return;
    }
    const HANDLE file = CreateFileW(state.path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    // The faulting thread's id, not the dump thread's: the dump shows the crash as the exception.
    MINIDUMP_EXCEPTION_INFORMATION exception{state.faultingThreadId, state.pointers, FALSE};
    MINIDUMP_EXCEPTION_INFORMATION* const exception_param =
        state.pointers != nullptr ? &exception : nullptr;
    const BOOL written = state.writeDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                         MiniDumpNormal, exception_param, nullptr, nullptr);
    const bool closed = CloseHandle(file) != FALSE;
    if (written != FALSE && closed) {
        writePathLine("crash dump");
    }
}

// The cause line, the folder chain, the snapshot, then the minidump, with no logger call, lock or
// allocation. Runs on the dump thread (or, when that is gone, on the crashing thread). The
// snapshot comes first: it needs no foreign DLL, so it is on disk when dbghelp faults or hangs.
void writeCrashFiles() noexcept {
    writeCauseLine();
    makeFolders();
    if (!formatStem()) {
        return;
    }
    writeSnapshotFile();
    writeDumpFile();
}

DWORD WINAPI dumpThread(void* /*unused*/) {
    WaitForSingleObject(state.request, INFINITE);
    writeCrashFiles();
    SetEvent(state.done);
    // Never returns: a thread exit (DLL_THREAD_DETACH) could fault while the crashing thread ends
    // the process and replace its exit code. TerminateProcess ends this thread.
    Sleep(INFINITE);
    return 0;
}

// Makes the calling thread the crash's owner. False on the dump thread or when this thread owns
// the crash already (a crash inside the handler): the caller ends the process at once. When
// another thread owns it, this one waits: that thread writes and ends the process; never hand it
// to WER. If that has not happened after WAITER_TIMEOUT_MS, this thread ends the process with
// `exit_code`. Small stack use only, as its callers.
bool takeCrash(DWORD self, UINT exit_code) noexcept {
    if (self == state.dumpThreadId) {
        return false;
    }
    DWORD expected = 0;
    if (!state.owner.compare_exchange_strong(expected, self)) {
        if (expected == self) {
            return false;
        }
        Sleep(WAITER_TIMEOUT_MS);
        TerminateProcess(GetCurrentProcess(), exit_code);
        return false;  // fallback, if TerminateProcess returns: the caller ends the process
    }
    return true;
}

// The owner's part: wakes the dump thread for `pointers` (nullptr for abort()) and waits for it
// within the bound, or writes on this thread when there is no dump thread.
void writeOwnedCrash(EXCEPTION_POINTERS* pointers, DWORD self) noexcept {
    state.pointers = pointers;
    state.faultingThreadId = self;
    if (state.thread != nullptr && WaitForSingleObject(state.thread, 0) == WAIT_TIMEOUT) {
        // The dump thread is alive.
        SetEvent(state.request);
        const HANDLE waits[2] = {state.done, state.thread};
        WaitForMultipleObjects(2, waits, FALSE, DUMP_TIMEOUT_MS);
    } else {
        // No dump thread was made (then there is no minidump either), or it is gone (ExitProcess
        // ends every other thread before the DLL detach): write on this thread.
        writeCrashFiles();
    }
}

// Small stack use only: a stack overflow runs this on a thread with little stack left.
LONG WINAPI onUnhandledException(EXCEPTION_POINTERS* pointers) {
    const DWORD self = GetCurrentThreadId();
    if (!takeCrash(self, pointers->ExceptionRecord->ExceptionCode)) {
        return EXCEPTION_EXECUTE_HANDLER;  // the dump or the filter itself crashed: end the process
    }
    writeOwnedCrash(pointers, self);
    // End the process here with the exception code as its exit code: no unwind (on a stack that
    // may be exhausted), no DLL detach, no WER dialog.
    TerminateProcess(GetCurrentProcess(), pointers->ExceptionRecord->ExceptionCode);
    return EXCEPTION_EXECUTE_HANDLER;  // fallback: the OS ends the process with the exception code
}

// The CRT's SIGABRT handler. abort() raises SIGABRT on the aborting thread before it would hand
// the process to WER, so abort(), std::terminate, a noexcept violation and an exception that
// leaves a std::thread function end here. The filter's rules, with no exception pointers and the
// CRT's abort exit code 3.
void onAbortSignal(int /*signal*/) {
    // The CRT resets SIGABRT to SIG_DFL before it calls this; set it again, so an abort() on
    // another thread meanwhile also comes here and waits instead of ending the process at once.
    static_cast<void>(std::signal(SIGABRT, &onAbortSignal));
    const DWORD self = GetCurrentThreadId();
    if (takeCrash(self, ABORT_EXIT_CODE)) {
        writeOwnedCrash(nullptr, self);
    }
    TerminateProcess(GetCurrentProcess(), ABORT_EXIT_CODE);
    // Fallback, if TerminateProcess returns: abort() goes on to its own end, by default a fast fail
    // to Windows Error Reporting (0xC0000409), not exit code 3.
}

// Copies `text` and a NUL into `target`; false when it does not fit.
template <class Char, std::size_t N>
bool copyText(Char (&target)[N], std::basic_string_view<Char> text) noexcept {
    if (text.size() >= N) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        target[i] = text[i];
    }
    target[text.size()] = Char{};
    return true;
}

// Recurses until the stack overflows (EXCEPTION_STACK_OVERFLOW); `depth` never gets negative, so
// every call recurses, and adding frame[0] to the result keeps each call from being a tail call.
int overflowStack(int depth) {
    volatile char frame[4096];
    frame[0] = static_cast<char>(depth);
    if (depth >= 0) {
        return overflowStack(depth + 1) + frame[0];
    }
    return frame[0];
}

// Four threads raise an access violation at the same moment; the first one's files must win.
void crashOnThreads() {
    std::atomic<bool> go{false};
    std::array<std::thread, 4> threads;
    for (std::thread& thread : threads) {
        thread = std::thread([&go] {
            while (!go.load()) {
            }
            RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
        });
    }
    go.store(true);
    for (std::thread& thread : threads) {
        thread.join();
    }
}

// Raises an access violation in its destructor when armed. Constant-initialized, so its
// destructor runs after the destructors of every static created later.
struct ExitCrash {
    std::atomic<bool> armed{false};
    constexpr ExitCrash() = default;
    ExitCrash(const ExitCrash&) = delete;
    ExitCrash& operator=(const ExitCrash&) = delete;
    ExitCrash(ExitCrash&&) = delete;
    ExitCrash& operator=(ExitCrash&&) = delete;
    ~ExitCrash() {
        if (armed.load()) {
            RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
        }
    }
};
constinit ExitCrash exit_crash;

// Exits 99 unless installBackend's stack guarantee is in effect on this thread: without it a stack
// overflow can end as an access violation. First writes "<app>: stack guarantee missing" on stderr
// when installBackend stored the app name (the handler is on), as the handler's other lines do.
void requireStackGuarantee() {
    ULONG guarantee = 0;  // 0: the call only reports the current guarantee
    if (SetThreadStackGuarantee(&guarantee) != FALSE && guarantee >= OVERFLOW_STACK_GUARANTEE) {
        return;
    }
    std::array<char, NAME_CAPACITY + 32> text{};
    std::size_t length = 0;
    if (state.appLength != 0 &&
        appendText(text, length, std::string_view(state.app, state.appLength)) &&
        appendText(text, length, ": stack guarantee missing\n")) {
        static_cast<void>(writeAll(GetStdHandle(STD_ERROR_HANDLE), text.data(), length));
    }
    ExitProcess(GUARANTEE_MISSING_EXIT_CODE);
}

}  // namespace

BackendStatus installBackend(const CrashBackendOptions& options) {
    const std::wstring& folder = options.folder.native();
    if (!copyText(state.folder, std::wstring_view(folder))) {
        return {.off = "the crash folder path is too long"};
    }
    state.folderLength = folder.size();
    state.rootLength = options.folder.root_path().native().size();
    // §4.9 step 2 limits both names to 63 bytes, so they fit; a name that did not would be empty.
    state.appLength = copyText(state.app, std::string_view(options.app)) ? options.app.size() : 0;
    state.exeLength =
        copyText(state.exe, std::string_view(options.process)) ? options.process.size() : 0;
    state.logger = options.logger;
    state.file = INVALID_HANDLE_VALUE;

    // The minidump needs dbghelp and the dump thread; without either the handler stays on for the
    // cause line and the snapshot, and only the minidump is off (the first reason is reported).
    std::string no_dumps;
    WriteDump write_dump = nullptr;
    auto library = DynamicLibrary::open("dbghelp.dll", LibrarySearch::SystemDirectory);
    if (!library) {
        no_dumps = library.error().describe();
    } else {
        write_dump = library->function<WriteDump>("MiniDumpWriteDump");
        if (write_dump == nullptr) {
            no_dumps = "MiniDumpWriteDump not found";
        }
    }
    // Process-lifetime handles, never closed (the filter may run at any time until the end).
    state.request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    state.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (state.request == nullptr || state.done == nullptr) {
        if (no_dumps.empty()) {
            no_dumps = "CreateEventW failed (error " + std::to_string(GetLastError()) + ")";
        }
    } else {
        state.thread = CreateThread(nullptr, DUMP_THREAD_STACK_BYTES, &dumpThread, nullptr,
                                    STACK_SIZE_PARAM_IS_A_RESERVATION, &state.dumpThreadId);
        if (state.thread == nullptr && no_dumps.empty()) {
            no_dumps = "CreateThread failed (error " + std::to_string(GetLastError()) + ")";
        }
    }
    if (state.thread == nullptr) {
        write_dump = nullptr;  // never MiniDumpWriteDump on the crashing thread's stack
    }
    // A stack overflow is dispatched, filtered (onUnhandledException) and unwound on the
    // overflowing thread's last pages, which by default leave only a few KB; without more, that
    // path can fault again and the process ends with an access violation instead. This covers the
    // calling thread (the main thread) only: other threads would need their own call. A failure
    // keeps the default guarantee, and the handler stays on.
    ULONG guarantee = OVERFLOW_STACK_GUARANTEE;
    static_cast<void>(SetThreadStackGuarantee(&guarantee));
    if (write_dump != nullptr) {
        library->keepLoaded();
        state.writeDump = write_dump;
    }
    SetUnhandledExceptionFilter(&onUnhandledException);
    // SIGABRT is a valid signal, so signal() cannot fail here.
    static_cast<void>(std::signal(SIGABRT, &onAbortSignal));
    return {.noDumps = std::move(no_dumps)};
}

void crashNow(std::string_view mode) {
    if (mode == "1") {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    } else if (mode == "stack") {
        requireStackGuarantee();
        static_cast<void>(overflowStack(0));
    } else if (mode == "threads") {
        crashOnThreads();
    } else if (mode == "exit") {
        exit_crash.armed.store(true);  // the process faults after main(), in the static destructors
    } else if (mode == "abort" || mode == "terminate") {
        // A Debug CRT would show its abort message box before it raises SIGABRT.
        static_cast<void>(_set_abort_behavior(0, _WRITE_ABORT_MSG));
        if (mode == "abort") {
            std::abort();
        }
        std::terminate();
    }
}

}  // namespace dvv::platform::detail
