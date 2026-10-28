find_package(doctest 2.5 REQUIRED CONFIG)

# A tiny run-time library for the DynamicLibrary tests. It is built into the same output directory
# as the test executable (both are defined here), so LibrarySearch::SystemDirectory must not find
# it while the default search does.
add_library(dvv_platform_test_module MODULE test_module.cpp)
dvv_set_compiler_options(dvv_platform_test_module)

# LibrarySearch::DllDirectory: a run-time library whose dependency sits in its own folder, away from
# the test executable and the working directory, so only a search of the library's own folder
# finds it. $<CONFIG> in the folder keeps multi-config generators from adding their own subfolder.
set(dll_directory "${CMAKE_CURRENT_BINARY_DIR}/dll-directory/$<CONFIG>")
add_library(dvv_platform_test_dependency SHARED test_dependency.cpp)
add_library(dvv_platform_test_dependent MODULE test_dependent.cpp)
target_link_libraries(dvv_platform_test_dependent PRIVATE dvv_platform_test_dependency)
set_target_properties(dvv_platform_test_dependency dvv_platform_test_dependent PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${dll_directory}"
    LIBRARY_OUTPUT_DIRECTORY "${dll_directory}"
    BUILD_RPATH "$ORIGIN")
dvv_set_compiler_options(dvv_platform_test_dependency)
dvv_set_compiler_options(dvv_platform_test_dependent)

add_executable(dvv_platform_tests
    main.cpp
    test_dynamic_library.cpp
    test_file_reader.cpp
    test_native_window.cpp)
target_link_libraries(dvv_platform_tests PRIVATE dvv::platform doctest::doctest)
# The FileReader tests use the private seam header (src/file_reader_detail.h).
target_include_directories(dvv_platform_tests PRIVATE ${dvv_platform_SOURCE_DIR}/src)
# doctest otherwise only forward-declares std::ostream, which breaks printing std types.
target_compile_definitions(dvv_platform_tests PRIVATE
    DOCTEST_CONFIG_USE_STD_HEADERS
    "DVV_PLATFORM_TEST_MODULE=\"$<TARGET_FILE:dvv_platform_test_module>\""
    "DVV_PLATFORM_TEST_MODULE_NAME=\"$<TARGET_FILE_NAME:dvv_platform_test_module>\""
    "DVV_PLATFORM_TEST_DEPENDENT=\"$<TARGET_FILE:dvv_platform_test_dependent>\"")
add_dependencies(dvv_platform_tests dvv_platform_test_module dvv_platform_test_dependent)
dvv_set_compiler_options(dvv_platform_tests)
add_test(NAME dvv_platform.unit COMMAND dvv_platform_tests)
# The timeout turns a hang into a failure long before the CI job limit.
set_tests_properties(dvv_platform.unit PROPERTIES
    TIMEOUT 300
    ENVIRONMENT DVV_LEAK_CHECK=fail
    ENVIRONMENT_MODIFICATION "${DVV_TEST_ENVIRONMENT}")
# Cycle labels: `sanity` marks the fast inner-loop subset (ctest --preset vs2026-sanity); `package`
# marks install and consumer tests, which only the full cycle runs (vs2026-small excludes them).
set_property(TEST dvv_platform.unit APPEND PROPERTY LABELS sanity)

# Display tests (spec §12, label display): a real window and a Vulkan surface for it. The executable
# exits with 77 (skipped) without a Vulkan loader, a driver with the surface extensions or a display,
# unless DVV_REQUIRE_GPU=1.
add_executable(dvv_platform_surface_tests
    surface_main.cpp
    surface_support.cpp
    test_surface.cpp
    test_test_window.cpp)
target_link_libraries(dvv_platform_surface_tests PRIVATE dvv::platform dvv::platform_testing
                                                         doctest::doctest)
target_compile_definitions(dvv_platform_surface_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
dvv_set_compiler_options(dvv_platform_surface_tests)
add_test(NAME dvv_platform.surface COMMAND dvv_platform_surface_tests)
set_tests_properties(dvv_platform.surface PROPERTIES
    LABELS display
    SKIP_RETURN_CODE 77
    TIMEOUT 120
    ENVIRONMENT DVV_LEAK_CHECK=fail
    ENVIRONMENT_MODIFICATION "${DVV_TEST_ENVIRONMENT}")
set_property(TEST dvv_platform.surface APPEND PROPERTY LABELS sanity)

# Package test (spec §3.1): install, then build a consumer with find_package(dvv_platform) and
# another with dvv_platform in-tree. The whole build tree is installed (CMAKE_BINARY_DIR), so the
# prefix also holds dvv_base, which dvv_dependencies() always adds as a subproject. Skipped for
# sanitizer builds, whose consumers would need the same runtime flags.
if(NOT DVV_SANITIZE)
    string(REPLACE ";" "," deps_prefix_path "${CMAKE_PREFIX_PATH}")
    add_test(NAME dvv_platform.package
             COMMAND ${CMAKE_COMMAND}
                     -DBUILD_DIR=${CMAKE_BINARY_DIR}
                     -DCONFIG=$<CONFIG>
                     -DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/package-prefix
                     -DSOURCE_DIR=${dvv_platform_SOURCE_DIR}
                     -DCONSUMER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/package
                     -DCONSUMER_BUILD=${CMAKE_CURRENT_BINARY_DIR}/package-consumer
                     -DCONSUMER_TREE_BUILD=${CMAKE_CURRENT_BINARY_DIR}/pkg-tree
                     -DGENERATOR=${CMAKE_GENERATOR}
                     -DPLATFORM=${CMAKE_GENERATOR_PLATFORM}
                     -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
                     -DDEPS_PREFIX_PATH=${deps_prefix_path}
                     -DDVV_BUILD_CMAKE_DIR=${DVV_BUILD_CMAKE_DIR}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/package/run-package-test.cmake)
    # Package and compile-fail tests of every dvv repository drive the same build tree in a
    # dependent build with DVV_TEST_DEPENDENCIES, so they share one lock name.
    set_tests_properties(dvv_platform.package PROPERTIES
        RESOURCE_LOCK dvv_build_tree
        LABELS package)
endif()
