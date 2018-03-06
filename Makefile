.DEFAULT_GOAL := all
.PHONY: all clean submodules

GBM_DIR = external/benchmark
GBM_INSTALL = $(GBM_DIR)/build

# Points to the root of Google Test, relative to where this file is.
# Remember to tweak this if you move this file.
GTEST_DIR = $(GBM_INSTALL)/googletest

# Flags passed to the preprocessor.
# Set Google Test's header directory as a system directory, such that
# the compiler doesn't generate warnings in Google Test headers.
CPPFLAGS += -isystem $(GTEST_DIR)/include

# Flags passed to the C++ compiler.
CXXFLAGS += -g -Wall -Wextra -pthread -std=c++17
BENCHMARK_FLAGS += -I $(GBM_INSTALL)/include


BENCHMARK_DIR = benchmarks
BENCHMARK_SRCS = $(shell find $(BENCHMARK_DIR) -name '*.cpp')

TESTS = unit_tests au

submodules:
	@git submodule init
	@git submodule update

gbenchmark: submodules
	mkdir -p $(GBM_INSTALL)
	cd $(GBM_INSTALL) && cmake .. -DMAKE_BUILD_TYPE=RELEASE -DCMAKE_INSTALL_PREFIX=. -DBENCHMARK_DOWNLOAD_DEPENDENCIES=ON && make install

$(GTEST_DIR)/lib/libgtest_main.a: gbenchmark

all: $(TESTS)
	./unit_tests

clean :
	rm -f $(TESTS) gtest.a gtest_main.a *.o
	rm -f benchmark
	#cd $(GBM_INSTALL) && make clean

test: au unit_tests
	./unit_tests

au: au.cpp
	$(CXX) -std=c++17 -g -o au au.cpp
	./au | od -tcz -tu1

unit_tests: unit_tests.cpp $(GTEST_DIR)/lib/libgtest_main.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -lpthread $^ -o $@

benchmark: $(BENCHMARK_SRCS) $(GBM_INSTALL)/lib/libbenchmark.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(BENCHMARK_FLAGS) -o $@ $^

