cmake_minimum_required(VERSION 3.9.1)

project(au VERSION 0.2.4 LANGUAGES CXX)

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

# All the sanitizer flags in one place
if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
    set(SANITIZE_FLAGS "${SANITIZE_FLAGS} -fsanitize=address")
    set(SANITIZE_FLAGS "${SANITIZE_FLAGS} -fsanitize=undefined")
else ()
    set(SANITIZE_FLAGS "${SANITIZE_FLAGS} -static-libasan")
    set(SANITIZE_FLAGS "${SANITIZE_FLAGS} -static-libubsan")
    set(SANITIZE_FLAGS "${SANITIZE_FLAGS} -fsanitize=address")
    set(SANITIZE_FLAGS "${SANITIZE_FLAGS} -fsanitize=signed-integer-overflow")
    set(SANITIZE_FLAGS "${SANITIZE_FLAGS} -fsanitize=bounds-strict -fsanitize=undefined")
endif ()

# Common flags shared across all builds
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC -g")
set(DEFAULT_CXX_FLAGS -Wall -Wextra -Werror)

# Debug flags
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${SANITIZE_FLAGS}")

include(CTest)

# Enable libtooling support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# External libraries/dependencies
if (STATIC)
    SET(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
endif ()
find_package(ZLIB REQUIRED)
include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(SYSTEM ${ZLIB_INCLUDE_DIRS} external/rapidjson/include external/tclap/include)
set(BENCHMARK_ENABLE_GTEST_TESTS CACHE BOOL OFF)
set(BENCHMARK_ENABLE_TESTING CACHE BOOL OFF)
add_subdirectory(external/benchmark)
add_subdirectory(external/googletest)

set(CMAKE_INSTALL_PREFIX ${PROJECT_BINARY_DIR}/dist)

set(ABSL_ENABLE_INSTALL ON)
add_subdirectory(external/abseil-cpp)
add_subdirectory(external/re2)

# au configuration
include_directories(src)
link_directories(${PROJECT_BINARY_DIR}/lib)

add_subdirectory(src)
add_subdirectory(benchmarks)
enable_testing()
add_subdirectory(test)
