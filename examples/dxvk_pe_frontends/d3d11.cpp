// SPDX-License-Identifier: MIT
// PE application control: DXVK D3D11/DXGI -> ps5vk.
#include "window.h"
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>

int main() {
  HWND window = pe_window(L"DXVK 2.6.2 D3D11 / PS5 WSI");
  if (!window) return 2;
  IDXGIFactory* factory = nullptr;
  IDXGIAdapter* adapter = nullptr;
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  IDXGISwapChain* swapchain = nullptr;
  ID3D11Texture2D* backbuffer = nullptr;
  ID3D11RenderTargetView* view = nullptr;
  D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_11_0;
  D3D_FEATURE_LEVEL selected = static_cast<D3D_FEATURE_LEVEL>(0);
  HRESULT factory_hr = CreateDXGIFactory(__uuidof(IDXGIFactory),
      reinterpret_cast<void**>(&factory));
  HRESULT adapter_hr = SUCCEEDED(factory_hr) && factory ?
      factory->EnumAdapters(0, &adapter) : factory_hr;
  HRESULT device_hr = SUCCEEDED(adapter_hr) && adapter ? D3D11CreateDevice(adapter,
      D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &requested, 1, D3D11_SDK_VERSION,
      &device, &selected, &context) : adapter_hr;
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
  HRESULT swap_hr = SUCCEEDED(device_hr) && factory && device ?
      factory->CreateSwapChain(device, &desc, &swapchain) : device_hr;
  HRESULT buffer_hr = SUCCEEDED(swap_hr) && swapchain ? swapchain->GetBuffer(0,
      __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer)) : swap_hr;
  HRESULT view_hr = SUCCEEDED(buffer_hr) && device ? device->CreateRenderTargetView(
      backbuffer, nullptr, &view) : buffer_hr;
  HRESULT present[2] = {view_hr, view_hr};
  if (SUCCEEDED(view_hr) && context && view) {
    const float colors[2][4] = {{0.1f, 0.3f, 0.5f, 1.0f},
                                {0.5f, 0.3f, 0.1f, 1.0f}};
    for (int frame = 0; frame < 2; ++frame) {
      context->ClearRenderTargetView(view, colors[frame]);
      present[frame] = swapchain->Present(1, 0);
      if (SUCCEEDED(present[frame])) pe_visible_frame();
    }
  }
  std::printf("DXVK_PE_D3D11 factory=0x%08x adapter=0x%08x device=0x%08x level=0x%04x swap=0x%08x buffer=0x%08x view=0x%08x present0=0x%08x present1=0x%08x\n",
      unsigned(factory_hr), unsigned(adapter_hr), unsigned(device_hr),
      unsigned(selected), unsigned(swap_hr), unsigned(buffer_hr),
      unsigned(view_hr), unsigned(present[0]), unsigned(present[1]));
  if (view) view->Release();
  if (backbuffer) backbuffer->Release();
  if (swapchain) swapchain->Release();
  if (context) context->Release();
  if (device) device->Release();
  if (adapter) adapter->Release();
  if (factory) factory->Release();
  DestroyWindow(window);
  return SUCCEEDED(factory_hr) && SUCCEEDED(adapter_hr) && SUCCEEDED(device_hr) &&
      selected == D3D_FEATURE_LEVEL_11_0 && SUCCEEDED(swap_hr) &&
      SUCCEEDED(buffer_hr) && SUCCEEDED(view_hr) &&
      SUCCEEDED(present[0]) && SUCCEEDED(present[1]) ? 0 : 1;
}
