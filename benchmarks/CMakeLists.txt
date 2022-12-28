add_executable(aubenchmark AuBenchmarks.cpp)
target_link_libraries(aubenchmark libau benchmark)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-but-set-variable")
