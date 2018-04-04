add_executable(Test
        AuUnitTests.cpp
        AuDecoderTests.cpp AuDecoderTestCases.cpp)
target_link_libraries(Test au-cpp gtest gtest_main gmock stdc++fs)
add_test(NAME Tests
        COMMAND Test
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
file(COPY cases DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
