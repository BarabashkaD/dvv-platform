# dvv-platform

Platform layer of the dvv stack used by rawreel, on Windows, Linux and macOS: native window handles
(`WindowHandle`), run-time libraries (`DynamicLibrary`), positional file reads (`FileReader`),
atomic file replacement (`replaceFile`), a crash handler (`installCrashHandler`) and a native test
window for display tests (`dvv::platform_testing`). C++23; depends on
[dvv-base](https://github.com/BarabashkaD/dvv-base) and the pinned Vulkan-Headers (C headers only,
`VK_NO_PROTOTYPES`; used by the deprecated surface.h). Nothing in it depends on a GUI toolkit.

## Build

Builds and tests on Windows (Visual Studio 2026), Linux (Clang 19 with libstdc++ 14) and macOS
(Apple Silicon) with [dvv-build](https://github.com/BarabashkaD/dvv-build), which provides the
presets, the shared CMake modules, the third-party dependencies and the CI workflow; its README
lists the prerequisites. dvv-build is cloned next to dvv-platform at the version of the `dvv-build`
line in `dvv-deps.txt`; `fetch-deps.cmake` clones dvv-base next to them at its minimum version from
the same file:

```powershell
git clone https://github.com/BarabashkaD/dvv-platform
git clone --branch v0.5.1 https://github.com/BarabashkaD/dvv-build
python -m pip install -r dvv-build/tools/requirements.txt
cmake -DREPO=dvv-platform -P dvv-build/tools/fetch-deps.cmake
./dvv-build/bootstrap.ps1
cd dvv-platform
cmake --workflow --preset vs2026
```

On Linux: `./dvv-build/bootstrap.sh`, then `cmake --workflow --preset ninja-clang`. On macOS:
`./dvv-build/bootstrap.sh`, then `cmake --workflow --preset ninja-macos`.
Applications use dvv-platform as a git submodule in their `libs/` folder, next to dvv-build.

## API

Target `dvv::platform` (namespace `dvv::platform`):

- `dvv/platform/pixel_size.h`: `PixelSize` in physical pixels.
- `dvv/platform/window_handle.h`: `WindowHandle`, a trivially copyable window handle (a window
  system and two native words, empty by default); `WindowSystem`, `windowSystemName()` and
  `defaultWindowSystem()`. It includes no OS header. The per-OS headers
  `window_handle_win32.h` (`Win32Window`: `HINSTANCE`, `HWND`), `window_handle_xlib.h`
  (`XlibWindow`) and `window_handle_metal.h` (`MetalWindow`: a `CAMetalLayer*`) build a
  `WindowHandle` with `windowHandleOf(<struct>)` and read it back with `win32WindowOf()`,
  `xlibWindowOf()` and `metalWindowOf()`. The caller keeps the window alive while anything made
  from its handle exists.
- `dvv/platform/native_window.h` (deprecated, removed in 1.0.0): `NativeWindow`, a `std::variant`
  of `std::monostate` and the per-OS structs; `windowSystemOf()` and
  `windowHandleOf(const NativeWindow&)`. Use `window_handle.h`, `pixel_size.h` and the per-OS
  header.
- `dvv/platform/surface.h` (deprecated, removed in 1.0.0; dvv-gpu creates its surfaces itself):
  `surfaceInstanceExtensions(system)` lists the instance extensions a window system needs.
  `createSurface(instance, vkGetInstanceProcAddr, window)` returns
  `std::expected<VkSurfaceKHR, VkResult>`: `VK_ERROR_EXTENSION_NOT_PRESENT` when the function is
  unavailable, `VK_ERROR_INITIALIZATION_FAILED` for `std::monostate` or null handles, else the
  driver's result. Destroy the surface before the window.
- `dvv/platform/dynamic_library.h`: `DynamicLibrary`, a move-only RAII handle.
  `DynamicLibrary::open(name, search)` takes a UTF-8 name and returns `Errc::Unsupported`
  "'<name>' could not be loaded: <system message>", or `Errc::Internal` for an empty name, a name
  containing `'\0'`, a `SystemDirectory` name with a path separator, or a `LibraryDirectory`
  name that is not an absolute path. `symbol()` / `function<F>()` return null for a missing
  symbol; the destructor unloads the library unless `keepLoaded()` was called.
  `libraryFileName(name, major)` gives the OS's file name of a versioned library: "avutil-60.dll",
  "libavutil.60.dylib", "libavutil.so.60".
- `dvv/platform/aligned_buffer.h`: `AlignedBuffer` (`std::unique_ptr<std::byte[],
  AlignedBufferDeleter>`) and `allocateAligned(bytes, alignment)`, memory from `operator new[]`
  with `std::align_val_t`.
- `dvv/platform/file_reader.h`: `FileReader` (below); it includes aligned_buffer.h.
- `dvv/platform/file_writer.h`: `replaceFile(target, bytes, options)` writes the bytes to a
  temporary file in the target's folder and replaces the target with it. `ReplaceFileOptions` sets
  `durability` (`FileDurability`: flush before the replace or not), `retries` and `retryDelay`
  (Windows) and `lostTarget` (`LostTarget`: what happens to the temporary file when Windows loses
  the target). Errors are `Errc::Io`, or `Errc::Internal` for a path without a file name.
- `dvv/platform/crash_handler.h`: `installCrashHandler(options)` (below) and the test hook
  `crashIfRequested(variable)`.

### Library search

- `LibrarySearch::Default`: `LoadLibraryExW(name, nullptr, 0)`.
- `LibrarySearch::SystemDirectory`: only the Windows system directory
  (`LOAD_LIBRARY_SEARCH_SYSTEM32`), never the application directory, the current directory or
  `PATH`; the name must be a bare file name. dvv-gpu opens `vulkan-1.dll` this way.
- `LibrarySearch::LibraryDirectory`: loads a library by absolute path and takes its dependencies
  only from its own folder, the executable's folder and System32, as the FFmpeg loader of
  dvv-components does. `LibrarySearch::DllDirectory` stays as an alias of it (deprecated, removed
  in 1.0.0).

### replaceFile

`replaceFile` creates the temporary file `<target file name>.<16 hex digits>.tmp` exclusively. On
macOS and Linux the temporary file gets the target's permission bits (a new target: 0666 less the
umask); filesystems without POSIX modes work, because the mode is set only when it is needed.
Windows replaces an existing target with `ReplaceFileW` and moves the file to a new target with
`MoveFileExW`; macOS and Linux rename. With `retries` set, Windows tries the replace again after a
sharing violation, a lock violation, access denied, `ERROR_UNABLE_TO_REMOVE_REPLACED`,
`ERROR_ALREADY_EXISTS` or `ERROR_FILE_EXISTS` for a new target another writer created first, and
`ERROR_FILE_NOT_FOUND` for an existing target that another writer's replace has briefly moved
away. It creates no folder and resolves no link. The temporary file is removed on every failure
except a lost target with `LostTarget::KeepTemporary`.

