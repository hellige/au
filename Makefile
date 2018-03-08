.DEFAULT_GOAL := all
.PHONY: all clean submodules

GBM_DIR = external/benchmark
GBM_INSTALL = $(GBM_DIR)/build

# Points to the root of Google Test, relative to where this file is.
# Google Benchmark downloads and builds it, so we reuse that build.
# Remember to tweak this if you move this file.
GTEST_DIR = $(GBM_INSTALL)/googletest

# Flags passed to the preprocessor.
# Set Google Test's header directory as a system directory, such that
# the compiler doesn't generate warnings in Google Test headers.
CPPFLAGS += -isystem $(GTEST_DIR)/include

# Flags passed to the C++ compiler.
CXXFLAGS += -ggdb3 -Wall -Wextra -pthread -std=c++17 -O3
BENCHMARK_FLAGS += -I src -I $(GBM_INSTALL)/include


BENCHMARK_DIR = benchmarks
BENCHMARK_SRCS = $(shell find $(BENCHMARK_DIR) -name '*.cpp')

TEST_DIR = tests
TEST_SRCS = $(shell find $(TEST_DIR) -name '*.cpp')

submodules:
	@git submodule init
	@git submodule update

gbenchmark: submodules
	mkdir -p $(GBM_INSTALL)
	cd $(GBM_INSTALL) && cmake .. -DMAKE_BUILD_TYPE=RELEASE -DCMAKE_INSTALL_PREFIX=. -DBENCHMARK_DOWNLOAD_DEPENDENCIES=ON && make install

$(GTEST_DIR)/lib/libgtest_main.a: gbenchmark
$(GBM_INSTALL)/lib/libbenchmark.a: gbenchmark

all: au

clean:
	rm -f au
	rm -f test
	rm -f benchmark
	rm -rf $(GBM_INSTALL)
	#cd $(GBM_INSTALL) && make clean

test: $(TEST_SRCS) $(GTEST_DIR)/lib/libgtest_main.a $(GTEST_DIR)/lib/libgtest.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -I src -lpthread $^ -o $@
	./test

au: src/main.cpp
	$(CXX) $(CXXFLAGS) -Isrc -Iinclude -static $^ -o $@

benchmark: $(BENCHMARK_SRCS) $(GBM_INSTALL)/lib/libbenchmark.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(BENCHMARK_FLAGS) -o $@ $^
	./benchmark

