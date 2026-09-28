// SPDX-License-Identifier: MIT
// DXVK native host control; no ps5vk or console execution is implied.
#include <d3d11.h>
#include <dxgi.h>
#include <SDL2/SDL.h>
#include <cstdio>
int main() {
  if (SDL_Init(SDL_INIT_VIDEO)) return 2;
  SDL_Window* window = SDL_CreateWindow("DXVK D3D11 host smoke", SDL_WINDOWPOS_UNDEFINED,
      SDL_WINDOWPOS_UNDEFINED, 64, 64, SDL_WINDOW_SHOWN | SDL_WINDOW_VULKAN);
  if (!window) { SDL_Quit(); return 3; }
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
  HRESULT adapter_hr = SUCCEEDED(factory_hr) ? factory->EnumAdapters(0, &adapter) : factory_hr;
  HRESULT device_hr = SUCCEEDED(adapter_hr) ? D3D11CreateDevice(adapter,
      D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &requested, 1, D3D11_SDK_VERSION,
      &device, &selected, &context) : adapter_hr;
  DXGI_SWAP_CHAIN_DESC desc = {};
  desc.BufferDesc.Width = 64;
  desc.BufferDesc.Height = 64;
  desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.OutputWindow = reinterpret_cast<HWND>(window);
  desc.Windowed = TRUE;
  desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  HRESULT swap_hr = SUCCEEDED(device_hr) ? factory->CreateSwapChain(device, &desc,
      &swapchain) : device_hr;
  HRESULT buffer_hr = SUCCEEDED(swap_hr) ? swapchain->GetBuffer(0,
      __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer)) : swap_hr;
  HRESULT view_hr = SUCCEEDED(buffer_hr) ? device->CreateRenderTargetView(backbuffer,
      nullptr, &view) : buffer_hr;
  if (SUCCEEDED(view_hr)) {
    const float color[4] = {0.1f, 0.3f, 0.5f, 1.0f};
    context->ClearRenderTargetView(view, color);
  }
  HRESULT present_hr = SUCCEEDED(view_hr) ? swapchain->Present(0, 0) : view_hr;
  std::printf("DXVK_NATIVE_D3D11 factory=0x%08x adapter=0x%08x device=0x%08x level=0x%04x swap=0x%08x buffer=0x%08x view=0x%08x present=0x%08x\n",
      static_cast<unsigned>(factory_hr), static_cast<unsigned>(adapter_hr),
      static_cast<unsigned>(device_hr), static_cast<unsigned>(selected),
      static_cast<unsigned>(swap_hr), static_cast<unsigned>(buffer_hr),
      static_cast<unsigned>(view_hr), static_cast<unsigned>(present_hr));
  if (view) view->Release();
  if (backbuffer) backbuffer->Release();
  if (swapchain) swapchain->Release();
  if (context) context->Release();
  if (device) device->Release();
  if (adapter) adapter->Release();
  if (factory) factory->Release();
  SDL_DestroyWindow(window);
  SDL_Quit();
  return SUCCEEDED(factory_hr) && SUCCEEDED(adapter_hr) && SUCCEEDED(device_hr)
      && selected == D3D_FEATURE_LEVEL_11_0 && SUCCEEDED(swap_hr)
      && SUCCEEDED(buffer_hr) && SUCCEEDED(view_hr) && SUCCEEDED(present_hr) ? 0 : 1;
}
