// xbox-cast media: logging shim. Portable code logs through here; the app routes it to Diag::Log
// (MediaPipeline installs the sink), the Linux harness prints to stderr.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdarg>

namespace XboxCast { namespace Media {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// tag is one of the contract tags: "video", "render", "audio".
typedef void (*LogSink)(LogLevel level, const char* tag, const char* message);

void SetLogSink(LogSink sink);
void Log(LogLevel level, const char* tag, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
void LogV(LogLevel level, const char* tag, const char* fmt, va_list ap);

}} // namespace XboxCast::Media

#define MLOGD(tag, ...) ::XboxCast::Media::Log(::XboxCast::Media::LogLevel::Debug, tag, __VA_ARGS__)
#define MLOGI(tag, ...) ::XboxCast::Media::Log(::XboxCast::Media::LogLevel::Info, tag, __VA_ARGS__)
#define MLOGW(tag, ...) ::XboxCast::Media::Log(::XboxCast::Media::LogLevel::Warn, tag, __VA_ARGS__)
#define MLOGE(tag, ...) ::XboxCast::Media::Log(::XboxCast::Media::LogLevel::Error, tag, __VA_ARGS__)
