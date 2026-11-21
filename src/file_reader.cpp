#include "dvv/platform/file_reader.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>

#include "dvv/base/error.h"
#include "dvv/base/log.h"
#include "dvv/base/utf8.h"
#include "file_reader_detail.h"

namespace dvv::platform {

namespace detail {

namespace {

// The smallest unbuffered alignment (a page), and the largest sector size taken as usual.
constexpr std::uint32_t MIN_UNBUFFERED_ALIGNMENT = 4096;
constexpr std::uint32_t MAX_SECTOR_SIZE = 65536;

// Closes a native handle on every early return unless release() hands it on.
class HandleGuard {
public:
    explicit HandleGuard(std::intptr_t handle) noexcept : handle_(handle) {}
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    HandleGuard(HandleGuard&&) = delete;
    HandleGuard& operator=(HandleGuard&&) = delete;
    ~HandleGuard() { closeNative(handle_); }

    [[nodiscard]] std::intptr_t get() const noexcept { return handle_; }
    [[nodiscard]] std::intptr_t release() noexcept { return std::exchange(handle_, NO_HANDLE); }

private:
    std::intptr_t handle_;
};

// Everything that can throw (the caller's path copy, the read lock) runs before the guard hands
// the handle on, so a failed allocation never leaks it.
FileReader adopt(HandleGuard& guard, ReaderParts parts) {
    parts.readLock = std::make_unique<std::mutex>();
    parts.handle = guard.release();
    return FileReaderAccess::make(std::move(parts));
}

Result<FileReader> openBuffered(const std::filesystem::path& path, const NativeOps& ops,
                                std::string fallback) {
    const auto open_error = [&path](int error) {
        return makeError(Errc::Io, std::format("'{}' could not be opened: {}",
                                               dvv::pathDisplayText(path), systemMessage(error)));
    };
    const NativeOpen opened = ops.open(path, OpenMode::Buffered);
    if (opened.handle == NO_HANDLE) {
        return open_error(opened.error);
    }
    HandleGuard guard(opened.handle);
    const NativeSize size = fileSize(guard.get());
    if (size.error != 0) {
        return open_error(size.error);
    }
    return adopt(guard, {.path = path,
                         .size = size.size,
                         .alignment = 1,
                         .fallback = std::move(fallback),
                         .read = ops.read});
}

// The open-time check of U-IO2: the unbuffered open, the volume's sector size and the first
// aligned read. Unexpected: the reason it failed.
std::expected<FileReader, std::string> openUnbuffered(const std::filesystem::path& path,
                                                      const NativeOps& ops) {
    const NativeOpen opened = ops.open(path, OpenMode::Unbuffered);
    if (opened.handle == NO_HANDLE) {
        return std::unexpected("the unbuffered open failed: " + systemMessage(opened.error));
    }
    HandleGuard guard(opened.handle);
    const NativeSize size = fileSize(guard.get());
    if (size.error != 0) {
        return std::unexpected("the file size is unknown: " + systemMessage(size.error));
    }
    const std::uint32_t sector = ops.sectorSize(guard.get());
    if (sector == 0) {
        return std::unexpected(std::string("the volume's sector size is unknown"));
    }
    if (!std::has_single_bit(sector) || sector > MAX_SECTOR_SIZE) {
        return std::unexpected(std::format("unusual sector size {}", sector));
    }
    const std::uint32_t alignment = std::max(MIN_UNBUFFERED_ALIGNMENT, sector);
    if (size.size > 0) {
        Result<AlignedBuffer> probe = allocateAligned(alignment, alignment);
        if (!probe) {
            return std::unexpected(probe.error().message);
        }
        const std::uint64_t expected = std::min<std::uint64_t>(size.size, alignment);
        const NativeRead read =
            ops.read(guard.get(), 0, std::span(probe->get(), alignment), size.size);
        if (read.error != 0) {
            return std::unexpected("the first aligned read failed: " + systemMessage(read.error));
        }
        if (read.bytes != expected) {
            return std::unexpected(std::format("the first aligned read returned {} of {} bytes",
                                               read.bytes, expected));
        }
    }
    return adopt(guard,
                 {.path = path, .size = size.size, .alignment = alignment, .read = ops.read});
}

}  // namespace

Result<FileReader> openWith(const std::filesystem::path& path, FileReadMode mode,
                            const NativeOps& ops) {
    const bool unbuffered =
        mode == FileReadMode::Unbuffered || (mode == FileReadMode::Auto && ops.autoTriesUnbuffered);
    if (!unbuffered) {
        return openBuffered(path, ops, {});
    }
    std::expected<FileReader, std::string> opened = openUnbuffered(path, ops);
    if (opened) {
        return std::move(*opened);
    }
    if (mode == FileReadMode::Unbuffered) {
        return makeError(Errc::Io, std::format("'{}' cannot be read unbuffered: {}",
                                               dvv::pathDisplayText(path), opened.error()));
    }
    // Auto: a file that cannot be opened at all reports the buffered open's error, without the
    // fallback line.
    DVV_TRY(FileReader reader, openBuffered(path, ops, opened.error()));
    DVV_LOG_INFO("'{}': the unbuffered check failed ({}); reading buffered",
                 dvv::pathDisplayText(path), opened.error());
    return reader;
}

}  // namespace detail

Result<FileReader> FileReader::open(const std::filesystem::path& path, FileReadMode mode) {
    return detail::openWith(path, mode, detail::NativeOps{});
}

FileReader::FileReader(detail::ReaderParts parts) noexcept : parts_(std::move(parts)) {}

FileReader::FileReader(FileReader&& other) noexcept : parts_(std::exchange(other.parts_, {})) {}

FileReader& FileReader::operator=(FileReader&& other) noexcept {
    if (this != &other) {
        close();
        parts_ = std::exchange(other.parts_, {});
    }
    return *this;
}

FileReader::~FileReader() {
    close();
}

void FileReader::close() noexcept {
    detail::closeNative(std::exchange(parts_.handle, detail::NO_HANDLE));
}

std::string FileReader::modeText() const {
    if (unbuffered()) {
        return "unbuffered";
    }
    if (parts_.fallback.empty()) {
        return "buffered";
    }
    return std::format("buffered (fallback: {})", parts_.fallback);
}

Result<std::size_t> FileReader::readAt(std::uint64_t offset,
                                       std::span<std::byte> destination) const {
    if (parts_.handle == detail::NO_HANDLE) {
        return makeError(Errc::Internal, std::format("'{}': the reader is closed",
                                                     dvv::pathDisplayText(parts_.path)));
    }
    if (unbuffered()) {
        const std::uint32_t block_alignment = parts_.alignment;
        const auto address = reinterpret_cast<std::uintptr_t>(destination.data());
        const std::uint64_t remainder = address % block_alignment;
        if (offset % block_alignment != 0 || destination.size() % block_alignment != 0 ||
            remainder != 0) {
            return makeError(Errc::Internal,
                             std::format("misaligned unbuffered read of '{}': offset {}, {} bytes, "
                                         "address mod {} = {}",
                                         dvv::pathDisplayText(parts_.path), offset,
                                         destination.size(), block_alignment, remainder));
        }
    }
    if (destination.empty()) {
        return 0;
    }
    detail::NativeRead read;
    {
        const std::scoped_lock lock(*parts_.readLock);  // U-IO2: one read at a time per file
        read = parts_.read(parts_.handle, offset, destination, parts_.size);
    }
    if (read.error != 0) {
        return makeError(
            Errc::Io,
            std::format("reading '{}' at offset {} failed: {}", dvv::pathDisplayText(parts_.path),
                        offset + read.bytes, detail::systemMessage(read.error)));
    }
    return read.bytes;
}

}  // namespace dvv::platform
