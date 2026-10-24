// xbox-cast media: castcore frame/packet types (contract section 1).
//   1. real castcore.h when it is on the include path (native/ built);
//   2. app build without castcore: core/ReceiverHost.h carries the contract's fallback copy, and MediaPipeline.h
//      includes ReceiverHost.h too, so media must use that same copy (one definition per translation unit);
//   3. Linux harness: test/castcore_types_fallback.h (the harness puts test/ on the include path).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#if __has_include(<castcore.h>)
extern "C" {
#include <castcore.h>
}
#define XBOXCAST_MEDIA_REAL_CASTCORE 1
#elif __has_include("core/ReceiverHost.h")
#include "core/ReceiverHost.h"
#else
#include "castcore_types_fallback.h"
#endif
