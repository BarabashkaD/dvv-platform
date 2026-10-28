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
#include <new>
#include <span>
#include <string>
#include <utility>

#include "dvv/base/error.h"
#include "dvv/base/log.h"
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

// Everything that can throw (the path copy, the read lock) runs before the guard hands the handle
// on, so a failed allocation never leaks it.
FileReader adopt(HandleGuard& guard, const std::filesystem::path& path, std::uint64_t size,
                 std::uint32_t alignment, std::string fallback, NativeReadFn read) {
    std::filesystem::path owned = path;
    auto lock = std::make_unique<std::mutex>();
    return FileReaderAccess::make(guard.release(), std::move(owned), size, alignment,
                                  std::move(fallback), read, std::move(lock));
}

Result<FileReader> openBuffered(const std::filesystem::path& path, const NativeOps& ops,
                                std::string fallback) {
    const auto open_error = [&path](int error) {
        return makeError(Errc::Io, std::format("'{}' could not be opened: {}", displayPath(path),
                                               systemMessage(error)));
    };
    const NativeOpen opened = ops.open(path, false);
    if (opened.handle == NO_HANDLE) {
        return open_error(opened.error);
    }
    HandleGuard guard(opened.handle);
    const NativeSize size = fileSize(guard.get());
    if (size.error != 0) {
        return open_error(size.error);
    }
    return adopt(guard, path, size.size, 1, std::move(fallback), ops.read);
}

// The open-time check of U-IO2: the unbuffered open, the volume's sector size and the first
// aligned read. Unexpected: the reason it failed.
std::expected<FileReader, std::string> openUnbuffered(const std::filesystem::path& path,
                                                      const NativeOps& ops) {
    const NativeOpen opened = ops.open(path, true);
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
    return adopt(guard, path, size.size, alignment, {}, ops.read);
}

}  // namespace

std::string displayPath(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    std::string result;
    result.reserve(text.size());
    for (const char8_t c : text) {
        result.push_back(static_cast<char>(c));
    }
    return result;
}

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
                                               displayPath(path), opened.error()));
    }
    // Auto: a file that cannot be opened at all reports the buffered open's error, without the
    // fallback line.
    DVV_TRY(FileReader reader, openBuffered(path, ops, opened.error()));
    DVV_LOG_INFO("'{}': the unbuffered check failed ({}); reading buffered", displayPath(path),
                 opened.error());
    return reader;
}

}  // namespace detail

Result<FileReader> FileReader::open(const std::filesystem::path& path, FileReadMode mode) {
    return detail::openWith(path, mode, detail::NativeOps{});
}

FileReader::FileReader(std::intptr_t handle, std::filesystem::path path, std::uint64_t size,
                       std::uint32_t alignment, std::string fallback, detail::NativeReadFn read,
                       std::unique_ptr<std::mutex> read_lock) noexcept
    : handle_(handle),
      path_(std::move(path)),
      size_(size),
      alignment_(alignment),
      unbuffered_(alignment > 1),
      fallback_(std::move(fallback)),
      read_(read),
      read_lock_(std::move(read_lock)) {}

FileReader::FileReader(FileReader&& other) noexcept
    : handle_(std::exchange(other.handle_, detail::NO_HANDLE)),
      path_(std::exchange(other.path_, {})),
      size_(std::exchange(other.size_, 0)),
      alignment_(std::exchange(other.alignment_, 1)),
      unbuffered_(std::exchange(other.unbuffered_, false)),
      fallback_(std::exchange(other.fallback_, {})),
      read_(std::exchange(other.read_, nullptr)),
      read_lock_(std::move(other.read_lock_)) {}

FileReader& FileReader::operator=(FileReader&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, detail::NO_HANDLE);
        path_ = std::exchange(other.path_, {});
        size_ = std::exchange(other.size_, 0);
        alignment_ = std::exchange(other.alignment_, 1);
        unbuffered_ = std::exchange(other.unbuffered_, false);
        fallback_ = std::exchange(other.fallback_, {});
        read_ = std::exchange(other.read_, nullptr);
        read_lock_ = std::move(other.read_lock_);
    }
    return *this;
}

FileReader::~FileReader() {
    close();
}

void FileReader::close() noexcept {
    detail::closeNative(std::exchange(handle_, detail::NO_HANDLE));
}

std::string FileReader::modeText() const {
    if (unbuffered_) {
        return "unbuffered";
    }
    if (fallback_.empty()) {
        return "buffered";
    }
    return std::format("buffered (fallback: {})", fallback_);
}

Result<std::size_t> FileReader::readAt(std::uint64_t offset,
                                       std::span<std::byte> destination) const {
    if (handle_ == detail::NO_HANDLE) {
        return makeError(Errc::Internal,
                         std::format("'{}': the reader is closed", detail::displayPath(path_)));
    }
    if (unbuffered_) {
        const auto address = reinterpret_cast<std::uintptr_t>(destination.data());
        const std::uint64_t remainder = address % alignment_;
        if (offset % alignment_ != 0 || destination.size() % alignment_ != 0 || remainder != 0) {
            return makeError(Errc::Internal,
                             std::format("misaligned unbuffered read of '{}': offset {}, {} bytes, "
                                         "address mod {} = {}",
                                         detail::displayPath(path_), offset, destination.size(),
                                         alignment_, remainder));
        }
    }
    if (destination.empty()) {
        return 0;
    }
    detail::NativeRead read;
    {
        const std::scoped_lock lock(*read_lock_);  // U-IO2: one read at a time per file
        read = read_(handle_, offset, destination, size_);
    }
    if (read.error != 0) {
        return makeError(Errc::Io, std::format("reading '{}' at offset {} failed: {}",
                                               detail::displayPath(path_), offset + read.bytes,
                                               detail::systemMessage(read.error)));
    }
    return read.bytes;
}

Result<AlignedBuffer> allocateAligned(std::size_t bytes, std::size_t alignment) {
    if (bytes == 0 || !std::has_single_bit(alignment)) {
        return makeError(Errc::Internal,
                         std::format("allocateAligned: {} bytes at alignment {}: the size must not "
                                     "be 0 and the alignment must be a power of two",
                                     bytes, alignment));
    }
    void* memory = ::operator new[](bytes, std::align_val_t{alignment}, std::nothrow);
    if (memory == nullptr) {
        return makeError(Errc::OutOfMemory,
                         std::format("allocating {} bytes aligned to {} failed", bytes, alignment));
    }
    return AlignedBuffer(static_cast<std::byte*>(memory), AlignedBufferDeleter{alignment});
}

void AlignedBufferDeleter::operator()(std::byte* memory) const noexcept {
    ::operator delete[](memory, std::align_val_t{alignment});
}

}  // namespace dvv::platform
