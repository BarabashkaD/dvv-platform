#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "dvv/base/error.h"
#include "dvv/base/log.h"
#include "dvv/platform/file_reader.h"
#include "file_reader_detail.h"

namespace {

namespace fs = std::filesystem;
using dvv::platform::allocateAligned;
using dvv::platform::FileReader;
using dvv::platform::FileReadMode;
using dvv::platform::detail::NativeOpen;
using dvv::platform::detail::NativeOps;
using dvv::platform::detail::NativeRead;
using dvv::platform::detail::NO_HANDLE;

// A unique directory under the system temporary directory, removed with its contents.
class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::random_device random;
        const fs::path base = fs::temp_directory_path();
        for (int attempt = 0; attempt < 100; ++attempt) {
            fs::path candidate =
                base / std::format("dvv-file-reader-{:08x}{:08x}", random(), random());
            std::error_code error;
            if (fs::create_directory(candidate, error)) {
                path_ = std::move(candidate);
                return;
            }
        }
        FAIL("no temporary directory could be created");
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    TemporaryDirectory(TemporaryDirectory&&) = delete;
    TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;
    ~TemporaryDirectory() {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

std::byte pattern(std::uint64_t i) {
    return static_cast<std::byte>((i * 131 + (i >> 8) * 7) & 0xFF);
}

std::vector<std::byte> patternBytes(std::size_t count) {
    std::vector<std::byte> bytes(count);
    for (std::size_t i = 0; i < count; ++i) {
        bytes[i] = pattern(i);
    }
    return bytes;
}

void writeFile(const fs::path& path, std::span<const std::byte> bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.is_open());
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    REQUIRE(stream.good());
}

// Whether `bytes` equal the pattern starting at file offset `offset`.
bool matchesPattern(std::span<const std::byte> bytes, std::uint64_t offset) {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] != pattern(offset + i)) {
            return false;
        }
    }
    return true;
}

std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

std::uint64_t alignDown(std::uint64_t value, std::uint64_t alignment) {
    return value / alignment * alignment;
}

std::string utf8(const std::u8string& text) {
    std::string result;
    result.reserve(text.size());
    for (const char8_t c : text) {
        result.push_back(static_cast<char>(c));
    }
    return result;
}

FileReader openReader(const fs::path& path, FileReadMode mode) {
    auto reader = FileReader::open(path, mode);
    REQUIRE_MESSAGE(reader.has_value(), reader.error().describe());
    return std::move(reader).value();
}

FileReader openReaderWith(const fs::path& path, FileReadMode mode, const NativeOps& ops) {
    auto reader = dvv::platform::detail::openWith(path, mode, ops);
    REQUIRE_MESSAGE(reader.has_value(), reader.error().describe());
    return std::move(reader).value();
}

dvv::platform::AlignedBuffer alignedBuffer(std::size_t bytes, std::size_t alignment) {
    auto buffer = allocateAligned(bytes, alignment);
    REQUIRE_MESSAGE(buffer.has_value(), buffer.error().describe());
    return std::move(buffer).value();
}

// The whole file through `reader`, in one read of the aligned superset.
std::vector<std::byte> readWhole(const FileReader& reader) {
    const std::size_t a = reader.alignment();
    const auto total =
        static_cast<std::size_t>(alignUp(std::max<std::uint64_t>(reader.size(), 1), a));
    const auto buffer = alignedBuffer(total, a);
    const auto bytes = reader.readAt(0, std::span(buffer.get(), total));
    REQUIRE_MESSAGE(bytes.has_value(), bytes.error().describe());
    REQUIRE(*bytes == reader.size());
    return {buffer.get(), buffer.get() + *bytes};
}

