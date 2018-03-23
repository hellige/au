add_executable(Test
        AuUnitTests.cpp
        AuDecoderTests.cpp)
target_link_libraries(Test au-cpp gtest gtest_main)
add_test(NAME Tests
        COMMAND Test
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
