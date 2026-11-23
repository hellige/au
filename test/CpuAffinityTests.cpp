#include "CpuAffinity.h"
#include "ZstdSeekable.h"

#include "gtest/gtest.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

#ifdef __linux__
#include <sched.h>
#endif

namespace au {

using V = std::vector<int>;

TEST(CpuAffinity, ParseCpuList) {
  EXPECT_EQ(parseCpuList("0"), V({0}));
  EXPECT_EQ(parseCpuList("3,1,2"), V({3, 1, 2})); // order kept
  EXPECT_EQ(parseCpuList("0-3"), V({0, 1, 2, 3}));
  EXPECT_EQ(parseCpuList("2,3,40-42"), V({2, 3, 40, 41, 42}));
  EXPECT_EQ(parseCpuList("0-7:2"), V({0, 2, 4, 6}));
  EXPECT_EQ(parseCpuList("1-8:3"), V({1, 4, 7}));
  EXPECT_EQ(parseCpuList("1,1,0-2"), V({1, 0, 2})); // duplicates dropped
}

TEST(CpuAffinity, ParseCpuListErrors) {
  for (auto bad : {"", ",", "1,", "a", "1-", "-1", "3-1", "1:2", "0-4:0",
                   "0-4:x", "1.5", "99999999999"}) {
    SCOPED_TRACE(bad);
    EXPECT_THROW(parseCpuList(bad), std::runtime_error);
  }
}

TEST(CpuAffinity, UsableCpuCount) {
  EXPECT_GE(usableCpuCount(), 1u);
  EXPECT_LE(usableCpuCount(), std::max(1u, std::thread::hardware_concurrency()));
#ifdef __linux__
  EXPECT_EQ(usableCpuCount(), allowedCpus().size());
#endif
}

#ifdef __linux__
TEST(CpuAffinity, PinThisThread) {
  auto allowed = allowedCpus();
  ASSERT_FALSE(allowed.empty());
  auto target = allowed.back();
  std::thread t([&] {
    pinThisThreadToCpu(target);
    cpu_set_t set;
    CPU_ZERO(&set);
    ASSERT_EQ(sched_getaffinity(0, sizeof(set), &set), 0);
    EXPECT_EQ(CPU_COUNT(&set), 1);
    EXPECT_TRUE(CPU_ISSET(static_cast<size_t>(target), &set));
  });
  t.join();
  // the main thread is unaffected
  EXPECT_EQ(allowedCpus(), allowed);
  EXPECT_THROW(pinThisThreadToCpu(CPU_SETSIZE + 1), std::runtime_error);
}

TEST(CpuAffinity, CompressWithPinnedThreads) {
  namespace fs = std::filesystem;
  auto dir = fs::temp_directory_path() / "au-cpu-affinity-test";
  fs::create_directories(dir);
  std::string data;
  for (int i = 0; i < 200000; i++) data += "line " + std::to_string(i) + "\n";
  { std::ofstream(dir / "in.txt") << data; }

  ZstdCompressOptions plain;
  plain.frameSize = 64 * 1024;
  zstdCompressFile(dir / "in.txt", dir / "plain.zst", plain);

  // pinned to (up to) three allowed CPUs, gives identical output
  auto allowed = allowedCpus();
  ZstdCompressOptions pinned = plain;
  pinned.cpus.assign(allowed.begin(),
                     allowed.begin() + std::min<long>(3, std::ssize(allowed)));
  pinned.threads = static_cast<unsigned>(pinned.cpus.size());
  zstdCompressFile(dir / "in.txt", dir / "pinned.zst", pinned);
  auto read = [](const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
  };
  EXPECT_EQ(read(dir / "plain.zst"), read(dir / "pinned.zst"));

  // a failed pin surfaces as an error rather than hanging or being ignored
  ZstdCompressOptions bad = plain;
  bad.cpus = {CPU_SETSIZE + 1};
  EXPECT_THROW(zstdCompressFile(dir / "in.txt", dir / "bad.zst", bad),
               std::runtime_error);

  fs::remove_all(dir);
}
#endif

}