// Restores the logger's level and removes the test's sinks (the tests add no other sinks).
class LoggerScope {
public:
    LoggerScope() : level_(dvv::Logger::instance().level()) {
        dvv::Logger::instance().clearSinks();
        dvv::Logger::instance().addSink(sink_);
        dvv::Logger::instance().setLevel(dvv::LogLevel::Info);
    }
    LoggerScope(const LoggerScope&) = delete;
    LoggerScope& operator=(const LoggerScope&) = delete;
    LoggerScope(LoggerScope&&) = delete;
    LoggerScope& operator=(LoggerScope&&) = delete;
    ~LoggerScope() {
        dvv::Logger::instance().clearSinks();
        dvv::Logger::instance().setLevel(level_);
    }

    [[nodiscard]] std::size_t count(std::string_view text) const {
        const std::vector<std::string> lines = sink_->lines();
        return static_cast<std::size_t>(std::ranges::count_if(
            lines, [text](const std::string& line) { return line.contains(text); }));
    }

private:
    dvv::LogLevel level_;
    std::shared_ptr<dvv::MemorySink> sink_ = std::make_shared<dvv::MemorySink>();
};

// Injected failures (the NativeOps seam takes plain function pointers).
constexpr int INJECTED_ERROR = 5;  // ERROR_ACCESS_DENIED / EIO

NativeOpen refuseUnbufferedOpen(const fs::path& path, bool unbuffered) noexcept {
    if (unbuffered) {
        return NativeOpen{.handle = NO_HANDLE, .error = INJECTED_ERROR};
    }
    return dvv::platform::detail::openNative(path, false);
}

std::uint32_t unknownSector(std::intptr_t /*handle*/) noexcept {
    return 0;
}

std::uint32_t unusualSector(std::intptr_t /*handle*/) noexcept {
    return 520;
}

// The real sector size where the platform's query works; 4096 elsewhere (tests that force
// unbuffered reads off Windows).
std::uint32_t knownSector(std::intptr_t handle) noexcept {
    const std::uint32_t sector = dvv::platform::detail::sectorSize(handle);
    return sector != 0 ? sector : 4096;
}

std::atomic<bool> fail_next_read{false};

NativeRead failFirstRead(std::intptr_t handle, std::uint64_t offset,
                         std::span<std::byte> destination, std::uint64_t file_size) noexcept {
    if (fail_next_read.exchange(false)) {
        return NativeRead{.bytes = 0, .error = INJECTED_ERROR};
    }
    return dvv::platform::detail::readNative(handle, offset, destination, file_size);
}

std::atomic<bool> short_next_read{false};

// The first read returns one byte fewer than it read (a short probe).
NativeRead shortFirstRead(std::intptr_t handle, std::uint64_t offset,
                          std::span<std::byte> destination, std::uint64_t file_size) noexcept {
    NativeRead read = dvv::platform::detail::readNative(handle, offset, destination, file_size);
    if (short_next_read.exchange(false) && read.bytes > 0) {
        --read.bytes;
    }
    return read;
}

// The handles this process holds (Windows); 0 elsewhere, where the checks below compare 0 == 0.
std::uint32_t processHandles() {
#if defined(_WIN32)
    DWORD count = 0;
    GetProcessHandleCount(GetCurrentProcess(), &count);
    return count;
#else
    return 0;
#endif
}

// Counts reads in flight; the sleep widens the window in which a second read could overlap.
std::atomic<int> reads_in_flight{0};
std::atomic<int> max_reads_in_flight{0};
std::atomic<int> reads_done{0};

NativeRead countingRead(std::intptr_t handle, std::uint64_t offset,
                        std::span<std::byte> destination, std::uint64_t file_size) noexcept {
    const int now = reads_in_flight.fetch_add(1) + 1;
    int seen = max_reads_in_flight.load();
    while (now > seen && !max_reads_in_flight.compare_exchange_weak(seen, now)) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const NativeRead read =
        dvv::platform::detail::readNative(handle, offset, destination, file_size);
    reads_in_flight.fetch_sub(1);
    reads_done.fetch_add(1);
    return read;
}

