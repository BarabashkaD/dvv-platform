# Changelog

All notable changes to dvv-platform. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow semantic versioning with
the dvv rules: compatible = the same major and at least the minimum version, also for 0.x; breaking
changes only in a new major, after a deprecation. This file is the release notes (there are no
GitHub Releases).

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
