.DEFAULT_GOAL := all
.PHONY: all clean submodules

# Points to the root of Google Test, relative to where this file is.
# Remember to tweak this if you move this file.
GTEST_DIR = googletest/googletest

# All Google Test headers.  Usually you shouldn't change this
# definition.
GTEST_HEADERS = $(GTEST_DIR)/include/gtest/*.h \
                $(GTEST_DIR)/include/gtest/internal/*.h

# Usually you shouldn't tweak such internal variables, indicated by a
# trailing _.
GTEST_SRCS_ = $(GTEST_DIR)/src/*.cc $(GTEST_DIR)/src/*.h $(GTEST_HEADERS)

GBM_DIR = benchmark
GBM_INSTALL = benchmark/build

# Flags passed to the preprocessor.
# Set Google Test's header directory as a system directory, such that
# the compiler doesn't generate warnings in Google Test headers.
CPPFLAGS += -isystem $(GTEST_DIR)/include
CPPFLAGS += -Ibenchmark/build/include

# Flags passed to the C++ compiler.
CXXFLAGS += -g -Wall -Wextra -pthread -std=c++17
CXXFLAGS += -Lbenchmark/build/lib -lbenchmark

TESTS = unit_tests au

BENCHMARK_DIR = benchmarks
BENCHMARK_SRCS = $(shell find $(BENCHMARK_DIR) -name '*.cpp')

submodules: googletest
	@git submodule init
	@git submodule update

gtest: submodules gtest.a gtest_main.a

gbenchmark: submodules
	mkdir -p $(GBM_INSTALL)
	cd benchmark/build && cmake .. -DMAKE_BUILD_TYPE=RELEASE -DCMAKE_INSTALL_PREFIX=. -DBENCHMARK_DOWNLOAD_DEPENDENCIES=ON && make install

all: gtest $(TESTS)

clean :
	rm -f $(TESTS) gtest.a gtest_main.a *.o

test: au unit_tests
	./unit_tests

# Builds gtest.a and gtest_main.a.

# For simplicity and to avoid depending on Google Test's
# implementation details, the dependencies specified below are
# conservative and not optimized.  This is fine as Google Test
# compiles fast and for ordinary users its source rarely changes.
gtest-all.o : $(GTEST_SRCS_)
	$(CXX) $(CPPFLAGS) -I$(GTEST_DIR) $(CXXFLAGS) -c \
            $(GTEST_DIR)/src/gtest-all.cc

gtest_main.o : $(GTEST_SRCS_)
	$(CXX) $(CPPFLAGS) -I$(GTEST_DIR) $(CXXFLAGS) -c \
            $(GTEST_DIR)/src/gtest_main.cc

gtest.a : gtest-all.o
	$(AR) $(ARFLAGS) $@ $^

gtest_main.a : gtest-all.o gtest_main.o
	$(AR) $(ARFLAGS) $@ $^

au: au.cpp
	$(CXX) -std=c++17 -g -o au au.cpp
	./au | od -tcz -tu1

unit_tests: unit_tests.cpp gtest_main.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -lpthread $^ -o $@

au_benchmarks: $(BENCHMARK_SRCS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $^

