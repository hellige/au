option(LTO "Use link-time optimization" OFF)
option(GeneratePGO "Generate PGO information" OFF)
option(UsePGO "User previously-generated PGO information" OFF)

if (LTO)
    message("Enabling link-time optimization")
    include(CheckIPOSupported)
    check_ipo_supported()
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()

if (GeneratePGO)
    if (UsePGO)
        message(FATAL "Can't have both generate and use PGO on at the same time")
    endif ()
    set(PGO "-fprofile-generate=${CMAKE_BINARY_DIR}")
endif ()

if (UsePGO)
    set(PGO "-fprofile-use=${CMAKE_BINARY_DIR}")
endif ()

if (COVERAGE)
    add_compile_options(--coverage -O0)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
endif ()

set(VERSION_FILE "${PROJECT_BINARY_DIR}/au/Version.h")
configure_file(au/Version.template.h ${VERSION_FILE})

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${PGO}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${PGO}")

# clang -Wextra only for our own code
if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Weverything -Wno-undef -Wno-c++98-compat -Wno-c++98-compat-pedantic -Wno-shadow-field-in-constructor -Wno-missing-prototypes -Wno-padded -Wno-float-equal -Wno-missing-noreturn -Wno-vla -Wno-vla-extension")
    # TODO either fix documentation or remove it, then remove this
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-documentation")
    # TODO decide if this is OK. It probably is
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-exit-time-destructors")
    # TODO should probably remove this and fix unnecessary things once API has settled.
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unneeded-member-function -Wno-unused-member-function -Wno-unused-template")
    # TODO if we ever go non-header only, we can improve things a tiny bit by bringing some of the vtable definitions out of line
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-weak-vtables")
else ()
    # enable some more gcc warnings to match clang
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wconversion -Wold-style-cast")
endif ()


add_library(libau INTERFACE)
include_directories(SYSTEM ../external/rapidjson/include ../external/tclap/include)
include_directories(au)
target_sources(libau INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/au/AuEncoder.h
        ${CMAKE_CURRENT_SOURCE_DIR}/au/AuDecoder.h)
target_include_directories(libau INTERFACE .)
target_compile_options(libau INTERFACE ${DEFAULT_CXX_FLAGS})
install(DIRECTORY au DESTINATION include)

add_executable(au main.cpp CatCmd.cpp Json2Au.cpp Stats.cpp Grep.cpp Tail.cpp ZindexCmd.cpp Zindex.cpp)
target_link_libraries(au libau ${ZLIB_LIBRARIES} re2::re2)
install(TARGETS au
        RUNTIME DESTINATION bin)

if (STATIC)
    set_target_properties(au PROPERTIES LINK_FLAGS_DEBUG -static)
    set_target_properties(au PROPERTIES LINK_FLAGS_RELEASE -static)
    set_target_properties(au PROPERTIES LINK_FLAGS_RELWITHDEBINFO -static)
    set_target_properties(au PROPERTIES LINK_FLAGS_MINSIZEREL -static)
endif ()
