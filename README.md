# dvv-platform

Platform layer of the dvv stack used by rawreel: native window descriptions (`NativeWindow`), Vulkan
surface creation, run-time libraries (`DynamicLibrary`), positional file reads (`FileReader`) and a
native test window for display tests (`dvv::platform_testing`). C++23; depends on
[dvv-base](https://github.com/BarabashkaD/dvv-base) and the pinned Vulkan-Headers (C headers only,
`VK_NO_PROTOTYPES`). Nothing in it depends on a GUI toolkit.

## Build

Builds and tests on Windows (Visual Studio 2026) and Linux (Clang 19 with libstdc++ 14) with
[dvv-build](https://github.com/BarabashkaD/dvv-build), which provides the presets, the shared CMake
modules, the third-party dependencies and the CI workflow; its README lists the prerequisites.
dvv-build is cloned next to dvv-platform at the version of the `dvv-build` line in `dvv-deps.txt`;
`fetch-deps.cmake` clones dvv-base next to them at its minimum version from the same file:

```powershell
git clone https://github.com/BarabashkaD/dvv-platform
git clone --branch v0.1.1 https://github.com/BarabashkaD/dvv-build
python -m pip install -r dvv-build/tools/requirements.txt
cmake -DREPO=dvv-platform -P dvv-build/tools/fetch-deps.cmake
./dvv-build/bootstrap.ps1
cd dvv-platform
cmake --workflow --preset vs2026
```

On Linux: `./dvv-build/bootstrap.sh`, then `cmake --workflow --preset ninja-clang`.
Applications use dvv-platform as a git submodule in their `libs/` folder, next to dvv-build.

## API

Target `dvv::platform` (namespace `dvv::platform`):

- `dvv/platform/native_window.h`: `WindowSystem`, `windowSystemName()` and `defaultWindowSystem()`;
  `PixelSize` in physical pixels; the handle structs `Win32Window` (`HINSTANCE`, `HWND`) and
  `XlibWindow`; `NativeWindow`, a `std::variant` of `std::monostate` and those structs;
  `windowSystemOf()`. The caller keeps the window alive while any surface made from it exists.
- `dvv/platform/surface.h`: `surfaceInstanceExtensions(system)` lists the instance extensions a
  window system needs. `createSurface(instance, vkGetInstanceProcAddr, window)` returns
  `std::expected<VkSurfaceKHR, VkResult>`: `VK_ERROR_EXTENSION_NOT_PRESENT` when the function is
  unavailable, `VK_ERROR_INITIALIZATION_FAILED` for `std::monostate` or null handles, else the
  driver's result. Destroy the surface before the window.
- `dvv/platform/dynamic_library.h`: `DynamicLibrary`, a move-only RAII handle.
  `DynamicLibrary::open(name, search)` takes a UTF-8 name and returns `Errc::Unsupported`
  "'<name>' could not be loaded: <system message>", or `Errc::Internal` for an empty name, a name
  containing `'\0'`, a `SystemDirectory` name with a path separator, or a `DllDirectory` name that
  is not an absolute path. `symbol()` / `function<F>()` return null for a missing symbol; the
  destructor unloads the library unless `keepLoaded()` was called.
- `dvv/platform/file_reader.h`: `FileReader` (below) and `allocateAligned(bytes, alignment)`, which
  returns an `AlignedBuffer` (`std::unique_ptr<std::byte[]>` from `operator new[]` with
  `std::align_val_t`).

### Library search

- `LibrarySearch::Default`: `LoadLibraryExW(name, nullptr, 0)`.
- `LibrarySearch::SystemDirectory`: only the Windows system directory
  (`LOAD_LIBRARY_SEARCH_SYSTEM32`), never the application directory, the current directory or
  `PATH`; the name must be a bare file name. dvv-gpu opens `vulkan-1.dll` this way.
- `LibrarySearch::DllDirectory`: loads a library by absolute path and takes its dependencies only
  from its own folder, the executable's folder and System32, as the FFmpeg loader of
  dvv-components does.

### FileReader

`FileReader::open(path, mode)` opens a file for `readAt(offset, destination)`, which has no shared
file position. A read blocks: run `readAt` on an I/O pool (`co_await resumeOn(io)`), never on a
render or UI thread. `FileReadMode`:

| Mode | Meaning |
| --- | --- |
| `Buffered` (the default argument) | Through the OS file cache with a sequential-access hint; any offset, length and address work. |
| `Unbuffered` | No OS cache: offset, length and destination address must be multiples of `alignment()` (max(4096, the volume's sector size)); read into `allocateAligned` memory. If the open-time check fails, `open` fails with `Errc::Io` "'<path>' cannot be read unbuffered: <reason>". |
| `Auto` | `Unbuffered` when the open-time check passes, else `Buffered` for this file with one info line "'<path>': the unbuffered check failed (<reason>); reading buffered". The raw and Y4M sources use `Auto` by default. |

The open-time check covers the unbuffered open, the volume's sector size (known, a power of two, at
most 64 KiB) and the first aligned read. `unbuffered()`, `alignment()` and `fallbackReason()` report
the mode in use; `modeText()` gives "unbuffered", "buffered" or "buffered (fallback: <reason>)".

Open flags: buffered `CreateFileW(GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING,
FILE_FLAG_SEQUENTIAL_SCAN)`; unbuffered the same with `FILE_FLAG_NO_BUFFERING` instead of the hint.
Reads are `ReadFile` on a synchronous handle with an `OVERLAPPED` that carries only the offset.

- **One read at a time.** Concurrent `readAt` calls on one reader wait for each other.
- **Sizes and errors.** The size is read once at open; reads return fewer bytes only at the end of
  the file (0 at or past it). Errors are `Errc::Io` with the UTF-8 path and the system message
  ("'<path>' could not be opened: ...", "reading '<path>' at offset <n> failed: ..."). A misaligned
  unbuffered read and a moved-from reader are `Errc::Internal`.
- **Thread safety.** `readAt`, `size()` and `path()` are `const` and may be called from several
  threads at once. Moving or destroying a reader must not overlap a read.
- **`allocateAligned`** needs a power-of-two alignment (`Errc::Internal` for 0 bytes or another
  alignment, `Errc::OutOfMemory` when the allocation fails).

### `TestWindow` (target `dvv::platform_testing`)

`dvv/platform/testing/test_window.h` (namespace `dvv::platform::testing`) is for tests only.
`TestWindow::create(size, title)` opens a visible top-level window; `Errc::Unsupported`
"no display ..." without a display. It offers `native()`, `clientSize()`, `resize()`, `minimise()`,
`restore()` and `pump()`. It is move-only and used on the thread that created it; destroy every
surface and presenter made from it first.

## Tests

- `dvv_platform.unit`: `DynamicLibrary`, `NativeWindow`, `PixelSize`, the extension lists and
  `FileReader`; no display needed.
- `dvv_platform.surface` (label `display`): creates a `TestWindow` and a Vulkan surface for it. It
  exits with 77 (skipped) without a Vulkan loader, surface extensions or display;
  `DVV_REQUIRE_GPU=1` turns that into a failure.
- `dvv_platform.package`: installs the package and builds consumers against it.

Licence: MIT.
