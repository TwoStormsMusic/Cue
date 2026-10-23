#pragma once

// A minimal test runner: TEST(name) { CHECK(...); } and run_all() from main.

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace check {

struct Test {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Test>& registry() {
    static std::vector<Test> tests;
    return tests;
}

inline int& failures() {
    static int count = 0;
    return count;
}

struct Register {
    Register(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline void fail(const char* file, int line, const std::string& what) {
    std::printf("    FAIL %s:%d: %s\n", file, line, what.c_str());
    ++failures();
}

inline int run_all(const char* filter = nullptr) {
    int failedTests = 0, ran = 0;
    for (auto& t : registry()) {
        if (filter && std::string(t.name).find(filter) == std::string::npos)
            continue;
        const int before = failures();
        std::printf("[ RUN  ] %s\n", t.name);
        t.fn();
        ++ran;
        if (failures() != before) {
            ++failedTests;
            std::printf("[ FAIL ] %s\n", t.name);
        } else {
            std::printf("[  OK  ] %s\n", t.name);
        }
    }
    std::printf("\n%d of %d tests passed\n", ran - failedTests, ran);
    return failedTests == 0 ? 0 : 1;
}

} // namespace check

#define CHECK_CAT2(a, b) a##b
#define CHECK_CAT(a, b) CHECK_CAT2(a, b)
#define TEST(name)                                                                  \
    static void name();                                                             \
    static check::Register CHECK_CAT(reg_, name)(#name, name);                      \
    static void name()

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond))                                                                \
            check::fail(__FILE__, __LINE__, #cond);                                 \
    } while (0)

#define CHECK_MSG(cond, ...)                                                        \
    do {                                                                            \
        if (!(cond)) {                                                              \
            char buf_[512];                                                         \
            std::snprintf(buf_, sizeof buf_, __VA_ARGS__);                          \
            check::fail(__FILE__, __LINE__, std::string(#cond) + "  (" + buf_ + ")"); \
        }                                                                           \
    } while (0)
