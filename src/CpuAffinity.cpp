#include "CpuAffinity.h"

#include "au/ParseError.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <thread>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace au {

namespace {

int parseCpuNumber(const std::string &s, const std::string &spec) {
  if (s.empty() || !std::all_of(s.begin(), s.end(), ::isdigit))
    THROW_RT("Invalid CPU list '" << spec << "': '" << s
             << "' is not a CPU number");
  try {
    return std::stoi(s);
  } catch (std::out_of_range &) {
    THROW_RT("Invalid CPU list '" << spec << "': '" << s << "' is too large");
  }
}

}

std::vector<int> parseCpuList(const std::string &spec) {
  std::vector<int> result;
  size_t start = 0;
  while (start <= spec.size()) {
    auto comma = spec.find(',', start);
    if (comma == std::string::npos) comma = spec.size();
    auto item = spec.substr(start, comma - start);
    start = comma + 1;

    int stride = 1;
    if (auto colon = item.find(':'); colon != std::string::npos) {
      stride = parseCpuNumber(item.substr(colon + 1), spec);
      if (stride < 1)
        THROW_RT("Invalid CPU list '" << spec << "': stride must be positive");
      item = item.substr(0, colon);
    }
    int lo, hi;
    if (auto dash = item.find('-'); dash != std::string::npos) {
      lo = parseCpuNumber(item.substr(0, dash), spec);
      hi = parseCpuNumber(item.substr(dash + 1), spec);
      if (hi < lo)
        THROW_RT("Invalid CPU list '" << spec << "': range " << item
                 << " is backwards");
    } else {
      if (stride != 1)
        THROW_RT("Invalid CPU list '" << spec << "': stride needs a range");
      lo = hi = parseCpuNumber(item, spec);
    }
    for (int cpu = lo; cpu <= hi; cpu += stride) {
      if (std::find(result.begin(), result.end(), cpu) == result.end())
        result.push_back(cpu);
    }
  }
  return result;
}

unsigned usableCpuCount() {
#ifdef __linux__
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) == 0) {
    auto n = CPU_COUNT(&set);
    if (n > 0) return static_cast<unsigned>(n);
  }
#endif
  return std::max(1u, std::thread::hardware_concurrency());
}

std::vector<int> allowedCpus() {
  std::vector<int> result;
#ifdef __linux__
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) == 0) {
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
      if (CPU_ISSET(cpu, &set)) result.push_back(cpu);
  }
#endif
  return result;
}

bool cpuPinningSupported() {
#ifdef __linux__
  return true;
#else
  return false;
#endif
}

void pinThisThreadToCpu(int cpu) {
#ifdef __linux__
  if (cpu < 0 || cpu >= CPU_SETSIZE)
    THROW_RT("CPU " << cpu << " is out of range");
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<size_t>(cpu), &set);
  auto err = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  if (err != 0)
    THROW_RT("Unable to pin thread to CPU " << cpu << ": " << strerror(err)
             << " (does it exist, and is it in this process's allowed set?)");
#else
  THROW_RT("Pinning threads to CPUs (CPU " << cpu
           << ") isn't supported on this platform");
#endif
}

}
