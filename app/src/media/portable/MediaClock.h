// xbox-cast media: the one clock every media timestamp uses.
// All pts from castcore are on xc_now_ns(); when castcore is linked (XC_HAVE_CASTCORE) we call it so the
// latency/A-V math compares like with like. Without castcore (Linux harness, idle-only app builds) we use
// std::chrono::steady_clock (QPC on MSVC, CLOCK_MONOTONIC on Linux), which is what castcore uses too.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>

#include "XcTypes.h"

namespace XboxCast { namespace Media {

#if defined(XC_HAVE_CASTCORE) && defined(XBOXCAST_MEDIA_REAL_CASTCORE)
inline int64_t NowNs() { return xc_now_ns(); }
#else
inline int64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
#endif

inline double NsToMs(int64_t ns) { return (double)ns / 1.0e6; }

}} // namespace XboxCast::Media
