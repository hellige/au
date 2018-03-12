cmake_minimum_required(VERSION 3.9.1)

project(au-cpp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXE_LINKER_FLAGS "${EXTRA_LINKER_FLAGS} ${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=gold")

# All the sanitizer flags in one place
set(SANITIZE_FLAGS "${SANITIZE_FLAGS} -static-libasan -fsanitize=address")
set(SANITIZE_FLAGS "${SANITIZE_FLAGS} -static-libubsan")
set(SANITIZE_FLAGS "${SANITIZE_FLAGS} -fsanitize=signed-integer-overflow")
set(SANITIZE_FLAGS "${SANITIZE_FLAGS} -fsanitize=bounds-strict -fsanitize=undefined")

# Common flags shared across all builds
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC -Wall -Wextra -Werror -g")

# Debug flags
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${SANITIZE_FLAGS}")
# Release flags (statically linked)
# TODO: should we make this an option? Needs to be off to get the sanitizers to work.
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -static")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -static")

# Enable libtooling support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# External libraries/dependencies
include_directories(SYSTEM external/rapidjson/include external/tclap/include)
set(BENCHMARK_ENABLE_GTEST_TESTS CACHE BOOL OFF)
set(BENCHMARK_ENABLE_TESTING CACHE BOOL OFF)
add_subdirectory(external/benchmark)
add_subdirectory(external/googletest)

# au configuration
include_directories(src)
link_directories(${PROJECT_BINARY_DIR}/lib)

add_subdirectory(src)
add_subdirectory(benchmarks)
enable_testing()
add_subdirectory(test)

set(CMAKE_INSTALL_PREFIX ${PROJECT_BINARY_DIR}/dist)