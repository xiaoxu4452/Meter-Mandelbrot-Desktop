// Shared test harness.
//
// tests/test_main.cpp owns the counters and the built-in algorithm tests.
// Port-layer and family tests are separate translation units: each registers
// itself with MSET_TEST(name) and runs after the built-ins, so adding
// coverage for a new test family never edits test_main.cpp.

#pragma once

#include <iostream>
#include <string>
#include <vector>

extern int g_checks;
extern int g_failures;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!(cond)) {                                                                    \
            ++g_failures;                                                                 \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " << #cond << "\n"; \
        }                                                                                 \
    } while (0)

namespace test_support {

using Fn = void (*)();

struct Item {
    const char* name;
    Fn fn;
};

void add(const char* name, Fn fn);
const std::vector<Item>& items();

}  // namespace test_support

#define MSET_TEST(test_name)                                                             \
    static void test_name();                                                             \
    namespace {                                                                          \
        struct TestReg_##test_name {                                                     \
            TestReg_##test_name() { ::test_support::add(#test_name, &test_name); }        \
        };                                                                               \
        static TestReg_##test_name test_reg_##test_name;                                 \
    }                                                                                    \
    static void test_name()
