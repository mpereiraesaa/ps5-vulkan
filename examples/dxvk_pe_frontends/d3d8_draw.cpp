// SPDX-License-Identifier: MIT
// DXVK 2.6.2 PE control: a fixed-function D3D8 triangle and two readbacks.
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
  IDirect3DVertexBuffer8* vertices = nullptr;

  ~State() {
    release(vertices);
    release(staging);
    release(backbuffer);
    release(device);
    release(api);
    if (window) DestroyWindow(window);
  }
};

struct Vertex {
  float x, y, z, rhw;
  D3DCOLOR diffuse;
};

static void stage(const char* name, int frame, HRESULT hr) {
  std::printf("DXVK_PE_D3D8_DRAW_STAGE name=%s frame=%d hr=0x%08x\n",
              name, frame, unsigned(hr));
  std::fflush(stdout);
}

static unsigned check_pixel(const char* place, int frame,
                            const std::uint8_t* pixel) {
  const bool centre = std::strcmp(place, "centre") == 0;
  const std::uint8_t expected[3] = {
      static_cast<std::uint8_t>(centre ? 0 : frame ? 28 : 132),
      static_cast<std::uint8_t>(centre ? 0 : 76),
      static_cast<std::uint8_t>(centre ? 255 : frame ? 132 : 28)};
  const unsigned mismatches = unsigned(pixel[0] != expected[0]) +
      unsigned(pixel[1] != expected[1]) + unsigned(pixel[2] != expected[2]);
  std::printf("DXVK_PE_D3D8_DRAW_PIXEL frame=%d place=%s bgra=%02x%02x%02x%02x mismatches=%u\n",
              frame, place, unsigned(pixel[0]), unsigned(pixel[1]),
              unsigned(pixel[2]), unsigned(pixel[3]), mismatches);
  std::fflush(stdout);
  return mismatches;
}

int main() {
  State state;
  state.window = pe_window(L"DXVK 2.6.2 D3D8 draw / Prospero Win");
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
  stage("device-return", -1, hr);
  if (FAILED(hr)) return 1;

  hr = state.device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO,
                                   &state.backbuffer);
  stage("backbuffer-return", -1, hr);
  if (FAILED(hr)) return 1;
  hr = state.device->CreateImageSurface(PE_WIDTH, PE_HEIGHT,
      D3DFMT_X8R8G8B8, &state.staging);
  stage("staging-return", -1, hr);
  if (FAILED(hr)) return 1;

  constexpr DWORD fvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
  hr = state.device->CreateVertexBuffer(3 * sizeof(Vertex), D3DUSAGE_WRITEONLY,
                                        fvf, D3DPOOL_DEFAULT, &state.vertices);
  stage("vertex-buffer-return", -1, hr);
  if (FAILED(hr)) return 1;
  BYTE* mapped = nullptr;
  hr = state.vertices->Lock(0, 0, &mapped, 0);
  stage("vertex-lock-return", -1, hr);
  if (FAILED(hr)) return 1;
  const Vertex triangle[3] = {
      {PE_WIDTH / 2.0f, PE_HEIGHT / 4.0f, 0.5f, 1.0f,
       D3DCOLOR_ARGB(255, 255, 0, 0)},
      {PE_WIDTH / 4.0f, PE_HEIGHT * 3.0f / 4.0f, 0.5f, 1.0f,
       D3DCOLOR_ARGB(255, 255, 0, 0)},
      {PE_WIDTH * 3.0f / 4.0f, PE_HEIGHT * 3.0f / 4.0f, 0.5f, 1.0f,
       D3DCOLOR_ARGB(255, 255, 0, 0)},
  };
  std::memcpy(mapped, triangle, sizeof(triangle));
  hr = state.vertices->Unlock();
  stage("vertex-unlock-return", -1, hr);
  if (FAILED(hr)) return 1;

  hr = state.device->SetRenderState(D3DRS_LIGHTING, FALSE);
  if (SUCCEEDED(hr)) hr = state.device->SetRenderState(D3DRS_ZENABLE, FALSE);
  if (SUCCEEDED(hr)) hr = state.device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  if (SUCCEEDED(hr)) hr = state.device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  if (SUCCEEDED(hr)) hr = state.device->SetTextureStageState(0, D3DTSS_COLOROP,
                                                              D3DTOP_SELECTARG1);
  if (SUCCEEDED(hr)) hr = state.device->SetTextureStageState(0, D3DTSS_COLORARG1,
                                                              D3DTA_DIFFUSE);
  if (SUCCEEDED(hr)) hr = state.device->SetTextureStageState(0, D3DTSS_ALPHAOP,
                                                              D3DTOP_SELECTARG1);
  if (SUCCEEDED(hr)) hr = state.device->SetTextureStageState(0, D3DTSS_ALPHAARG1,
                                                              D3DTA_DIFFUSE);
  if (SUCCEEDED(hr)) hr = state.device->SetVertexShader(fvf);
  if (SUCCEEDED(hr)) hr = state.device->SetStreamSource(0, state.vertices,
                                                        sizeof(Vertex));
  stage("pipeline-return", -1, hr);
  if (FAILED(hr)) return 1;

  unsigned mismatches = 0;
  HRESULT present[2] = {E_FAIL, E_FAIL};
  for (int frame = 0; frame < 2; ++frame) {
    const D3DCOLOR background = frame ? D3DCOLOR_XRGB(132, 76, 28)
                                      : D3DCOLOR_XRGB(28, 76, 132);
    hr = state.device->Clear(0, nullptr, D3DCLEAR_TARGET, background, 1.0f, 0);
    stage("clear-return", frame, hr);
    if (FAILED(hr)) return 1;
    hr = state.device->BeginScene();
    stage("begin-scene-return", frame, hr);
    if (FAILED(hr)) return 1;
    hr = state.device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
    stage("draw-return", frame, hr);
    const HRESULT end_hr = state.device->EndScene();
    stage("end-scene-return", frame, end_hr);
    if (FAILED(hr) || FAILED(end_hr)) return 1;

    stage("copy-enter", frame, S_OK);
    hr = state.device->CopyRects(state.backbuffer, nullptr, 0,
                                 state.staging, nullptr);
    stage("copy-return", frame, hr);
    if (FAILED(hr)) return 1;
    D3DLOCKED_RECT locked = {};
    stage("lock-enter", frame, S_OK);
    hr = state.staging->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    stage("lock-return", frame, hr);
    if (FAILED(hr)) return 1;
    const auto* bytes = static_cast<const std::uint8_t*>(locked.pBits);
    mismatches += check_pixel("centre", frame,
        bytes + (PE_HEIGHT / 2) * locked.Pitch + (PE_WIDTH / 2) * 4);
    mismatches += check_pixel("corner", frame, bytes + 8 * locked.Pitch + 8 * 4);
    state.staging->UnlockRect();

    stage("present-enter", frame, S_OK);
    present[frame] = state.device->Present(nullptr, nullptr, nullptr, nullptr);
    stage("present-return", frame, present[frame]);
    if (FAILED(present[frame])) return 1;
    pe_visible_frame();
  }
  const bool passed = !mismatches && SUCCEEDED(present[0]) &&
                      SUCCEEDED(present[1]);
  std::printf("DXVK_PE_D3D8_DRAW_RESULT present0=0x%08x present1=0x%08x mismatches=%u pass=%u\n",
              unsigned(present[0]), unsigned(present[1]), mismatches,
              unsigned(passed));
  std::fflush(stdout);
  return passed ? 0 : 1;
}