// The ops of Auto as on Windows, with one injected failure.
NativeOps autoOps() {
    NativeOps ops;
    ops.autoTriesUnbuffered = true;
    ops.sectorSize = knownSector;
    return ops;
}

}  // namespace

TEST_SUITE("file_reader") {
    TEST_CASE("allocateAligned returns aligned memory") {
        const std::array<std::pair<std::size_t, std::size_t>, 4> cases{
            {{1, 1}, {100, 512}, {8192, 4096}, {5000, 65536}}};
        for (const auto& [bytes, alignment] : cases) {
            CAPTURE(bytes);
            CAPTURE(alignment);
            const auto buffer = allocateAligned(bytes, alignment);
            REQUIRE(buffer.has_value());
            CHECK(reinterpret_cast<std::uintptr_t>(buffer->get()) % alignment == 0);
        }
        const auto empty = allocateAligned(0, 64);
        REQUIRE_FALSE(empty.has_value());
        CHECK(empty.error().code == dvv::Errc::Internal);
        const auto odd = allocateAligned(64, 3);
        REQUIRE_FALSE(odd.has_value());
        CHECK(odd.error().code == dvv::Errc::Internal);
    }

    TEST_CASE("a buffered reader reads any range") {
        const TemporaryDirectory dir;
        const fs::path file = dir.path() / "buffered.bin";
        writeFile(file, patternBytes(10000));
        const FileReader reader = openReader(file, FileReadMode::Buffered);
        CHECK(reader.size() == 10000);
        CHECK_FALSE(reader.unbuffered());
        CHECK(reader.alignment() == 1);
        CHECK(reader.fallbackReason().empty());
        CHECK(reader.modeText() == "buffered");
        CHECK(reader.path() == file);

        std::vector<std::byte> buffer(100);
        const auto middle = reader.readAt(3, buffer);
        REQUIRE(middle.has_value());
        CHECK(*middle == 100);
        CHECK(matchesPattern(buffer, 3));

        const auto tail = reader.readAt(9990, buffer);
        REQUIRE(tail.has_value());
        CHECK(*tail == 10);
        CHECK(matchesPattern(std::span(buffer).first(10), 9990));

        std::vector<std::byte> small(16);
        const auto at_end = reader.readAt(10000, small);
        REQUIRE(at_end.has_value());
        CHECK(*at_end == 0);
        const auto past_end = reader.readAt(20000, small);
        REQUIRE(past_end.has_value());
        CHECK(*past_end == 0);
    }

    // Buffered reads go through the file cache: no offset, size or address alignment is needed.
    TEST_CASE("unaligned buffered offsets, sizes and addresses read") {
        const TemporaryDirectory dir;
        const fs::path file = dir.path() / "unaligned.bin";
        constexpr std::size_t SIZE = 3 * 4096 + 123;
        writeFile(file, patternBytes(SIZE));
        const FileReader reader = openReader(file, FileReadMode::Buffered);
        const auto buffer = alignedBuffer(SIZE + 1, 4096);
        const std::span<std::byte> odd(buffer.get() + 1, 4097);  // odd address and size
        const auto bytes = reader.readAt(4095, odd);
        REQUIRE_MESSAGE(bytes.has_value(), bytes.error().describe());
        CHECK(*bytes == 4097);
        CHECK(matchesPattern(odd, 4095));
    }

    TEST_CASE("an unbuffered reader reads aligned ranges up to the partial last sector") {
        const TemporaryDirectory dir;
        const fs::path file = dir.path() / "unbuffered.bin";
        constexpr std::size_t SIZE = 3 * 4096 + 123;
        writeFile(file, patternBytes(SIZE));
        NativeOps ops;
        ops.sectorSize = knownSector;  // Linux without STATX_DIOALIGN still checks the path
        auto opened = dvv::platform::detail::openWith(file, FileReadMode::Unbuffered, ops);
#if !defined(_WIN32)
        if (!opened) {
            MESSAGE("O_DIRECT refused here: " << opened.error().describe());
            return;
        }
#endif
        REQUIRE_MESSAGE(opened.has_value(), opened.error().describe());  // NTFS temp directory
        const FileReader& reader = *opened;
        REQUIRE(reader.unbuffered());
        CHECK(reader.modeText() == "unbuffered");
        CHECK(reader.size() == SIZE);
        const std::uint32_t a = reader.alignment();
        CHECK(a >= 4096);
        CHECK(a % 512 == 0);
        const auto total = static_cast<std::size_t>(alignUp(SIZE, a));
        const auto buffer = alignedBuffer(total, a);
        const std::span<std::byte> all(buffer.get(), total);

        const auto whole = reader.readAt(0, all);
        REQUIRE(whole.has_value());
        CHECK(*whole == SIZE);
        CHECK(matchesPattern(all.first(SIZE), 0));

        const auto second = reader.readAt(a, all.first(a));
        REQUIRE(second.has_value());
        CHECK(*second == a);
        CHECK(matchesPattern(all.first(a), a));

        const std::uint64_t last = alignDown(SIZE, a);
        const auto tail = reader.readAt(last, all.first(a));
        REQUIRE(tail.has_value());
        CHECK(*tail == SIZE % a);
        CHECK(matchesPattern(all.first(SIZE % a), last));

        const auto at_end = reader.readAt(alignUp(SIZE, a), all.first(a));
        REQUIRE(at_end.has_value());
        CHECK(*at_end == 0);
    }

    TEST_CASE("every mode reads the same bytes as the reference") {
        const TemporaryDirectory dir;
        const fs::path file = dir.path() / "modes.bin";
        constexpr std::size_t SIZE = std::size_t{5} * 65536 + 777;
        const std::vector<std::byte> reference = patternBytes(SIZE);
        writeFile(file, reference);
        const FileReader buffered = openReader(file, FileReadMode::Buffered);
        const FileReader automatic = openReader(file, FileReadMode::Auto);
        CHECK(readWhole(buffered) == reference);
        CHECK(readWhole(automatic) == reference);
#if defined(_WIN32)
        CHECK(automatic.unbuffered());  // Auto on an NTFS temporary directory
        const FileReader unbuffered = openReader(file, FileReadMode::Unbuffered);
        CHECK(unbuffered.unbuffered());
        CHECK(readWhole(unbuffered) == reference);
#else
        CHECK_FALSE(automatic.unbuffered());  // Auto is Buffered off Windows
        CHECK(automatic.modeText() == "buffered");
#endif
    }

    TEST_CASE("a misaligned unbuffered read is Internal") {
        const TemporaryDirectory dir;
        const fs::path file = dir.path() / "misaligned.bin";
        writeFile(file, patternBytes(std::size_t{3} * 4096));
        NativeOps ops;
        ops.sectorSize = knownSector;
        auto opened = dvv::platform::detail::openWith(file, FileReadMode::Unbuffered, ops);
        if (!opened) {
            MESSAGE("unbuffered reads refused here: " << opened.error().describe());
            return;
        }
        const FileReader& reader = *opened;
        const std::uint32_t a = reader.alignment();
        const auto buffer = alignedBuffer(std::size_t{2} * a, a);
        const auto check = [](const dvv::Result<std::size_t>& result) {
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().code == dvv::Errc::Internal);
            CHECK(result.error().message.find("misaligned") != std::string::npos);
        };
        check(reader.readAt(1, std::span(buffer.get(), a)));
        check(reader.readAt(0, std::span(buffer.get(), a - 1)));
        check(reader.readAt(0, std::span(buffer.get() + 1, a)));
    }

