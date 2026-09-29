// SPDX-License-Identifier: MIT
// DXVK 2.6.2 PE control: four visible D3D8 quadrants for 30 bounded Presents.
#include "window.h"
#include <d3d8.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

template <typename T> static void release(T*& value) {
  if (value) value->Release();
  value = nullptr;
}

struct State {
  HWND window = nullptr;
  IDirect3D8* api = nullptr;
  IDirect3DDevice8* device = nullptr;
  IDirect3DSurface8* backbuffer = nullptr;
  IDirect3DSurface8* staging = nullptr;

  ~State() {
    release(staging);
    release(backbuffer);
    release(device);
    release(api);
    if (window) DestroyWindow(window);
  }
};

static void stage(const char* name, HRESULT hr) {
  std::printf("DXVK_PE_D3D8_SCANOUT_STAGE name=%s hr=0x%08x\n",
              name, unsigned(hr));
  std::fflush(stdout);
}

static unsigned check_pixel(const char* place, const std::uint8_t* bgra) {
  std::uint8_t expected[3] = {0, 0, 0};
  if (std::strcmp(place, "TL") == 0) expected[2] = 255;
  else if (std::strcmp(place, "TR") == 0) expected[1] = 255;
  else if (std::strcmp(place, "BL") == 0) expected[0] = 255;
  else { expected[1] = 255; expected[2] = 255; }
  const unsigned mismatches = unsigned(bgra[0] != expected[0]) +
      unsigned(bgra[1] != expected[1]) + unsigned(bgra[2] != expected[2]);
  std::printf("DXVK_PE_D3D8_SCANOUT_PIXEL place=%s bgra=%02x%02x%02x%02x mismatches=%u\n",
              place, unsigned(bgra[0]), unsigned(bgra[1]),
              unsigned(bgra[2]), unsigned(bgra[3]), mismatches);
  std::fflush(stdout);
  return mismatches;
}

int main() {
  State state;
  state.window = pe_window(L"DXVK D3D8 scanout / red green blue yellow");
  if (!state.window) return 2;
  state.api = Direct3DCreate8(D3D_SDK_VERSION);
  if (!state.api) return 3;
  D3DPRESENT_PARAMETERS params = {};
  params.BackBufferWidth = PE_WIDTH;
  params.BackBufferHeight = PE_HEIGHT;
  params.BackBufferFormat = D3DFMT_X8R8G8B8;
  params.BackBufferCount = 2;
  params.SwapEffect = D3DSWAPEFFECT_DISCARD;
  params.hDeviceWindow = state.window;
  params.Windowed = TRUE;
  params.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
  HRESULT hr = state.api->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
      state.window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &params, &state.device);
  stage("device", hr);
  if (FAILED(hr)) return 1;
  hr = state.device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO,
                                   &state.backbuffer);
  stage("backbuffer", hr);
  if (FAILED(hr)) return 1;
  hr = state.device->CreateImageSurface(PE_WIDTH, PE_HEIGHT,
      D3DFMT_X8R8G8B8, &state.staging);
  stage("staging", hr);
  if (FAILED(hr)) return 1;

  const D3DRECT rects[4] = {
      {0, 0, PE_WIDTH / 2, PE_HEIGHT / 2},
      {PE_WIDTH / 2, 0, PE_WIDTH, PE_HEIGHT / 2},
      {0, PE_HEIGHT / 2, PE_WIDTH / 2, PE_HEIGHT},
      {PE_WIDTH / 2, PE_HEIGHT / 2, PE_WIDTH, PE_HEIGHT},
  };
  const D3DCOLOR colors[4] = {
      D3DCOLOR_XRGB(255, 0, 0), D3DCOLOR_XRGB(0, 255, 0),
      D3DCOLOR_XRGB(0, 0, 255), D3DCOLOR_XRGB(255, 255, 0),
  };
  unsigned mismatches = 0;
  int frames = 0;
  HRESULT present = E_FAIL;
  for (int frame = 0; frame < 30; ++frame) {
    for (unsigned quadrant = 0; quadrant < 4; ++quadrant) {
      hr = state.device->Clear(1, &rects[quadrant], D3DCLEAR_TARGET,
                               colors[quadrant], 1.0f, 0);
      if (FAILED(hr)) {
        stage("clear", hr);
        goto done;
      }
    }
    if (frame == 0) {
      hr = state.device->CopyRects(state.backbuffer, nullptr, 0,
                                   state.staging, nullptr);
      stage("copy", hr);
      if (FAILED(hr)) goto done;
      D3DLOCKED_RECT locked = {};
      hr = state.staging->LockRect(&locked, nullptr, D3DLOCK_READONLY);
      stage("lock", hr);
      if (FAILED(hr)) goto done;
      const auto* bytes = static_cast<const std::uint8_t*>(locked.pBits);
      const struct { const char* name; int x, y; } samples[4] = {
          {"TL", PE_WIDTH / 4, PE_HEIGHT / 4},
          {"TR", PE_WIDTH * 3 / 4, PE_HEIGHT / 4},
          {"BL", PE_WIDTH / 4, PE_HEIGHT * 3 / 4},
          {"BR", PE_WIDTH * 3 / 4, PE_HEIGHT * 3 / 4},
      };
      for (const auto& sample : samples)
        mismatches += check_pixel(sample.name,
            bytes + sample.y * locked.Pitch + sample.x * 4);
      state.staging->UnlockRect();
      if (mismatches) goto done;
    }
    present = state.device->Present(nullptr, nullptr, nullptr, nullptr);
    if (FAILED(present)) {
      stage("present", present);
      goto done;
    }
    ++frames;
    if (frames % 10 == 0) {
      std::printf("DXVK_PE_D3D8_SCANOUT_PROGRESS frames=%d\n", frames);
      std::fflush(stdout);
    }
    pe_visible_frame();
  }
done:
  const bool passed = SUCCEEDED(hr) && SUCCEEDED(present) &&
      frames == 30 && !mismatches;
  std::printf("DXVK_PE_D3D8_SCANOUT_RESULT hr=0x%08x frames=%d present=0x%08x mismatches=%u pass=%u\n",
              unsigned(hr), frames, unsigned(present), mismatches,
              unsigned(passed));
  std::fflush(stdout);
  return passed ? 0 : 1;
}
