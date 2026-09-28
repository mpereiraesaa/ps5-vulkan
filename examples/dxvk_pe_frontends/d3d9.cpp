// SPDX-License-Identifier: MIT
// PE application control: DXVK 2.6.2 D3D9 -> ps5vk.
#include "window.h"
#include <d3d9.h>
#include <cstdio>

int main() {
  HWND window = pe_window(L"DXVK 2.6.2 D3D9 / Prospero Win");
  if (!window) return 2;
  IDirect3D9* api = Direct3DCreate9(D3D_SDK_VERSION);
  if (!api) { DestroyWindow(window); return 3; }
  D3DPRESENT_PARAMETERS params = {};
  params.BackBufferWidth = PE_WIDTH;
  params.BackBufferHeight = PE_HEIGHT;
  params.BackBufferFormat = D3DFMT_X8R8G8B8;
  params.BackBufferCount = 2;
  params.SwapEffect = D3DSWAPEFFECT_DISCARD;
  params.hDeviceWindow = window;
  params.Windowed = TRUE;
  params.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
  IDirect3DDevice9* device = nullptr;
  HRESULT create = api->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
      window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &params, &device);
  HRESULT clear[2] = {create, create};
  HRESULT present[2] = {create, create};
  const D3DCOLOR colors[2] = {D3DCOLOR_XRGB(28, 76, 132),
                              D3DCOLOR_XRGB(132, 76, 28)};
  if (SUCCEEDED(create) && device) {
    for (int frame = 0; frame < 2; ++frame) {
      clear[frame] = device->Clear(0, nullptr, D3DCLEAR_TARGET, colors[frame], 1.0f, 0);
      present[frame] = SUCCEEDED(clear[frame]) ?
          device->Present(nullptr, nullptr, nullptr, nullptr) : clear[frame];
      if (SUCCEEDED(present[frame])) pe_visible_frame();
    }
  }
  std::printf("DXVK_PE_D3D9 create=0x%08x clear0=0x%08x present0=0x%08x clear1=0x%08x present1=0x%08x\n",
      unsigned(create), unsigned(clear[0]), unsigned(present[0]),
      unsigned(clear[1]), unsigned(present[1]));
  if (device) device->Release();
  api->Release();
  DestroyWindow(window);
  return SUCCEEDED(create) && SUCCEEDED(clear[0]) && SUCCEEDED(present[0]) &&
      SUCCEEDED(clear[1]) && SUCCEEDED(present[1]) ? 0 : 1;
}
