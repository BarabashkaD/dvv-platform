# Changelog

All notable changes to dvv-platform. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow semantic versioning with
the dvv rules: compatible = the same major and at least the minimum version, also for 0.x; breaking
changes only in a new major, after a deprecation. This file is the release notes (there are no
GitHub Releases).

## [0.3.0] - 2026-11-21

### Added
- `dvv/platform/file_writer.h`: `replaceFile(target, bytes, options)` writes a file through a
  temporary file in the target's folder and replaces the target. The temporary file gets the
  target's permission bits; filesystems without POSIX modes work. `ReplaceFileOptions` sets the
  flush (`FileDurability`), retries on Windows (sharing and lock violations, access denied, 1175,
  an existing name for a new target, and `ERROR_FILE_NOT_FOUND` for an existing target another
  writer's replace has briefly moved away) and what happens to the temporary file when Windows
  loses the target (`LostTarget`). Its errors name the target: "replacing '<target>' failed:
  <reason>".
- `libraryFileName(name, major)` in `dynamic_library.h`: "avutil-60.dll", "libavutil.60.dylib",
  "libavutil.so.60".
- `LibrarySearch::LibraryDirectory`: the mode `DllDirectory` named for what it does on every OS.
- `dvv/platform/aligned_buffer.h` with `AlignedBuffer`, `AlignedBufferDeleter` and
  `allocateAligned`; `file_reader.h` includes it.
- `dvv/platform/pixel_size.h` with `PixelSize`.
- `dvv/platform/window_handle.h`: `WindowHandle`, a trivially copyable window handle (a window
  system and two native words), with `WindowSystem`, `windowSystemName()` and
  `defaultWindowSystem()`. Per-OS headers `window_handle_win32.h`, `window_handle_xlib.h` and
  `window_handle_metal.h` hold `Win32Window`, `XlibWindow` and `MetalWindow` with
  `windowHandleOf()` for each struct, and `win32WindowOf()`, `xlibWindowOf()` and
  `metalWindowOf()` to read a handle back. `windowHandleOf(const NativeWindow&)` converts the
  variant. `TestWindow::handle()`.
- `dvv/platform/crash_handler.h`: `installCrashHandler(options)` and the test hook
  `crashIfRequested(variable)`. Windows: a crash or `abort()` writes the logger's crash snapshot
  (`.dvvlog`), then a minidump; without dbghelp the snapshot is still written and the install
  logs "crash dumps are off: <reason>". macOS and Linux: a fatal signal writes the crash
  snapshot, then the system's crash report or core dump follows; a second crashing thread waits
  for the first one's files, then its signal takes its course under the restored action (a
  hardware fault faults again; SIGTRAP, SIGABRT and a sent signal are raised again). stderr names
  the cause and the files. A snapshot is named `.dvvlog` only once written. A hung crash ends
  the process after a time limit (macOS and Linux: `alarm()` at 120 s, then the crash signal
  under its default action; a second crashing thread waits at most 125 s on macOS and Linux, 130 s
  on Windows). The next start logs the snapshots of earlier crashed runs. No logger call after a
  crash.

### Changed
- Requires dvv-base 0.5.0 and dvv-build 0.5.1. A dependent that moves to dvv-platform 0.3.0 also
  lists `dvv-base 0.5.0` (or a later 0.x) in its `dvv-deps.txt`.
- Messages show a path that is not valid UTF-8 with U+FFFD (dvv-base's display text).
- The `DynamicLibrary::open` error for a relative path names `LibrarySearch::LibraryDirectory`.
- Per-OS sources are selected with `dvv_os_sources`; no behaviour change.

### Deprecated (removed in 1.0.0)
Every deprecated name still compiles in 0.3.0; each entry names its replacement.
- `dvv/platform/surface.h` (`surfaceInstanceExtensions`, `createSurface`): no replacement in
  dvv-platform; dvv-gpu 0.3.0 creates its surfaces itself from a `WindowHandle`. The package's
  Vulkan-Headers dependency, the `VK_NO_PROTOTYPES` definition and the QuartzCore link go with it.
- The header `native_window.h`: include `window_handle.h`, `pixel_size.h` and the per-OS header
  (`window_handle_win32.h`, `window_handle_xlib.h`, `window_handle_metal.h`). Reaching
  `PixelSize`, `WindowSystem` or the per-OS structs through native_window.h goes with it; the
  structs `Win32Window`, `XlibWindow` and `MetalWindow` themselves stay (also after 1.0.0), in
  their per-OS headers.
- `NativeWindow`: use `WindowHandle`. `NativeWindow{XlibWindow{display, window}}` becomes
  `windowHandleOf(XlibWindow{display, window})` (the same for the other structs);
  `std::get_if<XlibWindow>(&native)` becomes `xlibWindowOf(handle)`.
- `windowSystemOf(const NativeWindow&)`: use `handle.system()`.
- `windowHandleOf(const NativeWindow&)`: build the `WindowHandle` from the struct.
- `TestWindow::native()`: use `handle()`; `windowHandleOf(window.native())` becomes
  `window.handle()`.
- `LibrarySearch::DllDirectory`, kept as an alias of `LibraryDirectory`: use `LibraryDirectory`.

### Fixed
- The `surfaceInstanceExtensions` comment lists the Metal pair; the README names macOS.

## [0.2.0] - 2026-11-07

### Added
- macOS (Apple Silicon): `WindowSystem::Metal`, `MetalWindow` (a `CAMetalLayer*`) and
  `VK_EXT_metal_surface` surfaces.
- A Cocoa `TestWindow` for display tests (physical pixels, backing scale).
- macOS file reading: `F_NOCACHE` for unbuffered reads, `F_RDAHEAD` for sequential reads.

### Changed
- `NativeWindow` gains the `MetalWindow` alternative; code that visits the variant exhaustively
  must handle it.

## [0.1.0] - 2026-10-28

First versioned release.

### Added
- Native window descriptions (`NativeWindow`, `WindowSystem`, `PixelSize`) for Win32 and Xlib.
- Vulkan surface creation (`surfaceInstanceExtensions`, `createSurface`) through
  `vkGetInstanceProcAddr`, with the pinned Vulkan-Headers and `VK_NO_PROTOTYPES`.
- Run-time libraries (`DynamicLibrary`) and positional file reads (`FileReader`).
- A native test window for display tests (`dvv::platform_testing`).
- The CMake package `dvv_platform` (`find_package(dvv_platform 0.1)`, `SameMajorVersion`); it
  requires dvv-base 0.1.0 or later in the 0.x series.
