#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>

#if !defined(_WIN32)
#include <pthread.h>
#include <signal.h>
#endif

#include "crash_handler_detail.h"
#include "dvv/base/log.h"
#if !defined(_WIN32)
#include "crash_handler_posix.h"
#endif
#include "test_support.h"

namespace {

namespace fs = std::filesystem;
using dvv::platform::detail::appendNumber;
using dvv::platform::detail::formatCrashStem;
using dvv::platform::detail::isSnapshotName;
using dvv::platform::detail::isValidAppName;
using dvv::platform::detail::isValidProcessName;
using dvv::platform::detail::reportEarlierSnapshots;

constexpr std::string_view HEADER = "an earlier run crashed; its last log records follow (";
constexpr std::string_view TOO_LARGE = "an earlier run left a crash log too large to read (";
constexpr std::string_view FROM_SNAPSHOT = "(from crash snapshot)";

void writeBytes(const fs::path& path, std::string_view bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.is_open());
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(stream.good());
}

void appendU32(std::string& out, std::uint32_t value) {
    std::array<char, sizeof value> bytes{};
    std::memcpy(bytes.data(), &value, sizeof value);
    out.append(bytes.data(), bytes.size());
}

// A complete snapshot without records or sites: the magic, five zero dropped counts, the end of
// the records and a site count of 0, in host byte order.
std::string emptySnapshot() {
    std::string bytes = "DVVLOGS1";
    bytes.append(40, '\0');
    appendU32(bytes, 0xFFFFFFFF);
    appendU32(bytes, 0);
    return bytes;
}

// "p-20261120-1015<ss>-1" for the second `second`.
std::string stemAt(int second) {
    return std::format("p-20261120-1015{:02}-1", second);
}

}  // namespace

TEST_SUITE("crash_handler") {
    TEST_CASE("app and process names") {
        CHECK_FALSE(isValidAppName(""));
        CHECK_FALSE(isValidAppName(std::string(64, 'a')));
        CHECK_FALSE(isValidAppName("\xC3"));
        CHECK(isValidAppName("rawreel"));
        CHECK(isValidAppName(std::string(63, 'a')));
        CHECK_FALSE(isValidProcessName(""));
        CHECK_FALSE(isValidProcessName(std::string(64, 'a')));
        CHECK_FALSE(isValidProcessName("a/b"));
        CHECK_FALSE(isValidProcessName("\xC3\xA9"));
        CHECK(isValidProcessName("rawreel-gui"));
        CHECK(isValidProcessName("rawreel.cli_2"));
    }

    TEST_CASE("appendNumber") {
        const auto text = [](std::uint64_t value, unsigned base, unsigned digits) {
            std::array<char, 32> out{};
            const std::size_t count = appendNumber(out, value, base, digits);
            return std::string(out.data(), count);
        };
        constexpr std::uint64_t MAX = std::numeric_limits<std::uint64_t>::max();
        CHECK(text(0, 10, 1) == "0");
        CHECK(text(MAX, 10, 1) == "18446744073709551615");
        CHECK(text(0xE06D7363, 16, 8) == "e06d7363");
        CHECK(text(7, 10, 3) == "007");
        std::array<char, 19> short19{};
        CHECK(appendNumber(short19, MAX, 10, 1) == 0);
        std::array<char, 7> short7{};
        CHECK(appendNumber(short7, 0xE06D7363, 16, 8) == 0);
    }

    TEST_CASE("isSnapshotName") {
        CHECK(isSnapshotName("rawreel-gui-20261120-101500-42.dvvlog", "rawreel-gui"));
        CHECK_FALSE(isSnapshotName("rawreel-gui-20261120-101500-42.dvvlog", "rawreel"));
        CHECK_FALSE(isSnapshotName("rawreel-20261120-101500-42.dvvlog", "rawreel-gui"));
        CHECK_FALSE(
            isSnapshotName("rawreel-gui-20261120-101500-42.reported.dvvlog", "rawreel-gui"));
        CHECK_FALSE(isSnapshotName("rawreel-gui-2026112-101500-42.dvvlog", "rawreel-gui"));
        CHECK_FALSE(isSnapshotName("rawreel-gui-20261120-101500-.dvvlog", "rawreel-gui"));
        CHECK_FALSE(
            isSnapshotName("rawreel-gui-20261120-101500-12345678901.dvvlog", "rawreel-gui"));
        CHECK(isSnapshotName("rawreel-gui-20261120-101500-1234567890.dvvlog", "rawreel-gui"));
    }

    TEST_CASE("formatCrashStem") {
        const auto stem = [](std::int64_t seconds, std::uint64_t pid) {
            std::array<char, 64> out{};
            const std::size_t count = formatCrashStem(out, "p", seconds, pid);
            return std::string(out.data(), count);
        };
        CHECK(stem(0, 1) == "p-19700101-000000-1");
        CHECK(stem(1709251199, 42) == "p-20240229-235959-42");
        CHECK(stem(4102444799, 7) == "p-20991231-235959-7");
        CHECK(stem(-3600, 7) == "p-19691231-230000-7");
        std::array<char, 19> exact{};  // "p-19700101-000000-1" is 19 bytes
        CHECK(formatCrashStem(exact, "p", 0, 1) == 19);
        CHECK(std::string_view(exact.data(), exact.size()) == "p-19700101-000000-1");
        std::array<char, 18> shorter{};
        CHECK(formatCrashStem(shorter, "p", 0, 1) == 0);
    }

    TEST_CASE("the report takes the 8 oldest snapshots and renames them") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-crash-handler");
        const dvv::platform::test::LoggerScope log;
        for (int second = 0; second < 10; ++second) {
            writeBytes(dir.path() / (stemAt(second) + ".dvvlog"), emptySnapshot());
        }
        reportEarlierSnapshots(dir.path(), "p", 8);
        CHECK(log.count(HEADER) == 8);
        for (int second = 0; second < 8; ++second) {
            CHECK_FALSE(fs::exists(dir.path() / (stemAt(second) + ".dvvlog")));
            CHECK(fs::exists(dir.path() / (stemAt(second) + ".reported.dvvlog")));
        }
        for (int second = 8; second < 10; ++second) {
            CHECK(fs::exists(dir.path() / (stemAt(second) + ".dvvlog")));
            CHECK_FALSE(fs::exists(dir.path() / (stemAt(second) + ".reported.dvvlog")));
        }
        reportEarlierSnapshots(dir.path(), "p", 8);
        CHECK(log.count(HEADER) == 10);
        CHECK(fs::exists(dir.path() / (stemAt(9) + ".reported.dvvlog")));
        reportEarlierSnapshots(dir.path(), "p", 8);
        CHECK(log.count(HEADER) == 10);
    }

    TEST_CASE("a snapshot over the capacity is renamed, not read") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-crash-handler");
        const dvv::platform::test::LoggerScope log;
        writeBytes(dir.path() / (stemAt(0) + ".dvvlog"),
                   std::string(dvv::Logger::crashSnapshotCapacity() + 1, '\0'));
        reportEarlierSnapshots(dir.path(), "p", 8);
        CHECK(log.count(TOO_LARGE) == 1);
        CHECK(log.count(HEADER) == 0);
        CHECK_FALSE(fs::exists(dir.path() / (stemAt(0) + ".dvvlog")));
        CHECK(fs::exists(dir.path() / (stemAt(0) + ".reported.dvvlog")));
    }

    TEST_CASE("a cut-short and an empty snapshot give the header line only") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-crash-handler");
        const dvv::platform::test::LoggerScope log;
        writeBytes(dir.path() / (stemAt(0) + ".dvvlog"), "DVVLOGS1");
        writeBytes(dir.path() / (stemAt(1) + ".dvvlog"), "");
        reportEarlierSnapshots(dir.path(), "p", 8);
        CHECK(log.count(HEADER) == 2);
        CHECK(log.count(FROM_SNAPSHOT) == 0);
        for (int second = 0; second < 2; ++second) {
            CHECK_FALSE(fs::exists(dir.path() / (stemAt(second) + ".dvvlog")));
            CHECK(fs::exists(dir.path() / (stemAt(second) + ".reported.dvvlog")));
        }
    }

    TEST_CASE("the report skips what is not this process's snapshot") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-crash-handler");
        const dvv::platform::test::LoggerScope log;
        const fs::path folder = dir.path() / "p-20261120-101500-1.dvvlog";
        REQUIRE(fs::create_directory(folder));
        const fs::path other = dir.path() / "q-20261120-101500-1.dvvlog";
        writeBytes(other, emptySnapshot());
        reportEarlierSnapshots(dir.path(), "p", 8);
        CHECK(log.count(HEADER) == 0);
        CHECK(log.count(TOO_LARGE) == 0);
        CHECK(fs::is_directory(folder));
        CHECK(fs::exists(other));
    }

    TEST_CASE("a missing crashes folder is left alone") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-crash-handler");
        const dvv::platform::test::LoggerScope log;
        reportEarlierSnapshots(dir.path() / "crashes", "p", 8);
        CHECK(log.count("an earlier run") == 0);
        CHECK_FALSE(fs::exists(dir.path() / "crashes"));
    }
}

