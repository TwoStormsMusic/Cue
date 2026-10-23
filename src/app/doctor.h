#pragma once

#include <string>

namespace sh {

struct DoctorOptions {
    int rtPriority = 80;
    std::string deviceMatch = "M2"; // looked for in /proc/asound/cards
};

// Prints a report; returns non-zero if any check failed.
int runDoctor(const DoctorOptions& options);

} // namespace sh
