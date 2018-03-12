add_library(au-cpp INTERFACE)
target_sources(au-cpp INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/au/AuEncoder.h
        ${CMAKE_CURRENT_SOURCE_DIR}/au/AuDecoder.h)
target_include_directories(au-cpp INTERFACE au)
install(DIRECTORY au DESTINATION include)

add_executable(au main.cpp Json2Au.cpp)
target_link_libraries(au au-cpp)
install(TARGETS au
        RUNTIME DESTINATION bin)

# TODO: sample doesn't seem to build
#add_executable(sample sample.cpp)
#target_link_libraries(sample au-cpp)
#add_dependencies(sample FigUpToDate)
