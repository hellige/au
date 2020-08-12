if (COVERAGE)
    add_compile_options(--coverage -O0)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
endif ()

add_executable(Test
        AuUnitTests.cpp AuEncoderTests.cpp
        AuDecoderTests.cpp AuDecoderTestCases.cpp
        HelpersTest.cpp)
target_link_libraries(Test libau gtest gtest_main gmock stdc++fs)
add_test(NAME Tests
        COMMAND Test
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
file(COPY cases DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

add_custom_target(unittest Test
        COMMENT "Running unit tests\n\n"
        VERBATIM
        )
