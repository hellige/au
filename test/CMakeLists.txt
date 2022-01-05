if (COVERAGE)
    add_compile_options(--coverage -O0)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
endif ()

# this is probably only needed for gcc 8. once that's gone, try again without
# this.
if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    set(CXX_FS_LIB "stdc++fs")
endif()

add_executable(Test
        AuUnitTests.cpp AuEncoderTests.cpp
        AuDecoderTests.cpp AuDecoderTestCases.cpp
        HelpersTest.cpp TimestampPatternTest.cpp)
target_link_libraries(Test libau gtest gtest_main gmock ${CXX_FS_LIB})
add_test(NAME Tests
        COMMAND Test
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
file(COPY cases DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

add_custom_target(unittest Test
        COMMENT "Running unit tests\n\n"
        VERBATIM
        )