#if !defined(_WIN32)
namespace {
siginfo_t captured{};
void captureSignal(int /*signal*/, siginfo_t* info, void* /*context*/) {
    captured = *info;
}
}  // namespace

TEST_SUITE("crash_handler") {
    using dvv::platform::detail::faultRepeats;
    using dvv::platform::detail::signalWasSent;

    TEST_CASE("signalWasSent") {
        siginfo_t info{};
        info.si_code = 0;
        CHECK(signalWasSent(SIGABRT, info));
        info.si_code = SEGV_MAPERR;
        CHECK_FALSE(signalWasSent(SIGSEGV, info));
        info.si_code = SI_USER;
        CHECK(signalWasSent(SIGSEGV, info));
    }

    TEST_CASE("faultRepeats") {
        siginfo_t info{};
        info.si_code = SEGV_MAPERR;
        CHECK(faultRepeats(SIGSEGV, info));
        info.si_code = BUS_ADRALN;
        CHECK(faultRepeats(SIGBUS, info));
        info.si_code = TRAP_BRKPT;
        CHECK_FALSE(faultRepeats(SIGTRAP, info));
        info.si_code = 0;
        CHECK_FALSE(faultRepeats(SIGABRT, info));
        info.si_code = SI_USER;
        CHECK_FALSE(faultRepeats(SIGSEGV, info));
#if defined(__linux__)
        info.si_code = SI_KERNEL;
        CHECK_FALSE(faultRepeats(SIGTRAP, info));
#endif
    }

    TEST_CASE("pthread_kill counts as sent") {
        struct sigaction action{};
        action.sa_sigaction = &captureSignal;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        struct sigaction previous{};
        captured = siginfo_t{};
        REQUIRE(::sigaction(SIGUSR1, &action, &previous) == 0);
        REQUIRE(::pthread_kill(::pthread_self(), SIGUSR1) == 0);  // delivered before it returns
        ::sigaction(SIGUSR1, &previous, nullptr);
        CHECK(captured.si_signo == SIGUSR1);  // the handler ran
        CHECK(signalWasSent(SIGSEGV, captured));
    }
}
#endif
