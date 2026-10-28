cmake_minimum_required(VERSION 4.2)
project(dvv_platform VERSION 0.1.0 DESCRIPTION "dvv platform layer" LANGUAGES CXX)
# Version string; a pre-release build adds its release-stage label (user decision U-M4ORDER).
set(dvv_platform_VERSION_STRING "${PROJECT_VERSION}")

# dvv-build provides the shared CMake modules and the dependency check (dvv-deps.txt); the dvv
# presets set DVV_BUILD_CMAKE_DIR to its cmake folder.
if(NOT DVV_BUILD_CMAKE_DIR OR NOT IS_ABSOLUTE "${DVV_BUILD_CMAKE_DIR}"
   OR NOT EXISTS "${DVV_BUILD_CMAKE_DIR}/DvvDeps.cmake")
    message(FATAL_ERROR "dvv_platform: DVV_BUILD_CMAKE_DIR is not set, not absolute or "
                        "has no DvvDeps.cmake (it must be dvv-build's cmake folder); configure "
                        "with a dvv-build preset (e.g. `cmake --preset vs2026`, see README.md) "
                        "or pass -DDVV_BUILD_CMAKE_DIR=<absolute path of dvv-build>/cmake")
endif()
include("${DVV_BUILD_CMAKE_DIR}/DvvDeps.cmake")
# Adds dvv-base (dvv-deps.txt) and checks its version; a stand-alone build tests only this
# repository.
dvv_dependencies()

option(DVV_BUILD_TESTS "Build the dvv tests" ${PROJECT_IS_TOP_LEVEL})

# dvv-base sets DVV_CMAKE_DIR to the shared modules (in-tree: dvv-build's cmake folder).
if(NOT DVV_CMAKE_DIR)
    message(FATAL_ERROR "dvv_base did not define DVV_CMAKE_DIR")
endif()
include("${DVV_CMAKE_DIR}/DvvCompilerOptions.cmake")
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Vulkan comes only from the pinned packages in the third-party prefix, never from CMake's FindVulkan
# module, which would pick up an installed Vulkan SDK through VULKAN_SDK.
set(CMAKE_DISABLE_FIND_PACKAGE_Vulkan ON)
find_package(VulkanHeaders 1.4.365 EXACT CONFIG REQUIRED)
if(NOT WIN32)
    find_package(X11 REQUIRED)
endif()

if(WIN32)
    set(platform_sources
        src/dynamic_library_win32.cpp src/file_reader_win32.cpp src/surface_win32.cpp)
    set(test_window_source src/testing/test_window_win32.cpp)
else()
    set(platform_sources
        src/dynamic_library_posix.cpp src/file_reader_posix.cpp src/surface_xlib.cpp)
    set(test_window_source src/testing/test_window_xlib.cpp)
endif()
add_library(dvv_platform STATIC
    src/dynamic_library.cpp
    src/file_reader.cpp
    src/native_window.cpp
    src/surface.cpp
    ${platform_sources})
add_library(dvv::platform ALIAS dvv_platform)
set_target_properties(dvv_platform PROPERTIES EXPORT_NAME platform)
target_include_directories(dvv_platform PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
if(NOT WIN32)
    # Only the headers (Display, Window for VkXlibSurfaceCreateInfoKHR); nothing calls Xlib here.
    target_include_directories(dvv_platform SYSTEM PRIVATE ${X11_INCLUDE_DIR})
endif()
# Vulkan's C headers are part of the interface, always without prototypes: every Vulkan function
# comes through vkGetInstanceProcAddr. dlopen on Linux (CMAKE_DL_LIBS is empty on Windows).
target_compile_definitions(dvv_platform PUBLIC VK_NO_PROTOTYPES)
target_link_libraries(dvv_platform PUBLIC dvv::base Vulkan::Headers PRIVATE ${CMAKE_DL_LIBS})
dvv_set_compiler_options(dvv_platform)

# Native test windows for the display tests of dvv-platform, dvv-gpu and rawreel (spec §12).
# Installed and exported: a stand-alone dvv-gpu build may use an installed dvv_platform.
add_library(dvv_platform_testing STATIC ${test_window_source})
add_library(dvv::platform_testing ALIAS dvv_platform_testing)
set_target_properties(dvv_platform_testing PROPERTIES EXPORT_NAME platform_testing)
if(WIN32)
    target_link_libraries(dvv_platform_testing PUBLIC dvv::platform PRIVATE user32)
else()
    target_link_libraries(dvv_platform_testing PUBLIC dvv::platform PRIVATE X11::X11)
endif()
dvv_set_compiler_options(dvv_platform_testing)

install(TARGETS dvv_platform dvv_platform_testing EXPORT dvv_platformTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
set(config_dir ${CMAKE_INSTALL_LIBDIR}/cmake/dvv_platform)
install(EXPORT dvv_platformTargets NAMESPACE dvv:: DESTINATION ${config_dir})
configure_package_config_file(cmake/dvv_platformConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/dvv_platformConfig.cmake INSTALL_DESTINATION ${config_dir})
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/dvv_platformConfigVersion.cmake
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/dvv_platformConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/dvv_platformConfigVersion.cmake
    DESTINATION ${config_dir})

if(DVV_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
