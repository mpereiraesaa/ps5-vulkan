// SPDX-License-Identifier: MIT
// PE application control: Wine d3d10.dll wrapper -> DXVK D3D10 core.
#include "window.h"
#include <d3d10.h>
#include <dxgi.h>
#include <cstdio>
#ifdef PS5VK_PE_PIXEL_ORACLE
#include "pixel_oracle.h"
#endif

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
#ifdef PS5VK_PE_PIXEL_ORACLE
  ID3D10Texture2D* staging = nullptr;
  HRESULT staging_hr = E_FAIL;
  HRESULT pixel_hr[2] = {E_FAIL, E_FAIL};
  unsigned mismatches[2] = {4, 4};
#endif
  HRESULT create = D3D10CreateDeviceAndSwapChain(nullptr, D3D10_DRIVER_TYPE_HARDWARE,
      nullptr, 0, D3D10_SDK_VERSION, &desc, &swapchain, &device);
  HRESULT buffer = SUCCEEDED(create) && swapchain ? swapchain->GetBuffer(0,
      __uuidof(ID3D10Texture2D), reinterpret_cast<void**>(&backbuffer)) : create;
  HRESULT target = SUCCEEDED(buffer) && device ? device->CreateRenderTargetView(
      backbuffer, nullptr, &view) : buffer;
#ifdef PS5VK_PE_PIXEL_ORACLE
  if (SUCCEEDED(target)) {
    D3D10_TEXTURE2D_DESC staging_desc = {};
    staging_desc.Width = 1;
    staging_desc.Height = 1;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Usage = D3D10_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    staging_hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
  }
#endif
  HRESULT present[2] = {target, target};
  if (SUCCEEDED(target) && view) {
    const float colors[2][4] = {{28.0f / 255.0f, 76.0f / 255.0f,
                                 132.0f / 255.0f, 1.0f},
                                {132.0f / 255.0f, 76.0f / 255.0f,
                                 28.0f / 255.0f, 1.0f}};
    for (int frame = 0; frame < 2; ++frame) {
      device->ClearRenderTargetView(view, colors[frame]);
#ifdef PS5VK_PE_PIXEL_ORACLE
      if (SUCCEEDED(staging_hr) && staging) {
        D3D10_BOX box = {PE_WIDTH / 2, PE_HEIGHT / 2, 0,
                         PE_WIDTH / 2 + 1, PE_HEIGHT / 2 + 1, 1};
        device->CopySubresourceRegion(staging, 0, 0, 0, 0, backbuffer, 0, &box);
        D3D10_MAPPED_TEXTURE2D mapped = {};
        pixel_hr[frame] = staging->Map(0, D3D10_MAP_READ, 0, &mapped);
        if (SUCCEEDED(pixel_hr[frame])) {
          mismatches[frame] = pe_log_pixel("D3D10", unsigned(frame), mapped.pData, true);
          staging->Unmap(0);
        }
      }
#endif
      present[frame] = swapchain->Present(1, 0);
      if (SUCCEEDED(present[frame])) pe_visible_frame();
    }
  }
  std::printf("DXVK_PE_D3D10 create=0x%08x buffer=0x%08x view=0x%08x present0=0x%08x present1=0x%08x\n",
      unsigned(create), unsigned(buffer), unsigned(target),
      unsigned(present[0]), unsigned(present[1]));
#ifdef PS5VK_PE_PIXEL_ORACLE
  std::printf("DXVK_PE_D3D10_PIXEL_RESULT staging=0x%08x map0=0x%08x map1=0x%08x mismatches=%u\n",
              unsigned(staging_hr), unsigned(pixel_hr[0]), unsigned(pixel_hr[1]),
              mismatches[0] + mismatches[1]);
  if (staging) staging->Release();
#endif
  if (view) view->Release();
  if (backbuffer) backbuffer->Release();
  if (swapchain) swapchain->Release();
  if (device) device->Release();
  DestroyWindow(window);
  return SUCCEEDED(create) && SUCCEEDED(buffer) && SUCCEEDED(target) &&
      SUCCEEDED(present[0]) && SUCCEEDED(present[1])
#ifdef PS5VK_PE_PIXEL_ORACLE
      && SUCCEEDED(staging_hr) && SUCCEEDED(pixel_hr[0]) && SUCCEEDED(pixel_hr[1]) &&
      !mismatches[0] && !mismatches[1]
#endif
      ? 0 : 1;
}
