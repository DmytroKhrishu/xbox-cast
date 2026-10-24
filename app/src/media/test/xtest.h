// xbox-cast media harness: minimal test framework (no dependencies).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace xt {

struct Test {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Test>& Registry()
{
    static std::vector<Test> r;
    return r;
}

inline int& Failures()
{
    static int f = 0;
    return f;
}

inline std::string& FixtureDir()
{
    static std::string d;
    return d;
}

struct Reg {
    Reg(const char* n, std::function<void()> f) { Registry().push_back(Test{n, f}); }
};

struct Skip {
    std::string why;
};

} // namespace xt

#define XT_CAT2(a, b) a##b
#define XT_CAT(a, b) XT_CAT2(a, b)
#define TEST(name)                                                   \
    static void name();                                              \
    static xt::Reg XT_CAT(reg_, name)(#name, name);                  \
    static void name()

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
            xt::Failures()++;                                                             \
        }                                                                                 \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                              \
    do {                                                                                                   \
        double xa_ = (double)(a), xb_ = (double)(b);                                                       \
        if (!(std::fabs(xa_ - xb_) <= (eps))) {                                                            \
            std::printf("    FAIL %s:%d: %s = %.5f, expected %.5f +- %g\n", __FILE__, __LINE__, #a, xa_, xb_, \
                        (double)(eps));                                                                    \
            xt::Failures()++;                                                                              \
        }                                                                                                  \
    } while (0)

#define SKIP(why) throw xt::Skip{why}

#define INFO(...)                    \
    do {                             \
        std::printf("    ");         \
        std::printf(__VA_ARGS__);    \
        std::printf("\n");           \
    } while (0)
