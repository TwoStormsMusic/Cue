#include "cpu.h"

#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace sh {

std::string readFileTrimmed(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\r'))
        s.pop_back();
    return s;
}

std::vector<int> parseCpuList(const std::string& text) {
    std::vector<int> out;
    std::stringstream ss(text);
    std::string part;
    while (std::getline(ss, part, ',')) {
        if (part.empty())
            continue;
        const auto dash = part.find('-');
        try {
            if (dash == std::string::npos) {
                out.push_back(std::stoi(part));
            } else {
                const int a = std::stoi(part.substr(0, dash)), b = std::stoi(part.substr(dash + 1));
                for (int c = a; c <= b; ++c)
                    out.push_back(c);
            }
        } catch (...) {
        }
    }
    return out;
}

std::vector<int> performanceCpus() { return parseCpuList(readFileTrimmed("/sys/devices/cpu_core/cpus")); }
std::vector<int> efficiencyCpus() { return parseCpuList(readFileTrimmed("/sys/devices/cpu_atom/cpus")); }

std::vector<int> primaryThreads(const std::vector<int>& cpus) {
    std::set<int> seenFirst;
    std::vector<int> out;
    for (int c : cpus) {
        const auto siblings = parseCpuList(readFileTrimmed("/sys/devices/system/cpu/cpu" + std::to_string(c) + "/topology/thread_siblings_list"));
        const int first = siblings.empty() ? c : *std::min_element(siblings.begin(), siblings.end());
        if (seenFirst.insert(first).second)
            out.push_back(first);
    }
    return out;
}

int pickAudioCpu() {
    std::vector<int> cpus = performanceCpus();
    if (cpus.empty()) {
        const long n = sysconf(_SC_NPROCESSORS_ONLN);
        for (int c = 0; c < n; ++c)
            cpus.push_back(c);
    }
    std::vector<int> cores = primaryThreads(cpus);
    cores.erase(std::remove(cores.begin(), cores.end(), 0), cores.end());
    if (cores.empty())
        return -1;
    return cores.back();
}

} // namespace sh