#if defined(_WIN32)
    TEST_CASE("the Windows open flags of each mode") {
        const std::uint32_t buffered = dvv::platform::detail::nativeOpenFlags(false);
        CHECK((buffered & FILE_FLAG_SEQUENTIAL_SCAN) != 0);
        CHECK((buffered & FILE_FLAG_OVERLAPPED) == 0);  // synchronous handles
        CHECK((buffered & FILE_FLAG_NO_BUFFERING) == 0);
        const std::uint32_t unbuffered = dvv::platform::detail::nativeOpenFlags(true);
        CHECK((unbuffered & FILE_FLAG_NO_BUFFERING) != 0);
        CHECK((unbuffered & FILE_FLAG_SEQUENTIAL_SCAN) == 0);
        CHECK((unbuffered & FILE_FLAG_OVERLAPPED) == 0);
        CHECK(dvv::platform::detail::AUTO_TRIES_UNBUFFERED);
    }
#endif

    TEST_CASE("Auto falls back to buffered when the open-time check fails") {
        const TemporaryDirectory dir;
        const fs::path file = dir.path() / "fallback.bin";
        writeFile(file, patternBytes(10000));

        NativeOps ops = autoOps();
        std::string reason;
        bool fail_read = false;
        bool short_read = false;
        SUBCASE("the unbuffered open fails") {
            ops.open = refuseUnbufferedOpen;
            reason = "the unbuffered open failed";
        }
        SUBCASE("the sector size is unknown") {
            ops.sectorSize = unknownSector;
            reason = "sector size is unknown";
        }
        SUBCASE("the sector size is unusual") {
            ops.sectorSize = unusualSector;
            reason = "unusual sector size 520";
        }
        SUBCASE("the first aligned read fails") {
            ops.read = failFirstRead;
            fail_read = true;
            reason = "the first aligned read failed";
        }
        SUBCASE("the first aligned read is short") {
            ops.read = shortFirstRead;
            short_read = true;
            reason = "the first aligned read returned";
        }
        CAPTURE(reason);
        const auto open = [&] {
            fail_next_read = fail_read;
            short_next_read = short_read;
            FileReader opened = openReaderWith(file, FileReadMode::Auto, ops);
            fail_next_read = false;
            short_next_read = false;
            return opened;
        };
        // The abandoned unbuffered handle is closed: 16 opens and closes leave at most a few
        // handles that Windows creates lazily, not one leaked handle per open.
        const std::uint32_t handles = processHandles();
        for (int attempt = 0; attempt < 16; ++attempt) {
            const FileReader closed = open();
        }
        CHECK(processHandles() < handles + 8);

        const LoggerScope log;
        const FileReader reader = open();
        CHECK_FALSE(reader.unbuffered());
        CHECK(reader.alignment() == 1);
        CHECK(reader.fallbackReason().contains(reason));
        CHECK(reader.modeText() == "buffered (fallback: " + reader.fallbackReason() + ")");
        CHECK(log.count("reading buffered") == 1);
        CHECK(log.count(reason) == 1);

        std::array<std::byte, 7> buffer{};
        const auto bytes = reader.readAt(5, buffer);  // unaligned: a buffered read
        REQUIRE_MESSAGE(bytes.has_value(), bytes.error().describe());
        CHECK(*bytes == 7);
        CHECK(matchesPattern(buffer, 5));
    }

    TEST_CASE("an explicit Unbuffered whose check fails is Io, not a fallback") {
        const TemporaryDirectory dir;
        const fs::path file = dir.path() / "refused.bin";
        writeFile(file, patternBytes(10000));

        NativeOps ops = autoOps();
        std::string reason;
        SUBCASE("the unbuffered open fails") {
            ops.open = refuseUnbufferedOpen;
            reason = "the unbuffered open failed";
        }
        SUBCASE("the sector size is unknown") {
            ops.sectorSize = unknownSector;
            reason = "sector size is unknown";
        }
        SUBCASE("the first aligned read fails") {
            ops.read = failFirstRead;
            fail_next_read = true;
            reason = "the first aligned read failed";
        }
        SUBCASE("the sector size is unusual") {
            ops.sectorSize = unusualSector;
            reason = "unusual sector size 520";
        }
        SUBCASE("the first aligned read is short") {
            ops.read = shortFirstRead;
            short_next_read = true;
            reason = "the first aligned read returned";
        }
        CAPTURE(reason);
        const LoggerScope log;
        const auto reader = dvv::platform::detail::openWith(file, FileReadMode::Unbuffered, ops);
        fail_next_read = false;
        short_next_read = false;
        REQUIRE_FALSE(reader.has_value());
        CHECK(reader.error().code == dvv::Errc::Io);
        CHECK(reader.error().message.contains("refused.bin"));
        CHECK(reader.error().message.contains("cannot be read unbuffered"));
        CHECK(reader.error().message.contains(reason));
        CHECK(log.count("reading buffered") == 0);
    }

    TEST_CASE("Auto on a file that cannot be opened reports the open error without a fallback") {
        const TemporaryDirectory dir;
        const LoggerScope log;
        const auto reader = dvv::platform::detail::openWith(dir.path() / "missing.bin",
                                                            FileReadMode::Auto, autoOps());
        REQUIRE_FALSE(reader.has_value());
        CHECK(reader.error().code == dvv::Errc::Io);
        CHECK(reader.error().message.contains("could not be opened"));
        CHECK(log.count("reading buffered") == 0);
    }

    TEST_CASE("Auto where unbuffered reads are not tried is plain buffered") {
        const TemporaryDirectory dir;
        const fs::path file = dir.path() / "portable.bin";
        writeFile(file, patternBytes(100));
        NativeOps ops;
        ops.autoTriesUnbuffered = false;
        const LoggerScope log;
        const FileReader reader = openReaderWith(file, FileReadMode::Auto, ops);
        CHECK_FALSE(reader.unbuffered());
        CHECK(reader.fallbackReason().empty());
        CHECK(reader.modeText() == "buffered");
        CHECK(log.count("reading buffered") == 0);
    }

    // U-IO2: a file source reads one request at a time. The counting read sleeps, so overlapping
    // calls would be seen.
    TEST_CASE("a reader never has more than one read in flight") {
        const TemporaryDirectory dir;
        const fs::path file = dir.path() / "one.bin";
        constexpr std::size_t SIZE = std::size_t{1} << 20;
        constexpr std::size_t BLOCK = 65536;
        constexpr int THREADS = 8;
        constexpr int READS = 16;
        writeFile(file, patternBytes(SIZE));

        FileReadMode mode = FileReadMode::Buffered;
        SUBCASE("buffered") {
            mode = FileReadMode::Buffered;
        }
        SUBCASE("auto") {
            mode = FileReadMode::Auto;
        }
        NativeOps ops = autoOps();
        ops.read = countingRead;
        const FileReader reader = openReaderWith(file, mode, ops);
        reads_in_flight = 0;
        max_reads_in_flight = 0;
        reads_done = 0;
        std::atomic<int> failures{0};
        {
            std::vector<std::jthread> threads;
            threads.reserve(THREADS);
            for (int thread = 0; thread < THREADS; ++thread) {
                threads.emplace_back([&reader, &failures, thread] {
                    const auto buffer = allocateAligned(BLOCK, 65536);
                    if (!buffer) {
                        ++failures;
                        return;
                    }
                    const std::span<std::byte> destination(buffer->get(), BLOCK);
                    for (int read = 0; read < READS; ++read) {
                        const std::uint64_t offset =
                            static_cast<std::uint64_t>((thread * READS + read) % (SIZE / BLOCK)) *
                            BLOCK;
                        const auto bytes = reader.readAt(offset, destination);
                        if (!bytes || *bytes != BLOCK || !matchesPattern(destination, offset)) {
                            ++failures;
                        }
                    }
                });
            }
        }
        CHECK(failures == 0);
        CHECK(reads_done == THREADS * READS);
        CHECK(max_reads_in_flight == 1);
    }

    TEST_CASE("eight threads read correct bytes in every mode") {
        const TemporaryDirectory dir;
        const fs::path file = dir.path() / "concurrent.bin";
        constexpr std::size_t SIZE = std::size_t{8} << 20;
        constexpr std::size_t BLOCK = 65536;
        constexpr int THREADS = 8;
        constexpr int READS = 64;
        writeFile(file, patternBytes(SIZE));
        FileReadMode mode = FileReadMode::Buffered;
        SUBCASE("buffered") {
            mode = FileReadMode::Buffered;
        }
        SUBCASE("auto") {
            mode = FileReadMode::Auto;
        }
        const FileReader reader = openReader(file, mode);
        const std::uint32_t a = reader.alignment();
        MESSAGE("mode: " << reader.modeText() << ", alignment " << a);

        std::array<int, THREADS> mismatches{};
        {
            std::vector<std::jthread> threads;
            threads.reserve(THREADS);
            for (int thread = 0; thread < THREADS; ++thread) {
                threads.emplace_back([&reader, &mismatches, thread, a] {
                    int& count = mismatches[static_cast<std::size_t>(thread)];
                    auto buffer = allocateAligned(BLOCK, a);
                    if (!buffer) {
                        ++count;
                        return;
                    }
                    const std::span<std::byte> destination(buffer->get(), BLOCK);
                    auto seed = static_cast<std::uint64_t>(thread) + 1;
                    for (int read = 0; read < READS; ++read) {
                        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
                        const std::uint64_t offset = alignDown((seed >> 33) % (SIZE - BLOCK), a);
                        const auto bytes = reader.readAt(offset, destination);
                        if (!bytes || *bytes != BLOCK || !matchesPattern(destination, offset)) {
                            ++count;
                        }
                    }
                });
            }
        }
        int total = 0;
        for (const int count : mismatches) {
            total += count;
        }
        CHECK(total == 0);
    }

    TEST_CASE("a missing file is Io naming the file") {
        const TemporaryDirectory dir;
        for (const FileReadMode mode :
             {FileReadMode::Buffered, FileReadMode::Auto, FileReadMode::Unbuffered}) {
            const auto reader = FileReader::open(dir.path() / "missing.bin", mode);
            REQUIRE_FALSE(reader.has_value());
            CHECK(reader.error().code == dvv::Errc::Io);
            CHECK(reader.error().message.find("missing.bin") != std::string::npos);
        }
        const auto buffered = FileReader::open(dir.path() / "missing.bin");
        REQUIRE_FALSE(buffered.has_value());
        CHECK(buffered.error().message.find("could not be opened") != std::string::npos);
    }

    TEST_CASE("a directory is Io") {
        const TemporaryDirectory dir;
        for (const FileReadMode mode :
             {FileReadMode::Buffered, FileReadMode::Auto, FileReadMode::Unbuffered}) {
            const auto reader = FileReader::open(dir.path(), mode);
            REQUIRE_FALSE(reader.has_value());
            CHECK(reader.error().code == dvv::Errc::Io);
        }
    }

    TEST_CASE("an empty file opens with size 0 in every mode") {
        const TemporaryDirectory dir;
        const fs::path file = dir.path() / "empty.bin";
        writeFile(file, {});
        for (const FileReadMode mode : {FileReadMode::Buffered, FileReadMode::Auto}) {
            const FileReader reader = openReader(file, mode);
            CHECK(reader.size() == 0);
            const std::uint32_t a = reader.alignment();
            const auto buffer = alignedBuffer(std::max<std::size_t>(a, 16), a);
            const auto bytes =
                reader.readAt(0, std::span(buffer.get(), std::max<std::size_t>(a, 16)));
            REQUIRE(bytes.has_value());
            CHECK(*bytes == 0);
        }
    }

    TEST_CASE("a non-ASCII file name opens") {
        const TemporaryDirectory dir;
        const fs::path file = dir.path() / fs::path(u8"frame_ü_日本.bin");
        writeFile(file, patternBytes(100));
        for (const FileReadMode mode : {FileReadMode::Buffered, FileReadMode::Auto}) {
            const FileReader reader = openReader(file, mode);
            CHECK(reader.size() == 100);
            const std::size_t a = reader.alignment();
            const auto total = static_cast<std::size_t>(alignUp(100, a));
            const auto buffer = alignedBuffer(total, a);
            const auto bytes = reader.readAt(0, std::span(buffer.get(), total));
            REQUIRE(bytes.has_value());
            CHECK(*bytes == 100);
            CHECK(matchesPattern(std::span(buffer.get(), 100), 0));
        }

        const auto missing = FileReader::open(dir.path() / fs::path(u8"missing_ü.bin"));
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().code == dvv::Errc::Io);
        CHECK(missing.error().message.find(utf8(u8"missing_ü.bin")) != std::string::npos);
        CHECK(missing.error().message.find(utf8(u8"ü")) != std::string::npos);
    }

    TEST_CASE("a moved reader keeps the file; the moved-from one is closed") {
        const TemporaryDirectory dir;
        const fs::path file = dir.path() / "moved.bin";
        writeFile(file, patternBytes(10000));
        FileReader a = openReader(file, FileReadMode::Buffered);
        const FileReader b(std::move(a));
        CHECK(b.size() == 10000);
        std::vector<std::byte> buffer(64);
        const auto bytes = b.readAt(100, buffer);
        REQUIRE(bytes.has_value());
        CHECK(*bytes == 64);
        CHECK(matchesPattern(buffer, 100));

        // NOLINTNEXTLINE(bugprone-use-after-move): the moved-from state is what is tested
        const auto closed = a.readAt(0, buffer);
        REQUIRE_FALSE(closed.has_value());
        CHECK(closed.error().code == dvv::Errc::Internal);
        CHECK(closed.error().message.find("closed") != std::string::npos);
        CHECK(a.size() == 0);
        CHECK(a.alignment() == 1);
        CHECK_FALSE(a.unbuffered());
        CHECK(a.path().empty());

        // Move assignment closes the target's file and takes the source's.
        FileReader c = openReader(file, FileReadMode::Buffered);
        FileReader d = openReader(file, FileReadMode::Auto);
        const bool d_unbuffered = d.unbuffered();
        c = std::move(d);
        CHECK(c.unbuffered() == d_unbuffered);
        const std::size_t al = c.alignment();
        const auto aligned = alignedBuffer(alignUp(64, al), al);
        const auto again = c.readAt(0, std::span(aligned.get(), alignUp(64, al)));
        REQUIRE(again.has_value());
        CHECK(*again == std::min<std::size_t>(alignUp(64, al), 10000));
        CHECK(matchesPattern(std::span(aligned.get(), 64), 0));
    }

#if defined(__linux__)
    TEST_CASE("/dev/shm reads") {
        const fs::path shm = "/dev/shm";
        std::error_code error;
        if (!fs::is_directory(shm, error)) {
            MESSAGE("/dev/shm is not a directory; nothing to check");
            return;
        }
        std::random_device random;
        const fs::path file = shm / std::format("dvv-file-reader-{:08x}.bin", random());
        constexpr std::size_t SIZE = std::size_t{3} * 4096;
        writeFile(file, patternBytes(SIZE));
        {
            const FileReader reader = openReader(file, FileReadMode::Auto);
            std::vector<std::byte> buffer(SIZE);
            const auto bytes = reader.readAt(0, buffer);
            REQUIRE(bytes.has_value());
            CHECK(*bytes == SIZE);
            CHECK(matchesPattern(buffer, 0));
        }
        fs::remove(file, error);
    }
#endif
}
