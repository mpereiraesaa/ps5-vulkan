// SPDX-License-Identifier: MIT
// PE application control: DXVK 2.6.2 D3D9 -> ps5vk.
#include "window.h"
#include <d3d9.h>
#include <cstdio>
#ifdef PS5VK_PE_PIXEL_ORACLE
#include "pixel_oracle.h"

static void pe_d3d9_stage(unsigned frame, const char* stage, HRESULT result) {
  std::printf("DXVK_PE_D3D9_PIXEL_STAGE frame=%u stage=%s hr=0x%08x\n",
              frame, stage, unsigned(result));
  std::fflush(stdout);
}
#endif

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
#ifdef PS5VK_PE_PIXEL_ORACLE
  IDirect3DSurface9* backbuffer = nullptr;
  IDirect3DSurface9* staging = nullptr;
  HRESULT buffer_hr = E_FAIL;
  HRESULT staging_hr = E_FAIL;
  HRESULT copy_hr[2] = {E_FAIL, E_FAIL};
  HRESULT lock_hr[2] = {E_FAIL, E_FAIL};
  unsigned mismatches[2] = {3, 3};
  if (SUCCEEDED(create) && device) {
    buffer_hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
    if (SUCCEEDED(buffer_hr))
      staging_hr = device->CreateOffscreenPlainSurface(PE_WIDTH, PE_HEIGHT,
          D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &staging, nullptr);
  }
#endif
  const D3DCOLOR colors[2] = {D3DCOLOR_XRGB(28, 76, 132),
                              D3DCOLOR_XRGB(132, 76, 28)};
  if (SUCCEEDED(create) && device) {
    for (int frame = 0; frame < 2; ++frame) {
      clear[frame] = device->Clear(0, nullptr, D3DCLEAR_TARGET, colors[frame], 1.0f, 0);
#ifdef PS5VK_PE_PIXEL_ORACLE
      if (SUCCEEDED(clear[frame]) && SUCCEEDED(staging_hr) && staging) {
        pe_d3d9_stage(unsigned(frame), "copy-enter", clear[frame]);
        copy_hr[frame] = device->GetRenderTargetData(backbuffer, staging);
        pe_d3d9_stage(unsigned(frame), "copy-return", copy_hr[frame]);
        if (SUCCEEDED(copy_hr[frame])) {
          D3DLOCKED_RECT locked = {};
          pe_d3d9_stage(unsigned(frame), "lock-enter", copy_hr[frame]);
          lock_hr[frame] = staging->LockRect(&locked, nullptr, D3DLOCK_READONLY);
          pe_d3d9_stage(unsigned(frame), "lock-return", lock_hr[frame]);
          if (SUCCEEDED(lock_hr[frame])) {
            const auto* pixel = static_cast<const unsigned char*>(locked.pBits) +
                (PE_HEIGHT / 2) * locked.Pitch + (PE_WIDTH / 2) * 4;
            mismatches[frame] = pe_log_pixel("D3D9", unsigned(frame), pixel, false);
            std::fflush(stdout);
            staging->UnlockRect();
          }
        }
      }
      if (SUCCEEDED(clear[frame]))
        pe_d3d9_stage(unsigned(frame), "present-enter", clear[frame]);
#endif
      present[frame] = SUCCEEDED(clear[frame]) ?
          device->Present(nullptr, nullptr, nullptr, nullptr) : clear[frame];
#ifdef PS5VK_PE_PIXEL_ORACLE
      pe_d3d9_stage(unsigned(frame), "present-return", present[frame]);
#endif
      if (SUCCEEDED(present[frame])) pe_visible_frame();
    }
  }
  std::printf("DXVK_PE_D3D9 create=0x%08x clear0=0x%08x present0=0x%08x clear1=0x%08x present1=0x%08x\n",
      unsigned(create), unsigned(clear[0]), unsigned(present[0]),
      unsigned(clear[1]), unsigned(present[1]));
#ifdef PS5VK_PE_PIXEL_ORACLE
  std::printf("DXVK_PE_D3D9_PIXEL_RESULT buffer=0x%08x staging=0x%08x copy0=0x%08x lock0=0x%08x copy1=0x%08x lock1=0x%08x mismatches=%u\n",
              unsigned(buffer_hr), unsigned(staging_hr), unsigned(copy_hr[0]),
              unsigned(lock_hr[0]), unsigned(copy_hr[1]), unsigned(lock_hr[1]),
              mismatches[0] + mismatches[1]);
  if (staging) staging->Release();
  if (backbuffer) backbuffer->Release();
#endif
  if (device) device->Release();
  api->Release();
  DestroyWindow(window);
  return SUCCEEDED(create) && SUCCEEDED(clear[0]) && SUCCEEDED(present[0]) &&
      SUCCEEDED(clear[1]) && SUCCEEDED(present[1])
#ifdef PS5VK_PE_PIXEL_ORACLE
      && SUCCEEDED(buffer_hr) && SUCCEEDED(staging_hr) &&
      SUCCEEDED(copy_hr[0]) && SUCCEEDED(copy_hr[1]) &&
      SUCCEEDED(lock_hr[0]) && SUCCEEDED(lock_hr[1]) &&
      !mismatches[0] && !mismatches[1]
#endif
      ? 0 : 1;
}
