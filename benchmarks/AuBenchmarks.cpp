#include "AuEncoder.h"

#include <benchmark/benchmark.h>

#include <sstream>
#include <string.h>

static void BM_OStringStreamCreation(benchmark::State &state) {
  for (auto _ : state)
    std::ostringstream os;
}
BENCHMARK(BM_OStringStreamCreation);


static void BM_OssCreateAndWrite(benchmark::State &state) {
  int len = state.range(0);
  char *msg = (char*)malloc(len);

  for (auto _ : state) {
    std::ostringstream os;
    os.write(msg, len);
    benchmark::DoNotOptimize(os.str().c_str());
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_OssCreateAndWrite)->Range(1, 1<<8);


static void BM_VectorCreation(benchmark::State &state) {
  for (auto _ : state)
    std::vector<char> v;
}
BENCHMARK(BM_VectorCreation);


static void BM_VectorCreateAndWrite(benchmark::State &state) {
  int len = state.range(0);
  char *msg = (char*)malloc(len);

  for (auto _ : state) {
    std::vector<char> v;
    v.reserve(len);
    benchmark::DoNotOptimize(v.data());
    std::copy(msg, msg + len, std::back_inserter(v));
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_VectorCreateAndWrite)->Range(1, 1<<8);


static void BM_CharBufCreateAndCopy(benchmark::State &state) {
  int len = state.range(0);
  char *msg = (char*)malloc(len);

  for (auto _ : state) {
    char *buf = (char*)malloc(len);
    benchmark::DoNotOptimize(::memcpy(buf, msg, len));
    free(buf);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_CharBufCreateAndCopy)->Range(1, 1<<8);


static void BM_StringInternInsert(benchmark::State &state, bool force) {
  size_t elems = state.range(0);
  AuStringIntern stringIntern;

  for (auto _ : state) {
    for (size_t elem = 0; elem < elems; ++elem) {
      state.PauseTiming();
      std::ostringstream os;
      os << "value_" << elem;
      auto val = os.str();
      state.ResumeTiming();

      stringIntern.idx(val, force);
    }
  }
}
BENCHMARK_CAPTURE(BM_StringInternInsert, Forced,   true)->Range(1, 1<<16);
BENCHMARK_CAPTURE(BM_StringInternInsert, Unforced, false)->Range(1, 1<<16);


static void BM_StringInternLookup(benchmark::State &state, bool force) {
  size_t elems = state.range(0);
  AuStringIntern stringIntern;
  for (size_t i = 0; i < elems; ++i) {
    std::ostringstream os;
    os << "value_" << i;
    stringIntern.idx(os.str(), force);
  }

  for (auto _ : state) {
    stringIntern.idx(std::string_view("value_58"));
  }
}
BENCHMARK_CAPTURE(BM_StringInternLookup, Forced,   true)->Range(1, 1<<16);
BENCHMARK_CAPTURE(BM_StringInternLookup, Unforced, false)->Range(1, 1<<16);


BENCHMARK_MAIN();
