#include "au/AuEncoder.h"
#include "au/AuDecoder.h"
#include "au/FileByteSource.h"

#include <benchmark/benchmark.h>

#include <random>
#include <sstream>
#include <string.h>

static void BM_FileByteSource(benchmark::State &state) {
  size_t buffSz = state.range(0);
  au::FileByteSourceImpl src("/dev/urandom", buffSz);
  size_t sum = 0;

  for (auto _ : state) {
    for (size_t i = 0; i < buffSz * 1024 * 100; ++i) {
      sum += src.next().charValue();
    }
  }
}
BENCHMARK(BM_FileByteSource)->RangeMultiplier(2)->Range(16, 1<<8);

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


static void BM_StringInternInsert(benchmark::State &state, bool force, size_t cnt = 1) {
  size_t elems = state.range(0);
  au::AuStringIntern stringIntern;

  for (auto _ : state) {
    for (size_t elem = 0; elem < elems; ++elem) {
      state.PauseTiming();
      std::ostringstream os;
      for (unsigned i = 0; i < cnt; ++i) {
        os << "value_";
      }
      os << elem;
      auto val = os.str();
      state.ResumeTiming();

      stringIntern.idx(val, force ? au::AuIntern::ForceIntern : au::AuIntern::ForceExplicit);
    }
  }
}
BENCHMARK_CAPTURE(BM_StringInternInsert, Forced_Short,   true,   1)->Range(1, 1<<16);
BENCHMARK_CAPTURE(BM_StringInternInsert, Forced_Long,    true,  25)->Range(1, 1<<16);
BENCHMARK_CAPTURE(BM_StringInternInsert, Unforced_Short, false,  1)->Range(1, 1<<16);
BENCHMARK_CAPTURE(BM_StringInternInsert, Unforced_Long,  false, 25)->Range(1, 1<<16);


static void BM_StringInternLookup(benchmark::State &state, bool force, size_t cnt = 1) {
  size_t elems = state.range(0);
  au::AuStringIntern stringIntern;
  for (size_t i = 0; i < elems; ++i) {
    std::ostringstream os;
    for (unsigned i = 0; i < cnt; ++i) {
      os << "value_" << i;
    }
    stringIntern.idx(os.str(), force ? au::AuIntern::ForceIntern : au::AuIntern::ForceExplicit);
  }

  for (auto _ : state) {
    stringIntern.idx(std::string_view("value_58"), au::AuIntern::ByFrequency);
  }
}
BENCHMARK_CAPTURE(BM_StringInternLookup, Forced_Short,   true,   1)->Range(1, 1<<16);
BENCHMARK_CAPTURE(BM_StringInternLookup, Forced_Long,    true,  25)->Range(1, 1<<16);
BENCHMARK_CAPTURE(BM_StringInternLookup, Unforced_Short, false,  1)->Range(1, 1<<16);
BENCHMARK_CAPTURE(BM_StringInternLookup, Unforced_Long,  false, 25)->Range(1, 1<<16);

struct BM_AuWriter : au::AuWriter {
  au::AuVectorBuffer bmMsgBuf_;
  au::AuStringIntern bmStringIntern_;

  BM_AuWriter() : au::AuWriter(bmMsgBuf_, bmStringIntern_) {}

  void valueInt(uint64_t i) {
    AuWriter::valueInt(i);
  }
};

static void BM_valueInt_Noop(benchmark::State &state) {
  BM_AuWriter writer;

  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<uint64_t> distrib(0);

  for (auto _ : state) {
    state.PauseTiming();
    [[maybe_unused]] uint64_t val = distrib(gen);
    state.ResumeTiming();

    for (int i = 0; i < 1000; ++i) {
      asm volatile("" : : : "memory");
    }
  }
}
BENCHMARK(BM_valueInt_Noop);

static void BM_valueInt(benchmark::State &state) {
  BM_AuWriter writer;

  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<uint64_t> distrib(
      0, 1ul << ((32 - uint64_t(__builtin_clz(state.range(0)))) * 7));

  for (auto _ : state) {
    state.PauseTiming();
    uint64_t val = distrib(gen);
    state.ResumeTiming();

    for (int i = 0; i < 1000; ++i) {
      writer.bmMsgBuf_.clear();
      writer.valueInt(val);
    }
  }
}

// last iteration should fall off a 'cliff' because we switch to the general
// formula
BENCHMARK(BM_valueInt)->RangeMultiplier(2)->Range(1ul<<0, 1ul<<7);

BENCHMARK_MAIN();
