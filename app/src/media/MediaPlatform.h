// xbox-cast media: Windows-only includes shared by the media pipeline (D3D11, DXGI, XAudio2, FFmpeg hwcontext).
// Portable code lives in portable/ and never includes this.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <xaudio2.h>
#include <windows.ui.xaml.media.dxinterop.h>
#include <winrt/base.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
}

#include "portable/MediaLog.h"

namespace XboxCast { namespace Media {

// "0x887A0005 DXGI_ERROR_DEVICE_REMOVED" style text for logs.
const char* HrName(HRESULT hr);

inline bool IsDeviceLost(HRESULT hr)
{
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

}} // namespace XboxCast::Media
