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

#include(../CMakeLists.txt)
