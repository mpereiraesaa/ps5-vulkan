// SPDX-License-Identifier: MIT
// DXVK 2.6.2 PE control: four visible D3D10 quadrants for 30 bounded Presents.
#include "window.h"
#include <d3d10.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

template <typename T> static void release(T*& value) {
  if (value) value->Release();
  value = nullptr;
}

static void stage(const char* name, int frame, HRESULT hr) {
  std::printf("DXVK_PE_D3D10_SCANOUT_STAGE name=%s frame=%d hr=0x%08x\n",
              name, frame, unsigned(hr));
  std::fflush(stdout);
}

static unsigned check_pixel(const char* place, const void* data) {
  const auto* bgra = static_cast<const std::uint8_t*>(data);
  std::uint8_t expected[4] = {0, 0, 0, 255};
  if (std::strcmp(place, "TL") == 0) expected[2] = 255;
  else if (std::strcmp(place, "TR") == 0) expected[1] = 255;
  else if (std::strcmp(place, "BL") == 0) expected[0] = 255;
  else { expected[1] = 255; expected[2] = 255; }
  const unsigned mismatches = unsigned(bgra[0] != expected[0]) +
      unsigned(bgra[1] != expected[1]) + unsigned(bgra[2] != expected[2]) +
      unsigned(bgra[3] != expected[3]);
  std::printf("DXVK_PE_D3D10_SCANOUT_PIXEL place=%s bgra=%02x%02x%02x%02x mismatches=%u\n",
              place, unsigned(bgra[0]), unsigned(bgra[1]),
              unsigned(bgra[2]), unsigned(bgra[3]), mismatches);
  std::fflush(stdout);
  return mismatches;
}

static HRESULT read_pixel(ID3D10Device* device, ID3D10Texture2D* backbuffer,
                          ID3D10Texture2D* staging, const char* place,
                          UINT x, UINT y, unsigned* mismatches) {
  const D3D10_BOX box = {x, y, 0, x + 1, y + 1, 1};
  device->CopySubresourceRegion(staging, 0, 0, 0, 0, backbuffer, 0, &box);
  D3D10_MAPPED_TEXTURE2D mapped = {};
  const HRESULT hr = staging->Map(0, D3D10_MAP_READ, 0, &mapped);
  stage("pixel-map-return", 0, hr);
  if (SUCCEEDED(hr)) {
    *mismatches += check_pixel(place, mapped.pData);
    staging->Unmap(0);
  }
  return hr;
}

