add_library(au-cpp INTERFACE)
include_directories(external/tclap/include)
target_sources(au-cpp INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/au/AuEncoder.h
        ${CMAKE_CURRENT_SOURCE_DIR}/au/AuDecoder.h)
target_include_directories(au-cpp INTERFACE au)
install(DIRECTORY au DESTINATION include)

add_executable(au main.cpp Json2Au.cpp Canned.cpp Stats.cpp Grep.cpp)
target_link_libraries(au au-cpp)
install(TARGETS au
        RUNTIME DESTINATION bin)
