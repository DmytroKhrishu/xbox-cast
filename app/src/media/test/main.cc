// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "MediaLog.h"
#include "xtest.h"

static bool g_verbose = false;

static void QuietSink(XboxCast::Media::LogLevel level, const char* tag, const char* msg)
{
    if (g_verbose || level >= XboxCast::Media::LogLevel::Warn) std::printf("      [%s] %s\n", tag, msg);
}

int main(int argc, char** argv)
{
    const char* filter = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "-v")) g_verbose = true;
        else if (!std::strcmp(argv[i], "--fixtures") && i + 1 < argc) xt::FixtureDir() = argv[++i];
        else filter = argv[i];
    }
    if (xt::FixtureDir().empty()) {
        const char* e = std::getenv("XC_FIXTURES");
        if (e) xt::FixtureDir() = e;
    }
#ifdef XC_DEFAULT_FIXTURES
    if (xt::FixtureDir().empty()) xt::FixtureDir() = XC_DEFAULT_FIXTURES;
#endif
    XboxCast::Media::SetLogSink(&QuietSink);
    int run = 0, skipped = 0, failedTests = 0;
    for (auto& t : xt::Registry()) {
        if (filter && !std::strstr(t.name, filter)) continue;
        std::printf("[ RUN  ] %s\n", t.name);
        int before = xt::Failures();
        try {
            t.fn();
        } catch (const xt::Skip& s) {
            std::printf("[ SKIP ] %s: %s\n", t.name, s.why.c_str());
            skipped++;
            continue;
        }
        run++;
        if (xt::Failures() != before) {
            failedTests++;
            std::printf("[ FAIL ] %s\n", t.name);
        } else {
            std::printf("[  OK  ] %s\n", t.name);
        }
    }
    std::printf("\n%d tests run, %d failed, %d skipped, %d checks failed\n", run, failedTests, skipped, xt::Failures());
    return failedTests ? 1 : 0;
}