int main() {
  HWND window = pe_window(L"DXVK 2.6.2 D3D10 scanout / red green blue yellow");
  if (!window) return 2;
  ID3D10Device* device = nullptr;
  IDXGISwapChain* swapchain = nullptr;
  ID3D10Texture2D* backbuffer = nullptr;
  ID3D10RenderTargetView* view = nullptr;
  ID3D10Texture2D* staging = nullptr;
  ID3D10VertexShader* vs = nullptr;
  ID3D10PixelShader* ps = nullptr;
  ID3D10InputLayout* layout = nullptr;
  ID3D10Buffer* vertex_buffer = nullptr;
  ID3DBlob* vs_code = nullptr;
  ID3DBlob* ps_code = nullptr;
  ID3DBlob* errors = nullptr;
  HRESULT result = E_FAIL;
  HRESULT present = E_FAIL;
  int frames = 0;
  unsigned mismatches = 0;

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
  stage("device-enter", -1, S_OK);
  result = D3D10CreateDeviceAndSwapChain(nullptr, D3D10_DRIVER_TYPE_HARDWARE,
      nullptr, 0, D3D10_SDK_VERSION, &desc, &swapchain, &device);
  stage("device-return", -1, result);
  if (FAILED(result)) goto done;
  result = swapchain->GetBuffer(0, __uuidof(ID3D10Texture2D),
                                reinterpret_cast<void**>(&backbuffer));
  stage("backbuffer-return", -1, result);
  if (FAILED(result)) goto done;
  result = device->CreateRenderTargetView(backbuffer, nullptr, &view);
  stage("view-return", -1, result);
  if (FAILED(result)) goto done;

  {
    D3D10_TEXTURE2D_DESC desc = {};
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D10_USAGE_STAGING;
    desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    result = device->CreateTexture2D(&desc, nullptr, &staging);
    stage("staging-return", -1, result);
    if (FAILED(result)) goto done;
  }

  {
    const char shader[] =
        "struct VIn { float2 pos : POSITION; };"
        "struct VOut { float4 pos : SV_Position; };"
        "VOut vs_main(VIn input) { VOut o; o.pos = float4(input.pos, 0, 1); return o; }"
        "float4 ps_main(VOut input) : SV_Target {"
        "float2 p = input.pos.xy;"
        "if (p.x < 960) { if (p.y < 540) return float4(1,0,0,1); return float4(0,0,1,1); }"
        "if (p.y < 540) return float4(0,1,0,1); return float4(1,1,0,1);"
        "}";
    stage("vs-compile-enter", -1, S_OK);
    result = D3DCompile(shader, sizeof(shader) - 1, "draw-control", nullptr,
                        nullptr, "vs_main", "vs_4_0", 0, 0, &vs_code, &errors);
    stage("vs-compile-return", -1, result);
    if (FAILED(result)) goto done;
    release(errors);
    stage("ps-compile-enter", -1, S_OK);
    result = D3DCompile(shader, sizeof(shader) - 1, "draw-control", nullptr,
                        nullptr, "ps_main", "ps_4_0", 0, 0, &ps_code, &errors);
    stage("ps-compile-return", -1, result);
    if (FAILED(result)) goto done;
  }
  result = device->CreateVertexShader(vs_code->GetBufferPointer(),
                                       vs_code->GetBufferSize(), &vs);
  stage("vertex-shader-return", -1, result);
  if (FAILED(result)) goto done;
  result = device->CreatePixelShader(ps_code->GetBufferPointer(),
                                      ps_code->GetBufferSize(), &ps);
  stage("pixel-shader-return", -1, result);
  if (FAILED(result)) goto done;
  {
    const D3D10_INPUT_ELEMENT_DESC element =
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
         D3D10_INPUT_PER_VERTEX_DATA, 0};
    result = device->CreateInputLayout(&element, 1, vs_code->GetBufferPointer(),
                                        vs_code->GetBufferSize(), &layout);
    stage("input-layout-return", -1, result);
    if (FAILED(result)) goto done;
  }
  {
    const float vertices[3][2] = {{-1.0f, -1.0f}, {-1.0f, 3.0f}, {3.0f, -1.0f}};
    D3D10_BUFFER_DESC desc = {};
    desc.ByteWidth = sizeof(vertices);
    desc.Usage = D3D10_USAGE_DEFAULT;
    desc.BindFlags = D3D10_BIND_VERTEX_BUFFER;
    D3D10_SUBRESOURCE_DATA initial = {};
    initial.pSysMem = vertices;
    result = device->CreateBuffer(&desc, &initial, &vertex_buffer);
    stage("vertex-buffer-return", -1, result);
    if (FAILED(result)) goto done;
  }

  {
    const UINT stride = 2 * sizeof(float);
    const UINT offset = 0;
    const D3D10_VIEWPORT viewport = {0, 0, PE_WIDTH, PE_HEIGHT, 0, 1};
    const float background[4] = {0, 0, 0, 1};
    device->IASetInputLayout(layout);
    device->IASetVertexBuffers(0, 1, &vertex_buffer, &stride, &offset);
    device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    device->VSSetShader(vs);
    device->PSSetShader(ps);
    device->RSSetViewports(1, &viewport);
    device->OMSetRenderTargets(1, &view, nullptr);
    for (int frame = 0; frame < 30; ++frame) {
      device->ClearRenderTargetView(view, background);
      stage("draw-enter", frame, S_OK);
      device->Draw(3, 0);
      stage("draw-return", frame, S_OK);
      if (frame == 0) {
        result = read_pixel(device, backbuffer, staging, "TL",
                            PE_WIDTH / 4, PE_HEIGHT / 4, &mismatches);
        if (FAILED(result)) goto done;
        result = read_pixel(device, backbuffer, staging, "TR",
                            PE_WIDTH * 3 / 4, PE_HEIGHT / 4, &mismatches);
        if (FAILED(result)) goto done;
        result = read_pixel(device, backbuffer, staging, "BL",
                            PE_WIDTH / 4, PE_HEIGHT * 3 / 4, &mismatches);
        if (FAILED(result)) goto done;
        result = read_pixel(device, backbuffer, staging, "BR",
                            PE_WIDTH * 3 / 4, PE_HEIGHT * 3 / 4, &mismatches);
        if (FAILED(result) || mismatches) goto done;
      }
      stage("present-enter", frame, S_OK);
      present = swapchain->Present(1, 0);
      stage("present-return", frame, present);
      if (FAILED(present)) goto done;
      ++frames;
      if (frames % 10 == 0) {
        std::printf("DXVK_PE_D3D10_SCANOUT_PROGRESS frames=%d\n", frames);
        std::fflush(stdout);
      }
      pe_visible_frame();
    }
  }

done:
  if (errors && FAILED(result))
    std::printf("DXVK_PE_D3D10_SCANOUT_COMPILE_ERROR %.*s\n",
                int(errors->GetBufferSize()),
                static_cast<const char*>(errors->GetBufferPointer()));
  const bool passed = SUCCEEDED(result) && SUCCEEDED(present) &&
      frames == 30 && !mismatches;
  std::printf("DXVK_PE_D3D10_SCANOUT_RESULT hr=0x%08x frames=%d present=0x%08x mismatches=%u pass=%u\n",
              unsigned(result), frames, unsigned(present),
              mismatches, unsigned(passed));
  std::fflush(stdout);
  release(errors);
  release(ps_code);
  release(vs_code);
  release(vertex_buffer);
  release(layout);
  release(ps);
  release(vs);
  release(staging);
  release(view);
  release(backbuffer);
  release(swapchain);
  release(device);
  DestroyWindow(window);
  return passed ? 0 : 1;
}
