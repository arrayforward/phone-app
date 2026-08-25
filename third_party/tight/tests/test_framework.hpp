#pragma once

// Minimal header-only test framework used by the tight ctest suite.

#include <cstdio>
#include <exception>
#include <functional>
#include <string>
#include <vector>

namespace ttest {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

inline int g_failures = 0;

struct Registrar {
    Registrar(const std::string& name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

inline void fail(const char* file, int line, const std::string& msg) {
    ++g_failures;
    std::printf("  FAIL %s:%d: %s\n", file, line, msg.c_str());
}

inline int run_all() {
    int pass = 0;
    int fail = 0;
    for (auto& t : registry()) {
        int before = g_failures;
        try {
            t.fn();
        } catch (const std::exception& e) {
            ++g_failures;
            std::printf("  FAIL %s: unexpected exception: %s\n",
                        t.name.c_str(), e.what());
        } catch (...) {
            ++g_failures;
            std::printf("  FAIL %s: unexpected non-std exception\n",
                        t.name.c_str());
        }
        if (g_failures == before) {
            ++pass;
            std::printf("PASS %s\n", t.name.c_str());
        } else {
            ++fail;
            std::printf("FAIL %s\n", t.name.c_str());
        }
    }
    std::printf("\n%d/%d tests passed\n", pass, pass + fail);
    return fail == 0 ? 0 : 1;
}

} // namespace ttest

#define TEST_CASE(name)                                                        \
    static void name();                                                        \
    static ::ttest::Registrar ttest_registrar_##name(#name, name);             \
    static void name()

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) ::ttest::fail(__FILE__, __LINE__, "CHECK(" #cond ")");    \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        auto ttest_a = (a);                                                    \
        auto ttest_b = (b);                                                    \
        if (!(ttest_a == ttest_b)) {                                           \
            ::ttest::fail(__FILE__, __LINE__, "CHECK_EQ(" #a ", " #b ")");     \
        }                                                                      \
    } while (0)

#define CHECK_NE(a, b)                                                         \
    do {                                                                       \
        auto ttest_a = (a);                                                    \
        auto ttest_b = (b);                                                    \
        if (!(ttest_a != ttest_b)) {                                           \
            ::ttest::fail(__FILE__, __LINE__, "CHECK_NE(" #a ", " #b ")");     \
        }                                                                      \
    } while (0)

#define CHECK_GT(a, b)                                                         \
    do {                                                                       \
        auto ttest_a = (a);                                                    \
        auto ttest_b = (b);                                                    \
        if (!(ttest_a > ttest_b)) {                                            \
            ::ttest::fail(__FILE__, __LINE__, "CHECK_GT(" #a ", " #b ")");     \
        }                                                                      \
    } while (0)

#define CHECK_GE(a, b)                                                         \
    do {                                                                       \
        auto ttest_a = (a);                                                    \
        auto ttest_b = (b);                                                    \
        if (!(ttest_a >= ttest_b)) {                                           \
            ::ttest::fail(__FILE__, __LINE__, "CHECK_GE(" #a ", " #b ")");     \
        }                                                                      \
    } while (0)

#define CHECK_LE(a, b)                                                         \
    do {                                                                       \
        auto ttest_a = (a);                                                    \
        auto ttest_b = (b);                                                    \
        if (!(ttest_a <= ttest_b)) {                                           \
            ::ttest::fail(__FILE__, __LINE__, "CHECK_LE(" #a ", " #b ")");     \
        }                                                                      \
    } while (0)
