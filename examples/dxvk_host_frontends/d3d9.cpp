// SPDX-License-Identifier: MIT
// DXVK native host control; no ps5vk or console execution is implied.
#include <d3d9.h>
#include <SDL2/SDL.h>
#include <cstdio>
int main() {
  if (SDL_Init(SDL_INIT_VIDEO)) return 2;
  SDL_Window* window = SDL_CreateWindow("DXVK D3D9 host smoke", SDL_WINDOWPOS_UNDEFINED,
      SDL_WINDOWPOS_UNDEFINED, 64, 64, SDL_WINDOW_SHOWN | SDL_WINDOW_VULKAN);
  if (!window) { SDL_Quit(); return 3; }
  IDirect3D9* api = Direct3DCreate9(D3D_SDK_VERSION);
  if (!api) { SDL_DestroyWindow(window); SDL_Quit(); return 4; }
  D3DPRESENT_PARAMETERS params = {};
  params.BackBufferWidth = 64;
  params.BackBufferHeight = 64;
  params.BackBufferFormat = D3DFMT_X8R8G8B8;
  params.BackBufferCount = 1;
  params.SwapEffect = D3DSWAPEFFECT_DISCARD;
  params.hDeviceWindow = reinterpret_cast<HWND>(window);
  params.Windowed = TRUE;
  params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9* device = nullptr;
  HRESULT create = api->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
      params.hDeviceWindow, D3DCREATE_HARDWARE_VERTEXPROCESSING, &params, &device);
  HRESULT clear = device ? device->Clear(0, nullptr, D3DCLEAR_TARGET,
      D3DCOLOR_XRGB(28, 76, 132), 1.0f, 0) : create;
  HRESULT present = device && SUCCEEDED(clear) ? device->Present(nullptr, nullptr,
      nullptr, nullptr) : clear;
  std::printf("DXVK_NATIVE_D3D9 create=0x%08x clear=0x%08x present=0x%08x\n",
      static_cast<unsigned>(create), static_cast<unsigned>(clear),
      static_cast<unsigned>(present));
  if (device) device->Release();
  api->Release();
  SDL_DestroyWindow(window);
  SDL_Quit();
  return SUCCEEDED(create) && SUCCEEDED(clear) && SUCCEEDED(present) ? 0 : 1;
}
