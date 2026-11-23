#pragma once

#include <string>
#include <vector>

namespace au {

/// Parses a CPU list in the same syntax as `taskset -c` and the kernel's
/// cpulist files: comma-separated CPU numbers and inclusive ranges, with an
/// optional stride, e.g. "0,2,4-7" or "0-15:2". Throws on malformed input.
/// The result is in the order given, without duplicates.
std::vector<int> parseCpuList(const std::string &spec);

/// The number of CPUs this process is allowed to run on (respecting
/// taskset/cpusets/etc., where the platform supports it), or the number of
/// online CPUs otherwise. Always at least 1.
unsigned usableCpuCount();

/// The CPUs this process is currently allowed to run on (e.g., as set by
/// taskset), in increasing order. Empty if the platform can't tell us.
std::vector<int> allowedCpus();

/// Whether pinThisThreadToCpu() is supported on this platform.
bool cpuPinningSupported();

/// Pins the calling thread to a single CPU. Throws on failure, e.g. if the
/// CPU doesn't exist or isn't in the process's allowed set.
void pinThisThreadToCpu(int cpu);

}
