/* D3D11 FL11_0 workload: either a 64x64 offscreen target or a 1920x1080
 * DXGI swapchain. Draw a 48x64 pattern, read back the top-left 64x64 region
 * and compare every sampled pixel. Presentation runs three frames with
 * distinct clear markers and requires Present to return S_OK each time.
 * The oracle checks backbuffer contents before Present, not external scanout.
 * Everything is released before returning. */
#include "workload.h"
#include "dxvk_shaders.h"

#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdio>
#include <cstring>

namespace {

class Stages {
public:
    Stages(const DxvkNativeHooks &hooks, DxvkNativeSummary *summary)
        : m_hooks(hooks), m_summary(summary) { }

    void begin(const char *stage) { mark(stage, "begin", ""); }
    void ok(const char *stage, const char *detail = "") { mark(stage, "ok", detail); }
    bool check(const char *stage, HRESULT hr) {
        char detail[64];
        std::snprintf(detail, sizeof(detail), "hr=0x%08x", static_cast<unsigned>(hr));
        mark(stage, SUCCEEDED(hr) ? "ok" : "fail", detail);
        return SUCCEEDED(hr);
    }
    void fail(const char *stage, const char *detail) { mark(stage, "fail", detail); }

private:
    void mark(const char *stage, const char *state, const char *detail) {
        if (std::strcmp(stage, "shutdown"))
            std::snprintf(m_summary->last_stage, sizeof(m_summary->last_stage), "%s", stage);
        if (m_hooks.stage) m_hooks.stage(stage, state, detail ? detail : "");
    }

    const DxvkNativeHooks &m_hooks;
    DxvkNativeSummary *m_summary;
};

template<typename T>
void release(T *&object) {
    if (object) object->Release();
    object = nullptr;
}

} // namespace

