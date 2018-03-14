# clang -Wextra only for our own code
if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Weverything -Wno-undef -Wno-c++98-compat -Wno-shadow-field-in-constructor -Wno-missing-prototypes -Wno-padded")
    # TODO either fix documentation or remove it, then remove this
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-documentation")
    # TODO decide if this is OK. It probably is
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-exit-time-destructors")
    # TODO should probably remove this and fix unnecessary things once API has settled.
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unneeded-member-function -Wno-unused-member-function -Wno-unused-template")
    # TODO if we ever go non-header only, we can improve things a tiny bit by b ringing some of the vtable definitions out of line
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-weak-vtables")
endif ()


add_library(au-cpp INTERFACE)
include_directories(external/tclap/include)
target_sources(au-cpp INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/au/AuEncoder.h
        ${CMAKE_CURRENT_SOURCE_DIR}/au/AuDecoder.h)
target_include_directories(au-cpp INTERFACE au)
install(DIRECTORY au DESTINATION include)

add_executable(au main.cpp Json2Au.cpp Canned.cpp Stats.cpp Grep.cpp Tail.cpp)
target_link_libraries(au au-cpp)
install(TARGETS au
        RUNTIME DESTINATION bin)

# Release flags (statically linked)
# TODO: should we make this an option? Needs to be off to get the sanitizers to work.
set_target_properties(au PROPERTIES LINK_FLAGS_RELEASE -static)
set_target_properties(au PROPERTIES LINK_FLAGS_RELWITHDEBINFO -static)
