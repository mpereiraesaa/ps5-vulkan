// SPDX-License-Identifier: MIT
// PE application control: Wine d3d10.dll wrapper -> DXVK D3D10 core.
#include "window.h"
#include <d3d10.h>
#include <dxgi.h>
#include <cstdio>

int main() {
  HWND window = pe_window(L"DXVK 2.6.2 D3D10 / Prospero Win");
  if (!window) return 2;
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
  ID3D10Device* device = nullptr;
  IDXGISwapChain* swapchain = nullptr;
  ID3D10Texture2D* backbuffer = nullptr;
  ID3D10RenderTargetView* view = nullptr;
  HRESULT create = D3D10CreateDeviceAndSwapChain(nullptr, D3D10_DRIVER_TYPE_HARDWARE,
      nullptr, 0, D3D10_SDK_VERSION, &desc, &swapchain, &device);
  HRESULT buffer = SUCCEEDED(create) && swapchain ? swapchain->GetBuffer(0,
      __uuidof(ID3D10Texture2D), reinterpret_cast<void**>(&backbuffer)) : create;
  HRESULT target = SUCCEEDED(buffer) && device ? device->CreateRenderTargetView(
      backbuffer, nullptr, &view) : buffer;
  HRESULT present[2] = {target, target};
  if (SUCCEEDED(target) && view) {
    const float colors[2][4] = {{28.0f / 255.0f, 76.0f / 255.0f,
                                 132.0f / 255.0f, 1.0f},
                                {132.0f / 255.0f, 76.0f / 255.0f,
                                 28.0f / 255.0f, 1.0f}};
    for (int frame = 0; frame < 2; ++frame) {
      device->ClearRenderTargetView(view, colors[frame]);
      present[frame] = swapchain->Present(1, 0);
      if (SUCCEEDED(present[frame])) pe_visible_frame();
    }
  }
  std::printf("DXVK_PE_D3D10 create=0x%08x buffer=0x%08x view=0x%08x present0=0x%08x present1=0x%08x\n",
      unsigned(create), unsigned(buffer), unsigned(target),
      unsigned(present[0]), unsigned(present[1]));
  if (view) view->Release();
  if (backbuffer) backbuffer->Release();
  if (swapchain) swapchain->Release();
  if (device) device->Release();
  DestroyWindow(window);
  return SUCCEEDED(create) && SUCCEEDED(buffer) && SUCCEEDED(target) &&
      SUCCEEDED(present[0]) && SUCCEEDED(present[1]) ? 0 : 1;
}
