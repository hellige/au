cmake_minimum_required(VERSION 3.9)
project(au)

set(CMAKE_CXX_STANDARD 17)

file(GLOB_RECURSE SOURCE_FILES src/*.cpp src/*.h src/*.hpp)
file(GLOB_RECURSE TEST_FILES tests/*.cpp tests/*.h tests/*.hpp)
file(GLOB_RECURSE BENCHMARK_FILES benchmarks/*.cpp benchmarks/*.h benchmarks/*.hpp)

include_directories(src tests)
include_directories(SYSTEM include)

add_custom_target(au-test
        make test
        COMMENT making tests
        WORKING_DIRECTORY ${au_SOURCE_DIR})
add_custom_target(au-benchmark
        make benchmark
        COMMENT making benchmarks
        WORKING_DIRECTORY ${au_SOURCE_DIR})

add_executable(au ${SOURCE_FILES})