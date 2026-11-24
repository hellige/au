cmake_minimum_required(VERSION 3.9.1)

project(au VERSION 0.6.0 LANGUAGES C CXX)

option(STATIC "Build statically linked binary" OFF)
option(COVERAGE "Build with code coverage enabled" OFF)

execute_process(
        COMMAND git describe --tags --always --dirty --match v*
        OUTPUT_VARIABLE GIT_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
)
if (NOT "v${PROJECT_VERSION}" STREQUAL "${GIT_VERSION}")
    message(STATUS, "This doesn't really seem to be verion ${PROJECT_VERSION}...")
    set(PROJECT_VERSION "snapshot-${GIT_VERSION}")
    message(STATUS, "Overriding to ${PROJECT_VERSION}!")
endif ()

message(STATUS, "Coverage: ${COVERAGE}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXE_LINKER_FLAGS "${EXTRA_LINKER_FLAGS} ${CMAKE_EXE_LINKER_FLAGS}")

# Sanitizer flags (Debug only; applied to our own code below, not to vendored externals)
if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
    set(SANITIZE_FLAGS -fsanitize=address -fsanitize=undefined)
else ()
    set(SANITIZE_FLAGS
            -static-libasan -static-libubsan
            -fsanitize=address
            -fsanitize=signed-integer-overflow
            -fsanitize=bounds-strict -fsanitize=undefined)
endif ()

# Common flags shared across all builds
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC -g")
set(DEFAULT_CXX_FLAGS -Wall -Wextra -Werror)

if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
    # Disable Clang's unsafe-buffer-usage warning (Clang 16+)
    # Also suppress unknown-warning-option for older Clang versions
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unknown-warning-option -Wno-unsafe-buffer-usage")
    # Disable bogus cross-compilation warning on macOS
    if (APPLE)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-poison-system-directories")
    endif ()
endif ()

# Helper: apply sanitizer flags to a target in Debug builds. Scoped to our own
# targets so vendored externals (abseil, re2, ...) stay un-instrumented; some
# of them have constexpr code that breaks under sanitizer instrumentation.
function(au_enable_sanitizers target)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:${SANITIZE_FLAGS}>)
    target_link_options(${target} PRIVATE $<$<CONFIG:Debug>:${SANITIZE_FLAGS}>)
endfunction()

include(CTest)

# Enable libtooling support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# External libraries/dependencies
if (STATIC)
    SET(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
endif ()
find_package(ZLIB REQUIRED)
include_directories(SYSTEM ${ZLIB_INCLUDE_DIRS} external/rapidjson/include external/tclap/include)
set(BENCHMARK_ENABLE_GTEST_TESTS CACHE BOOL OFF)
set(BENCHMARK_ENABLE_TESTING CACHE BOOL OFF)
add_subdirectory(external/benchmark)
add_subdirectory(external/googletest)

set(CMAKE_INSTALL_PREFIX ${PROJECT_BINARY_DIR}/dist)

set(ABSL_ENABLE_INSTALL ON)
add_subdirectory(external/abseil-cpp)
add_subdirectory(external/re2)

# re2 exposes its include path as a normal (non-SYSTEM) interface include,
# which causes our strict warning flags (e.g. -Wzero-as-null-pointer-constant
# under clang's -Weverything) to fire on re2's own headers. Mark it SYSTEM.
get_target_property(_re2_inc re2 INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(re2 PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_re2_inc}")

# zstd: only the static library; we build the seekable-format contrib code
# ourselves, since zstd's cmake doesn't expose it as a library.
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_DICTBUILDER OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
add_subdirectory(external/zstd/build/cmake external/zstd EXCLUDE_FROM_ALL)
set(ZSTD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/zstd)
add_library(zstd_seekable STATIC
        ${ZSTD_DIR}/contrib/seekable_format/zstdseek_compress.c
        ${ZSTD_DIR}/contrib/seekable_format/zstdseek_decompress.c)
target_compile_definitions(zstd_seekable PRIVATE XXH_NAMESPACE=ZSTD_)
# lib/common is needed for xxhash.h (we compute frame checksums ourselves) and
# for mem.h (used internally by the seekable code).
target_include_directories(zstd_seekable SYSTEM PUBLIC
        ${ZSTD_DIR}/lib ${ZSTD_DIR}/lib/common ${ZSTD_DIR}/contrib/seekable_format)
target_link_libraries(zstd_seekable PUBLIC libzstd_static)

# au configuration
# Note: our include paths (src/, and the build dir for the generated
# au/Version.h) are attached to the libau target rather than set globally
# with include_directories(), so they don't leak into external targets. In
# particular, src/ZstdSeekable.h must not be visible to zstd's own sources,
# which include "zstd.h": on case-insensitive filesystems the two collide.
link_directories(${PROJECT_BINARY_DIR}/lib)

add_subdirectory(src)
add_subdirectory(benchmarks)
enable_testing()
add_subdirectory(test)
