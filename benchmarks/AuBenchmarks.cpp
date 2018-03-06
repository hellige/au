#include <benchmark/benchmark.h>

#include <sstream>

static void BM_OStringStreamCreation(benchmark::State &state) {
    for (auto _ : state)
        std::ostringstream os;
}
BENCHMARK(BM_OStringStreamCreation);

static void BM_VectorCreation(benchmark::State &state) {
    for (auto _ : state)
        std::vector<char> v;
}
BENCHMARK(BM_VectorCreation);

BENCHMARK_MAIN();