int dxvk_native_run_workload(const DxvkNativeHooks &hooks, DxvkNativeSummary *summary,
                             void *presentation_window)
{
    std::memset(summary, 0, sizeof(*summary));
    summary->outcome = DXVK_NATIVE_REFUSED;
    Stages stages(hooks, summary);

    IDXGISwapChain1 *swapchain = nullptr;
    IDXGIDevice *dxgi_device = nullptr;
    IDXGIAdapter *adapter = nullptr;
    IDXGIFactory2 *factory = nullptr;
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    ID3D11Texture2D *target = nullptr, *staging = nullptr;
    ID3D11RenderTargetView *view = nullptr;
    ID3D11VertexShader *vs = nullptr;
    ID3D11PixelShader *ps = nullptr;
    bool mapped = false;

    D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_11_0;
    D3D_FEATURE_LEVEL selected = static_cast<D3D_FEATURE_LEVEL>(0);
    stages.begin("d3d11.device");
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                   &requested, 1, D3D11_SDK_VERSION,
                                   &device, &selected, &context);
    summary->create_hresult = static_cast<uint32_t>(hr);
    summary->feature_level = static_cast<uint32_t>(selected);
    {
        char detail[96];
        std::snprintf(detail, sizeof(detail), "hr=0x%08x feature_level=0x%04x device=%d context=%d",
                      static_cast<unsigned>(hr), static_cast<unsigned>(selected),
                      device != nullptr, context != nullptr);
        if (FAILED(hr) || !device || selected != D3D_FEATURE_LEVEL_11_0) {
            stages.fail("d3d11.device", detail);
            goto cleanup;
        }
        stages.ok("d3d11.device", detail);
    }
    stages.begin("d3d11.context");
    if (!context) { stages.fail("d3d11.context", "context=null"); goto cleanup; }
    stages.ok("d3d11.context", "immediate=1");

    {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = DXVK_ORACLE_WIDTH;
        desc.Height = DXVK_ORACLE_HEIGHT;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (presentation_window) {
            stages.begin("dxgi.device");
            if (!stages.check("dxgi.device", device->QueryInterface(
                    __uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_device)))) goto cleanup;
            stages.begin("dxgi.adapter");
            if (!stages.check("dxgi.adapter", dxgi_device->GetAdapter(&adapter))) goto cleanup;
            stages.begin("dxgi.factory");
            if (!stages.check("dxgi.factory", adapter->GetParent(
                    __uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory)))) goto cleanup;
            DXGI_SWAP_CHAIN_DESC1 sc = {};
            sc.Width = DXVK_PRESENT_WIDTH;
            sc.Height = DXVK_PRESENT_HEIGHT;
            sc.Format = desc.Format;
            sc.SampleDesc.Count = 1;
            sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sc.BufferCount = 2;
            sc.Scaling = DXGI_SCALING_STRETCH;
            sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            sc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            stages.begin("dxgi.swapchain");
            if (!stages.check("dxgi.swapchain", factory->CreateSwapChainForHwnd(
                    device, static_cast<HWND>(presentation_window), &sc,
                    nullptr, nullptr, &swapchain))) goto cleanup;
            stages.begin("dxgi.backbuffer");
            if (!stages.check("dxgi.backbuffer", swapchain->GetBuffer(
                    0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&target)))) goto cleanup;
        } else {
            stages.begin("resource.render_target");
            if (!stages.check("resource.render_target", device->CreateTexture2D(&desc, nullptr, &target)))
                goto cleanup;
        }
        stages.begin("resource.render_target_view");
        if (!stages.check("resource.render_target_view",
                          device->CreateRenderTargetView(target, nullptr, &view)))
            goto cleanup;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stages.begin("resource.staging");
        if (!stages.check("resource.staging", device->CreateTexture2D(&desc, nullptr, &staging)))
            goto cleanup;
    }

    stages.begin("shader.vertex");
    if (!stages.check("shader.vertex", device->CreateVertexShader(
            dxvk_native_vs_dxbc, sizeof(dxvk_native_vs_dxbc), nullptr, &vs)))
        goto cleanup;
    stages.begin("shader.pixel");
    if (!stages.check("shader.pixel", device->CreatePixelShader(
            dxvk_native_ps_dxbc, sizeof(dxvk_native_ps_dxbc), nullptr, &ps)))
        goto cleanup;

    for (uint32_t frame = 0; frame < (presentation_window ? DXVK_PRESENT_FRAMES : 1u); ++frame) {
        stages.begin("clear");
        const float clear[4] = {(DXVK_ORACLE_CLEAR_R + 16u * frame) / 255.0f, DXVK_ORACLE_CLEAR_G / 255.0f,
                                DXVK_ORACLE_CLEAR_B / 255.0f, DXVK_ORACLE_CLEAR_A / 255.0f};
        context->OMSetRenderTargets(1, &view, nullptr);
        context->ClearRenderTargetView(view, clear);
        stages.ok("clear");

        stages.begin("draw");
        D3D11_VIEWPORT viewport = {};
        viewport.Width = static_cast<float>(DXVK_ORACLE_VIEWPORT_WIDTH);
        viewport.Height = static_cast<float>(DXVK_ORACLE_HEIGHT);
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs, nullptr, 0);
        context->PSSetShader(ps, nullptr, 0);
        context->Draw(3, 0);
        stages.ok("draw", "vertices=3 viewport=48x64");

        stages.begin("copy");
        if (presentation_window) {
            const D3D11_BOX region = {0, 0, 0, DXVK_ORACLE_WIDTH, DXVK_ORACLE_HEIGHT, 1};
            context->CopySubresourceRegion(staging, 0, 0, 0, 0, target, 0, &region);
        } else {
            context->CopyResource(staging, target);
        }
        stages.ok("copy");

        stages.begin("map");
        D3D11_MAPPED_SUBRESOURCE map = {};
        if (!stages.check("map", context->Map(staging, 0, D3D11_MAP_READ, 0, &map)))
            goto cleanup;
        mapped = true;

        stages.begin("oracle");
        dxvk_oracle_result oracle;
        int verdict = dxvk_oracle_check_frame(static_cast<const uint8_t *>(map.pData),
                                               map.RowPitch, frame, &oracle);
        context->Unmap(staging, 0);
        mapped = false;
        if (!presentation_window && hooks.oracle) hooks.oracle(&oracle);
        char detail[96];
        std::snprintf(detail, sizeof(detail), "mismatches=%u checksum=%08x row_pitch=%u",
                      oracle.mismatches, oracle.checksum, static_cast<unsigned>(map.RowPitch));
        if (verdict == 0) {
            stages.ok("oracle", detail);
        } else {
            stages.fail("oracle", detail);
            summary->outcome = DXVK_NATIVE_ORACLE_MISMATCH;
            if (presentation_window && hooks.frame) hooks.frame(frame, &oracle, 0x80004005u);
            goto cleanup;
        }
        if (presentation_window) {
            stages.begin("dxgi.present");
            hr = swapchain->Present(1, 0);
            if (hooks.frame) hooks.frame(frame, &oracle, static_cast<uint32_t>(hr));
            // DXGI_STATUS_OCCLUDED is a success HRESULT, but no displayed frame.
            if (hr != S_OK) { stages.fail("dxgi.present", "Present did not return S_OK"); goto cleanup; }
            stages.ok("dxgi.present");
            ++summary->presented_frames;
        }
    }
    summary->outcome = DXVK_NATIVE_RENDERED;

cleanup:
    stages.begin("shutdown");
    if (mapped && context) context->Unmap(staging, 0);
    if (context) {
        context->ClearState();
        context->Flush();
    }
    release(ps);
    release(vs);
    release(view);
    release(staging);
    release(target);
    if (swapchain) summary->swapchain_refs_at_release = swapchain->Release();
    release(factory);
    release(adapter);
    release(dxgi_device);
    if (context) summary->context_refs_at_release = context->Release();
    context = nullptr;
    if (device) summary->device_refs_at_release = device->Release();
    device = nullptr;
    char detail[64];
    std::snprintf(detail, sizeof(detail), "device_refs=%u context_refs=%u",
                  summary->device_refs_at_release, summary->context_refs_at_release);
    stages.ok("shutdown", detail);
    return summary->outcome;
}
