#pragma once

#include <string>
#include <vector>

namespace cue {

// "0-3,8,10-11" -> {0,1,2,3,8,10,11}
std::vector<int> parseCpuList(const std::string& text);

// Intel hybrid CPUs list their core types in sysfs. On other CPUs both
// lists are empty.
std::vector<int> performanceCpus();
std::vector<int> efficiencyCpus();

// First hardware thread of each physical core in cpus (one per core).
std::vector<int> primaryThreads(const std::vector<int>& cpus);

// The CPU the audio thread should run on: the last performance core's
// first thread, keeping away from CPU 0, which serves most interrupts.
int pickAudioCpu();

std::string readFileTrimmed(const std::string& path);

} // namespace cue
