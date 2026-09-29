// SPDX-License-Identifier: MIT
// DXVK 2.6.2 PE control: four visible 1080p quadrants for bounded TV inspection.
#include "window.h"
#include <d3d9.h>
#include <cstdint>
#include <cstdio>

static void stage(const char* name, int frame, HRESULT hr) {
  std::printf("DXVK_PE_D3D9_SCANOUT_STAGE name=%s frame=%d hr=0x%08x\n",
              name, frame, unsigned(hr));
  std::fflush(stdout);
}

static unsigned check_pixel(const char* quadrant, const std::uint8_t* pixel,
                            std::uint8_t blue, std::uint8_t green,
                            std::uint8_t red) {
  const unsigned mismatch = unsigned(pixel[0] != blue) +
      unsigned(pixel[1] != green) + unsigned(pixel[2] != red);
  std::printf("DXVK_PE_D3D9_SCANOUT_PIXEL quadrant=%s bgra=%02x%02x%02x%02x mismatches=%u\n",
              quadrant, unsigned(pixel[0]), unsigned(pixel[1]),
              unsigned(pixel[2]), unsigned(pixel[3]), mismatch);
  std::fflush(stdout);
  return mismatch;
}

int main() {
  HWND window = pe_window(L"DXVK D3D9 scanout / red green blue yellow");
  if (!window) return 2;
  IDirect3D9* api = Direct3DCreate9(D3D_SDK_VERSION);
  if (!api) return 3;
  IDirect3DDevice9* device = nullptr;
  IDirect3DSurface9* backbuffer = nullptr;
  IDirect3DSurface9* staging = nullptr;
  HRESULT hr = E_FAIL;
  HRESULT present = E_FAIL;
  unsigned mismatches = 0;
  int frames = 0;
  D3DPRESENT_PARAMETERS params = {};
  params.BackBufferWidth = PE_WIDTH;
  params.BackBufferHeight = PE_HEIGHT;
  params.BackBufferFormat = D3DFMT_X8R8G8B8;
  params.BackBufferCount = 2;
  params.SwapEffect = D3DSWAPEFFECT_DISCARD;
  params.hDeviceWindow = window;
  params.Windowed = TRUE;
  params.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
  hr = api->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                         D3DCREATE_HARDWARE_VERTEXPROCESSING, &params, &device);
  stage("device-return", -1, hr);
  if (FAILED(hr)) goto done;
  hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
  stage("backbuffer-return", -1, hr);
  if (FAILED(hr)) goto done;
  hr = device->CreateOffscreenPlainSurface(PE_WIDTH, PE_HEIGHT,
      D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &staging, nullptr);
  stage("staging-return", -1, hr);
  if (FAILED(hr)) goto done;
  for (int frame = 0; frame < 30; ++frame) {
    const D3DRECT rects[4] = {
        {0, 0, PE_WIDTH / 2, PE_HEIGHT / 2},
        {PE_WIDTH / 2, 0, PE_WIDTH, PE_HEIGHT / 2},
        {0, PE_HEIGHT / 2, PE_WIDTH / 2, PE_HEIGHT},
        {PE_WIDTH / 2, PE_HEIGHT / 2, PE_WIDTH, PE_HEIGHT}};
    const D3DCOLOR colors[4] = {
        D3DCOLOR_XRGB(255, 0, 0), D3DCOLOR_XRGB(0, 255, 0),
        D3DCOLOR_XRGB(0, 0, 255), D3DCOLOR_XRGB(255, 255, 0)};
    for (int quadrant = 0; quadrant < 4; ++quadrant) {
      hr = device->Clear(1, &rects[quadrant], D3DCLEAR_TARGET,
                         colors[quadrant], 1.0f, 0);
      if (FAILED(hr)) {
        stage("clear-return", frame, hr);
        goto done;
      }
    }
    if (frame == 0) {
      hr = device->GetRenderTargetData(backbuffer, staging);
      stage("readback-return", 0, hr);
      if (FAILED(hr)) goto done;
      D3DLOCKED_RECT mapped = {};
      hr = staging->LockRect(&mapped, nullptr, D3DLOCK_READONLY);
      stage("lock-return", 0, hr);
      if (FAILED(hr)) goto done;
      const auto* data = static_cast<const std::uint8_t*>(mapped.pBits);
      const auto sample = [&](int x, int y) {
        return data + y * mapped.Pitch + x * 4;
      };
      mismatches += check_pixel("TL", sample(PE_WIDTH / 4, PE_HEIGHT / 4),
                                0, 0, 255);
      mismatches += check_pixel("TR", sample(PE_WIDTH * 3 / 4, PE_HEIGHT / 4),
                                0, 255, 0);
      mismatches += check_pixel("BL", sample(PE_WIDTH / 4, PE_HEIGHT * 3 / 4),
                                255, 0, 0);
      mismatches += check_pixel("BR", sample(PE_WIDTH * 3 / 4, PE_HEIGHT * 3 / 4),
                                0, 255, 255);
      staging->UnlockRect();
      if (mismatches) goto done;
    }
    present = device->Present(nullptr, nullptr, nullptr, nullptr);
    if (FAILED(present)) {
      stage("present-return", frame, present);
      goto done;
    }
    ++frames;
    if ((frame + 1) % 10 == 0) {
      std::printf("DXVK_PE_D3D9_SCANOUT_PROGRESS frames=%d\n", frames);
      std::fflush(stdout);
    }
    pe_visible_frame();
  }
done:
  const bool passed = SUCCEEDED(hr) && SUCCEEDED(present) &&
                      mismatches == 0 && frames == 30;
  std::printf("DXVK_PE_D3D9_SCANOUT_RESULT frames=%d present=0x%08x mismatches=%u pass=%u\n",
              frames, unsigned(present), mismatches, unsigned(passed));
  std::fflush(stdout);
  if (staging) staging->Release();
  if (backbuffer) backbuffer->Release();
  if (device) device->Release();
  api->Release();
  DestroyWindow(window);
  return passed ? 0 : 1;
}
