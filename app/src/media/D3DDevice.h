// xbox-cast media: the one D3D11 device shared by the FFmpeg D3D11VA decoder and the swap-chain renderer.
//
// Created with VIDEO_SUPPORT | BGRA_SUPPORT (hardware, WARP as a last resort). The immediate context is used from the
// decode thread (FFmpeg's D3D11VA calls, through the lock callbacks below) and the render thread, so both serialize on
// one recursive mutex, and ID3D10Multithread protection is on as a second guard.
// Device loss: whoever sees DXGI_ERROR_DEVICE_REMOVED marks the device lost; MediaPipeline creates a new D3DDevice
// (new generation) and the decoder/renderer rebuild everything on their next iteration.
//
// Approach from moonlight-xbox (GPL-3.0, github.com/TheElixZammuto/moonlight-xbox): Common/DeviceResources.cpp
// (device creation) and Streaming/FFmpegDecoder.cpp (AVD3D11VADeviceContext on the app's own device + lock callbacks).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "MediaPlatform.h"

namespace XboxCast { namespace Media {

class D3DDevice {
public:
    // Returns nullptr when no device could be created at all (logged).
    static std::shared_ptr<D3DDevice> Create(uint32_t generation);
    ~D3DDevice();

    ID3D11Device1* Device() const { return m_device.get(); }
    ID3D11DeviceContext1* Context() const { return m_context.get(); }
    std::recursive_mutex& Mutex() { return m_mutex; }
    uint32_t Generation() const { return m_generation; }
    bool IsWarp() const { return m_warp; }
    const std::string& Description() const { return m_desc; }

    // New AV_HWDEVICE_TYPE_D3D11VA device context wrapping this device (caller owns the ref). nullptr on failure.
    AVBufferRef* NewHwDevice();

    // Lost-device bookkeeping (any thread).
    void MarkLost(const char* where, HRESULT hr);
    bool Lost() const { return m_lost.load(); }
    // Checks GetDeviceRemovedReason (cheap) and marks lost if removed.
    bool CheckLost(const char* where);

private:
    D3DDevice() = default;
    static void LockCb(void* ctx);
    static void UnlockCb(void* ctx);

    winrt::com_ptr<ID3D11Device1> m_device;
    winrt::com_ptr<ID3D11DeviceContext1> m_context;
    std::recursive_mutex m_mutex;
    std::atomic<bool> m_lost{false};
    uint32_t m_generation = 0;
    bool m_warp = false;
    std::string m_desc;
};

}} // namespace XboxCast::Media
