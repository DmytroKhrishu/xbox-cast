// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "MediaLog.h"

#include <atomic>
#include <cstdio>

namespace XboxCast { namespace Media {

namespace {
std::atomic<LogSink> g_sink{nullptr};

void DefaultSink(LogLevel level, const char* tag, const char* message)
{
    static const char kLetters[] = {'D', 'I', 'W', 'E'};
    std::fprintf(stderr, "%c %s %s\n", kLetters[(int)level & 3], tag, message);
}
} // namespace

void SetLogSink(LogSink sink) { g_sink.store(sink); }

void LogV(LogLevel level, const char* tag, const char* fmt, va_list ap)
{
    char buf[1024];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    LogSink sink = g_sink.load();
    (sink ? sink : DefaultSink)(level, tag, buf);
}

void Log(LogLevel level, const char* tag, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    LogV(level, tag, fmt, ap);
    va_end(ap);
}

}} // namespace XboxCast::Media
