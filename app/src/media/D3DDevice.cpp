// SPDX-License-Identifier: GPL-3.0-or-later
// Device creation approach from moonlight-xbox (GPL-3.0), Common/DeviceResources.cpp.
#include "pch.h"
#include "D3DDevice.h"

#include <cstdio>

namespace XboxCast { namespace Media {

const char* HrName(HRESULT hr)
{
    static thread_local char buf[64];
    const char* n = "";
    switch (hr) {
    case S_OK: n = "S_OK"; break;
    case E_FAIL: n = "E_FAIL"; break;
    case E_INVALIDARG: n = "E_INVALIDARG"; break;
    case E_OUTOFMEMORY: n = "E_OUTOFMEMORY"; break;
    case E_NOTIMPL: n = "E_NOTIMPL"; break;
    case DXGI_ERROR_DEVICE_REMOVED: n = "DXGI_ERROR_DEVICE_REMOVED"; break;
    case DXGI_ERROR_DEVICE_RESET: n = "DXGI_ERROR_DEVICE_RESET"; break;
    case DXGI_ERROR_DEVICE_HUNG: n = "DXGI_ERROR_DEVICE_HUNG"; break;
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR: n = "DXGI_ERROR_DRIVER_INTERNAL_ERROR"; break;
    case DXGI_ERROR_INVALID_CALL: n = "DXGI_ERROR_INVALID_CALL"; break;
    case DXGI_ERROR_WAS_STILL_DRAWING: n = "DXGI_ERROR_WAS_STILL_DRAWING"; break;
    case DXGI_ERROR_UNSUPPORTED: n = "DXGI_ERROR_UNSUPPORTED"; break;
    default: break;
    }
    std::snprintf(buf, sizeof buf, "0x%08X %s", (unsigned)hr, n);
    return buf;
}

std::shared_ptr<D3DDevice> D3DDevice::Create(uint32_t generation)
{
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                               D3D_FEATURE_LEVEL_10_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    winrt::com_ptr<ID3D11Device> dev;
    winrt::com_ptr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_9_1;
    bool warp = false;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, dev.put(), &got, ctx.put());
    if (FAILED(hr)) {
        MLOGE("render", "D3D11CreateDevice(HARDWARE, VIDEO_SUPPORT|BGRA) failed: %s, trying without VIDEO_SUPPORT",
              HrName(hr));
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                               ARRAYSIZE(levels), D3D11_SDK_VERSION, dev.put(), &got, ctx.put());
    }
    if (FAILED(hr)) {
        MLOGE("render", "hardware D3D11 device failed: %s, falling back to WARP (software rendering)", HrName(hr));
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                               ARRAYSIZE(levels), D3D11_SDK_VERSION, dev.put(), &got, ctx.put());
        warp = true;
    }
    if (FAILED(hr)) {
        MLOGE("render", "no D3D11 device at all: %s", HrName(hr));
        return nullptr;
    }
    std::shared_ptr<D3DDevice> d(new D3DDevice());
    d->m_device = dev.as<ID3D11Device1>();
    d->m_context = ctx.as<ID3D11DeviceContext1>();
    d->m_generation = generation;
    d->m_warp = warp;
    if (auto mt = dev.try_as<ID3D10Multithread>()) mt->SetMultithreadProtected(TRUE);

    char adapter[128] = "?";
    if (auto dxgiDev = dev.try_as<IDXGIDevice>()) {
        winrt::com_ptr<IDXGIAdapter> a;
        DXGI_ADAPTER_DESC ad = {};
        if (SUCCEEDED(dxgiDev->GetAdapter(a.put())) && SUCCEEDED(a->GetDesc(&ad))) {
            std::snprintf(adapter, sizeof adapter, "%ls (vendor 0x%04x device 0x%04x, %u MB VRAM)", ad.Description,
                          ad.VendorId, ad.DeviceId, (unsigned)(ad.DedicatedVideoMemory >> 20));
        }
        dxgiDev->SetGPUThreadPriority(7);
    }
    bool videoDev = d->m_device.try_as<ID3D11VideoDevice>() != nullptr;
    char desc[320];
    std::snprintf(desc, sizeof desc, "%s FL %x.%x, %s, video device %s, gen %u", warp ? "WARP" : "hardware",
                  (got >> 12) & 0xf, (got >> 8) & 0xf, adapter, videoDev ? "yes" : "NO (no hw decode)", generation);
    d->m_desc = desc;
    MLOGI("render", "D3D11 device: %s", desc);
    return d;
}

D3DDevice::~D3DDevice()
{
    if (m_context) {
        std::lock_guard<std::recursive_mutex> lk(m_mutex);
        m_context->ClearState();
        m_context->Flush();
    }
}

void D3DDevice::LockCb(void* ctx) { static_cast<D3DDevice*>(ctx)->m_mutex.lock(); }
void D3DDevice::UnlockCb(void* ctx) { static_cast<D3DDevice*>(ctx)->m_mutex.unlock(); }

AVBufferRef* D3DDevice::NewHwDevice()
{
    if (!m_device.try_as<ID3D11VideoDevice>()) {
        MLOGW("video", "device has no ID3D11VideoDevice: hardware decode unavailable");
        return nullptr;
    }
    AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!ref) return nullptr;
    auto* hw = reinterpret_cast<AVHWDeviceContext*>(ref->data);
    auto* d3d = reinterpret_cast<AVD3D11VADeviceContext*>(hw->hwctx);
    // FFmpeg takes ownership of one reference of each interface (it Releases them in its free callback).
    m_device->AddRef();
    m_context->AddRef();
    d3d->device = m_device.get();
    d3d->device_context = m_context.get();
    d3d->lock = &D3DDevice::LockCb;
    d3d->unlock = &D3DDevice::UnlockCb;
    d3d->lock_ctx = this;
    int err = av_hwdevice_ctx_init(ref);
    if (err < 0) {
        char e[128];
        av_strerror(err, e, sizeof e);
        MLOGE("video", "av_hwdevice_ctx_init(D3D11VA) failed: %s", e);
        av_buffer_unref(&ref);
        return nullptr;
    }
    return ref;
}

void D3DDevice::MarkLost(const char* where, HRESULT hr)
{
    if (m_lost.exchange(true)) return;
    HRESULT reason = m_device ? m_device->GetDeviceRemovedReason() : E_FAIL;
    MLOGE("render", "D3D device LOST in %s: %s (removed reason %s), gen %u will be recreated", where, HrName(hr),
          HrName(reason), m_generation);
}

bool D3DDevice::CheckLost(const char* where)
{
    if (m_lost.load()) return true;
    HRESULT reason = m_device->GetDeviceRemovedReason();
    if (FAILED(reason)) MarkLost(where, reason);
    return m_lost.load();
}

}} // namespace XboxCast::Media
