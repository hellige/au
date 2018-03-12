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

# Enable libtooling support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_INSTALL_PREFIX ${PROJECT_BINARY_DIR}/dist)

include_directories(SYSTEM rapidjson/include)
add_subdirectory(googletest)

set(BENCHMARK_ENABLE_GTEST_TESTS OFF)
set(BENCHMARK_ENABLE_TESTING OFF)
add_subdirectory(benchmark)

include_directories(src)
link_directories(${PROJECT_BINARY_DIR}/lib)

add_subdirectory(src)
add_subdirectory(benchmarks)
enable_testing()
add_subdirectory(test)