### Crash handler

`dvv/platform/crash_handler.h`: an app calls `installCrashHandler({.app = "<app>", .process =
"<executable>"})` once, after it adds its log sinks. Crash files go to
the `crashes` folder in `paths::userDataDir(app)`; the folder and any missing folder above it
are created at the first crash. `app` is 1 to 63 bytes of UTF-8.

| OS | On a crash | Then |
| --- | --- | --- |
| Windows | the log snapshot `<process>-<yyyymmdd-hhmmss>-<pid>.dvvlog`, then a minidump `<same stem>.dmp` | the process ends with the exception code |
| macOS | the log snapshot `<process>-<yyyymmdd-hhmmss>-<pid>.dvvlog` | the system writes its crash report as for any process |
| Linux | the log snapshot `<process>-<yyyymmdd-hhmmss>-<pid>.dvvlog` | a core dump, where the system enables them |

stderr gets "<app>: crash exception 0x<code>" (Windows) or "<app>: crash
signal <number>" (macOS, Linux), then "<app>: crash log <path>", then "<app>: crash dump <path>"
(Windows). The snapshot holds
the log records that had not reached the sinks yet (asynchronous logging) and the dropped counts.
The time in the names is local time at the crash; on macOS and Linux it uses the offset at
install, so after a daylight saving change it is off by that change. At the next start of the same
`process`, `installCrashHandler` logs each earlier snapshot at Info and renames it to
`<stem>.reported.dvvlog`. On macOS and Linux the handler takes SIGSEGV, SIGBUS, SIGILL, SIGFPE,
SIGTRAP and SIGABRT on an alternate stack of the installing thread; a stack overflow on another
thread ends the process without a snapshot. A handler for these signals installed before it runs
after it; one installed later replaces it.

On Windows without `dbghelp.dll` or its `MiniDumpWriteDump` (or when the dump thread cannot
start), the handler still installs: a crash writes the stderr lines and the snapshot, no `.dmp`,
and the install logs one Warn "crash dumps are off: <reason>". When two threads crash at once, the
first one's files are written; on macOS and Linux a second crashing thread waits for them, then
its signal is raised again.

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
"no display ..." without a display. It offers `handle()` (a `WindowHandle`), `clientSize()`,
`resize()`, `minimise()`, `restore()` and `pump()`; `native()` is deprecated (use `handle()`). It is
move-only and used on the thread that created it; destroy every surface and presenter made from it
first.

## Tests

- `dvv_platform.unit`: `DynamicLibrary`, `WindowHandle`, `NativeWindow`, `PixelSize`,
  `AlignedBuffer`, `replaceFile`, the crash handler's internals, the extension lists and
  `FileReader`; no display needed.
- `dvv_platform.surface` (label `display`): creates a `TestWindow` and a Vulkan surface for it. It
  exits with 77 (skipped) without a Vulkan loader, surface extensions or display;
  `DVV_REQUIRE_GPU=1` turns that into a failure.
- `dvv_platform.crash`: a child process crashes in each mode and a script checks the stderr lines,
  the files and the report of the next start. Not registered under sanitizers.
- `dvv_platform.package`: installs the package and builds consumers against it.

Licence: MIT.
