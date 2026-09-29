// SPDX-License-Identifier: MIT
// DXVK 2.6.2 PE control: resize one live swapchain from 1080p to 4K.
#include "window.h"
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>

template <typename T> static void release(T*& value) {
  if (value) value->Release();
  value = nullptr;
}

static void stage(const char* name, HRESULT hr) {
  std::printf("DXVK_PE_D3D11_RESIZE_STAGE name=%s hr=0x%08x\n", name,
              unsigned(hr));
  std::fflush(stdout);
}

static HRESULT show_phase(ID3D11Device* device, ID3D11DeviceContext* context,
                          IDXGISwapChain* swapchain, const char* phase,
                          UINT width, UINT height, const float color[4]) {
  ID3D11Texture2D* image = nullptr;
  ID3D11RenderTargetView* view = nullptr;
  HRESULT hr = swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                     reinterpret_cast<void**>(&image));
  stage("get-buffer", hr);
  if (FAILED(hr)) goto done;
  {
    D3D11_TEXTURE2D_DESC actual = {};
    image->GetDesc(&actual);
    std::printf("DXVK_PE_D3D11_RESIZE_BUFFER phase=%s width=%u height=%u\n",
                phase, actual.Width, actual.Height);
    std::fflush(stdout);
    if (actual.Width != width || actual.Height != height) {
      hr = E_FAIL;
      goto done;
    }
  }
  hr = device->CreateRenderTargetView(image, nullptr, &view);
  stage("create-view", hr);
  if (FAILED(hr)) goto done;
  for (int frame = 0; frame < 3; ++frame) {
    context->ClearRenderTargetView(view, color);
    hr = swapchain->Present(1, 0);
    std::printf("DXVK_PE_D3D11_RESIZE_PRESENT phase=%s frame=%d hr=0x%08x\n",
                phase, frame + 1, unsigned(hr));
    std::fflush(stdout);
    if (FAILED(hr)) break;
    pe_visible_frame();
  }
done:
  context->ClearState();
  context->Flush();
  release(view);
  release(image);
  return hr;
}

int main() {
  HWND window = pe_window(L"DXVK D3D11 1080p to 4K resize");
  if (!window) return 2;
  IDXGIFactory* factory = nullptr;
  IDXGIAdapter* adapter = nullptr;
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  IDXGISwapChain* swapchain = nullptr;
  HRESULT result = E_FAIL;
  D3D_FEATURE_LEVEL selected = static_cast<D3D_FEATURE_LEVEL>(0);
  const D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_11_0;
  const float first_color[4] = {1, 0, 0, 1};
  const float second_color[4] = {0, 1, 0, 1};
  int phases = 0;

  result = CreateDXGIFactory(__uuidof(IDXGIFactory),
                             reinterpret_cast<void**>(&factory));
  stage("factory", result);
  if (FAILED(result)) goto done;
  result = factory->EnumAdapters(0, &adapter);
  stage("adapter", result);
  if (FAILED(result)) goto done;
  result = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                             &requested, 1, D3D11_SDK_VERSION,
                             &device, &selected, &context);
  stage("device", result);
  if (FAILED(result) || selected != D3D_FEATURE_LEVEL_11_0) goto done;
  {
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferDesc.Width = PE_WIDTH;
    desc.BufferDesc.Height = PE_HEIGHT;
    desc.BufferDesc.RefreshRate.Numerator = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.OutputWindow = window;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    result = factory->CreateSwapChain(device, &desc, &swapchain);
    stage("create-swapchain-1080", result);
    if (FAILED(result)) goto done;
  }
  result = show_phase(device, context, swapchain, "1080p", PE_WIDTH,
                      PE_HEIGHT, first_color);
  if (FAILED(result)) goto done;
  ++phases;
  std::printf("DXVK_PE_D3D11_RESIZE_PHASE_DONE phase=1080p\n");
  std::fflush(stdout);

  if (!SetWindowPos(window, nullptr, 0, 0, 3840, 2160,
                    SWP_NOZORDER | SWP_NOACTIVATE)) {
    result = E_FAIL;
    stage("set-window-4k", result);
    goto done;
  }
  pe_visible_frame();
  result = swapchain->ResizeBuffers(2, 3840, 2160,
                                    DXGI_FORMAT_B8G8R8A8_UNORM, 0);
  stage("resize-buffers-4k", result);
  if (FAILED(result)) goto done;
  result = show_phase(device, context, swapchain, "4k", 3840, 2160,
                      second_color);
  if (FAILED(result)) goto done;
  ++phases;
  std::printf("DXVK_PE_D3D11_RESIZE_PHASE_DONE phase=4k\n");
  std::fflush(stdout);

done:
  std::printf("DXVK_PE_D3D11_RESIZE_RESULT hr=0x%08x level=0x%04x phases=%d pass=%u\n",
              unsigned(result), unsigned(selected), phases,
              unsigned(SUCCEEDED(result) && phases == 2));
  std::fflush(stdout);
  release(swapchain);
  release(context);
  release(device);
  release(adapter);
  release(factory);
  DestroyWindow(window);
  return SUCCEEDED(result) && phases == 2 ? 0 : 1;
}
