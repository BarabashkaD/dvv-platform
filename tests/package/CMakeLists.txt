# Consumer of dvv_platform; built by run-package-test.cmake. With DVV_PLATFORM_SOURCE_DIR it takes
# dvv_platform in-tree through FetchContent (as a dependent library or the app takes it; then
# DVV_BUILD_CMAKE_DIR names dvv-build's cmake folder), otherwise the installed package.
cmake_minimum_required(VERSION 4.2)
project(dvv_platform_package_consumer LANGUAGES CXX)

if(DVV_PLATFORM_SOURCE_DIR)
    include(FetchContent)
    FetchContent_Declare(dvv_platform SOURCE_DIR "${DVV_PLATFORM_SOURCE_DIR}")
    FetchContent_MakeAvailable(dvv_platform)
else()
    find_package(dvv_platform 0.1 REQUIRED)
endif()
if(NOT DVV_CMAKE_DIR)
    message(FATAL_ERROR "dvv_base did not define DVV_CMAKE_DIR")
endif()
include("${DVV_CMAKE_DIR}/DvvCompilerOptions.cmake")

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE dvv::platform dvv::platform_testing)
dvv_set_compiler_options(consumer)
