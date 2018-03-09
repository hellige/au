cmake_minimum_required(VERSION 3.2.2)

set(GCC_VERSION "7.2.0-1" CACHE STRING "Version of GCC to use (e.g. 7.2.0-1)")
option(RUNNING_UNDER_TEAMCITY "Are we running under team city?" OFF)
option(AFL "Compile and run under American Fuzzy Lop" OFF)
option(CLANG_COMPATIBLE "Remove any flags that clang doesn't like" OFF)


#######################
# All this stuff has to be before the project() line
# NB no CMAKE_*_FLAGS can be set here as the project() line compiler discovery
# process overwrites them.
execute_process(
        COMMAND gcc -print-multiarch
        OUTPUT_VARIABLE GCC_MULTIARCH
        OUTPUT_STRIP_TRAILING_WHITESPACE)
if (NOT GCC_MULTIARCH STREQUAL "")
    message("Using multiarch at ${GCC_MULTIARCH}")
    set(ENV{LIBRARY_PATH} /usr/lib/${GCC_MULTIARCH}) # used only for compiler discovery; not propagated to the build
    set(EXTRA_LINKER_FLAGS "${EXTRA_LINKER_FLAGS} -B/usr/lib/${GCC_MULTIARCH}")
    set(EXTRA_COMPILER_FLAGS "${EXTRA_COMPILER_FLAGS} -I/usr/include/${GCC_MULTIARCH}")
endif ()
set(FIG env FIG_REMOTE_URL=ftp://devnas/builds/Fig/repos fig)
execute_process(
        COMMAND ${FIG} --no-file --suppress-cleanup-of-retrieves --update-if-missing
        -g GCC_HOME gcc/${GCC_VERSION}
        OUTPUT_STRIP_TRAILING_WHITESPACE
        OUTPUT_VARIABLE GCC_HOME)
message("Using compiler at ${GCC_HOME}")
set(EXTRA_LINKER_FLAGS "${EXTRA_LINKER_FLAGS} -Wl,-rpath,${GCC_HOME}/lib64")
set(CMAKE_LIBRARY_PATH ${CMAKE_LIBRARY_PATH} /usr/lib/x86_64-linux-gnu)
set(CMAKE_C_COMPILER "${GCC_HOME}/bin/gcc")
set(CMAKE_CXX_COMPILER "${GCC_HOME}/bin/g++")
#######################


project(au-cpp)

set(FIG_FILE "${PROJECT_BINARY_DIR}/package.fig")
configure_file(${CMAKE_SOURCE_DIR}/package.fig ${FIG_FILE})

#######################
# Compiler flags
#set(CMAKE_CXX_STANDARD 14) -- one day we'll be able to put '17' here but that requires CMake 3.8
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${EXTRA_COMPILER_FLAGS} -std=c++1z")
set(CMAKE_EXE_LINKER_FLAGS "${EXTRA_LINKER_FLAGS} ${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=gold -pthread")

if (AFL)
    configure_file(${CMAKE_SOURCE_DIR}/test/afl-g++ ${PROJECT_BINARY_DIR}/afl-g++)
    set(CMAKE_CXX_COMPILER ${PROJECT_BINARY_DIR}/afl-g++)
elseif (CLANG_COMPATIBLE)
    # We don't actually build with clang, but it doesn't like our sanitizer flags
else ()
    # All the sanitizer flags in one place
    set(SANITIZE_FLAGS "${SANITIZE_FLAGS} -static-libasan -fsanitize=address")
    set(SANITIZE_FLAGS "${SANITIZE_FLAGS} -static-libubsan")
    set(SANITIZE_FLAGS "${SANITIZE_FLAGS} -fsanitize=signed-integer-overflow")
    set(SANITIZE_FLAGS "${SANITIZE_FLAGS} -fsanitize=bounds-strict -fsanitize=undefined")
endif ()

# Common flags shared across all builds
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC -Wall -Wextra -Werror -march=core2 -g")

# Debug flags
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${SANITIZE_FLAGS} -fno-lto")

# CMake has some weird ideas about "Release" and "RelWithDebInfo" builds. By default
# "Release" is "-O3" and "RelWithDebInfo" is "-g -O2". We put "-g" in the common
# options above, and use the same flags in both Release and RelWithDebInfo
# We also link statically in release
set(OPTIMIZE_FLAGS "-DNDEBUG -O3 -static")
if (NOT CLANG_COMPATIBLE)
    set(OPTIMIZE_FLAGS "${OPTIMIZE_FLAGS} -flto=8 -ffat-lto-objects")
endif ()

set(CMAKE_CXX_FLAGS_RELEASE "${OPTIMIZE_FLAGS}")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${OPTIMIZE_FLAGS}")
#######################

# Enable libtooling support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_INSTALL_PREFIX ${PROJECT_BINARY_DIR}/dist)

add_custom_command(
        OUTPUT ${PROJECT_BINARY_DIR}/.fig_up_to_date
        DEPENDS ${PROJECT_BINARY_DIR}/package.fig
        COMMAND ${FIG}
        ARGS -m --config build --file ${PROJECT_BINARY_DIR}/package.fig
        --log-level warning
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
        COMMAND touch ARGS ${PROJECT_BINARY_DIR}/.fig_up_to_date)

add_custom_target(FigUpToDate DEPENDS ${PROJECT_BINARY_DIR}/.fig_up_to_date)

include_directories(SYSTEM ${PROJECT_BINARY_DIR}/include)
include_directories(src)
link_directories(${PROJECT_BINARY_DIR}/lib)

add_subdirectory(src)
add_subdirectory(benchmarks)
enable_testing()
add_subdirectory(test)

add_custom_target(
        publish-local
        DEPENDS au-cpp
        COMMAND "${CMAKE_COMMAND}" --build . --target install
        COMMAND ${FIG} --publish-local au-cpp/local)

if (RUNNING_UNDER_TEAMCITY)
    add_custom_target(
            publish
            DEPENDS au-cpp
            COMMAND "${CMAKE_COMMAND}" --build . --target install
            COMMAND ${FIG} --publish au-cpp/${VERSION}-gcc${GCC_VERSION})
endif ()
