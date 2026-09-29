// SPDX-License-Identifier: MIT
// DXVK 2.6.2 PE control: a real D3D11 triangle, read back before two Presents.
#include "window.h"
#include <d3d11.h>
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
  std::printf("DXVK_PE_D3D11_DRAW_STAGE name=%s frame=%d hr=0x%08x\n",
              name, frame, unsigned(hr));
  std::fflush(stdout);
}

static unsigned check_pixel(const char* place, int frame, const void* data) {
  const auto* bgra = static_cast<const std::uint8_t*>(data);
  const bool centre = std::strcmp(place, "centre") == 0;
  const std::uint8_t expected[4] = {
      static_cast<std::uint8_t>(centre ? 0 : frame ? 28 : 132),
      static_cast<std::uint8_t>(centre ? 0 : 76),
      static_cast<std::uint8_t>(centre ? 255 : frame ? 132 : 28),
      255};
  const unsigned mismatches = unsigned(bgra[0] != expected[0]) +
      unsigned(bgra[1] != expected[1]) + unsigned(bgra[2] != expected[2]) +
      unsigned(bgra[3] != expected[3]);
  std::printf("DXVK_PE_D3D11_DRAW_PIXEL frame=%d place=%s bgra=%02x%02x%02x%02x mismatches=%u\n",
              frame, place, unsigned(bgra[0]), unsigned(bgra[1]),
              unsigned(bgra[2]), unsigned(bgra[3]), mismatches);
  std::fflush(stdout);
  return mismatches;
}

static HRESULT read_pixel(ID3D11DeviceContext* context, ID3D11Texture2D* backbuffer,
                          ID3D11Texture2D* staging, int frame, const char* place,
                          UINT x, UINT y, unsigned* mismatches) {
  const D3D11_BOX box = {x, y, 0, x + 1, y + 1, 1};
  const bool centre = std::strcmp(place, "centre") == 0;
  stage(centre ? "centre-copy-enter" : "corner-copy-enter", frame, S_OK);
  context->CopySubresourceRegion(staging, 0, 0, 0, 0, backbuffer, 0, &box);
  stage(centre ? "centre-copy-return" : "corner-copy-return", frame, S_OK);
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  stage(centre ? "centre-map-enter" : "corner-map-enter", frame, S_OK);
  const HRESULT hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
  stage(centre ? "centre-map-return" : "corner-map-return", frame, hr);
  if (SUCCEEDED(hr)) {
    *mismatches += check_pixel(place, frame, mapped.pData);
    context->Unmap(staging, 0);
  }
  return hr;
}

int main() {
  HWND window = pe_window(L"DXVK 2.6.2 D3D11 draw / Prospero Win");
  if (!window) return 2;
  IDXGIFactory* factory = nullptr;
  IDXGIAdapter* adapter = nullptr;
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  IDXGISwapChain* swapchain = nullptr;
  ID3D11Texture2D* backbuffer = nullptr;
  ID3D11RenderTargetView* view = nullptr;
  ID3D11Texture2D* staging = nullptr;
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11InputLayout* layout = nullptr;
  ID3D11Buffer* vertex_buffer = nullptr;
  ID3DBlob* vs_code = nullptr;
  ID3DBlob* ps_code = nullptr;
  ID3DBlob* errors = nullptr;
  HRESULT result = E_FAIL;
  HRESULT present[2] = {E_FAIL, E_FAIL};
  unsigned mismatches = 0;
  D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_11_0;
  D3D_FEATURE_LEVEL selected = static_cast<D3D_FEATURE_LEVEL>(0);

  stage("factory-enter", -1, S_OK);
  result = CreateDXGIFactory(__uuidof(IDXGIFactory),
                             reinterpret_cast<void**>(&factory));
  stage("factory-return", -1, result);
  if (FAILED(result)) goto done;
  result = factory->EnumAdapters(0, &adapter);
  stage("adapter-return", -1, result);
  if (FAILED(result)) goto done;
  stage("device-enter", -1, S_OK);
  result = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                             &requested, 1, D3D11_SDK_VERSION,
                             &device, &selected, &context);
  stage("device-return", -1, result);
  if (FAILED(result) || selected != D3D_FEATURE_LEVEL_11_0) goto done;

  {
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
    stage("swapchain-enter", -1, S_OK);
    result = factory->CreateSwapChain(device, &desc, &swapchain);
    stage("swapchain-return", -1, result);
    if (FAILED(result)) goto done;
  }
  result = swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                reinterpret_cast<void**>(&backbuffer));
  stage("backbuffer-return", -1, result);
  if (FAILED(result)) goto done;
  result = device->CreateRenderTargetView(backbuffer, nullptr, &view);
  stage("view-return", -1, result);
  if (FAILED(result)) goto done;

  {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    result = device->CreateTexture2D(&desc, nullptr, &staging);
    stage("staging-return", -1, result);
    if (FAILED(result)) goto done;
  }

  {
    const char shader[] =
        "struct VIn { float2 pos : POSITION; };"
        "struct VOut { float4 pos : SV_Position; };"
        "VOut vs_main(VIn input) { VOut o; o.pos = float4(input.pos, 0, 1); return o; }"
        "float4 ps_main(VOut input) : SV_Target { return float4(1, 0, 0, 1); }";
    stage("vs-compile-enter", -1, S_OK);
    result = D3DCompile(shader, sizeof(shader) - 1, "draw-control", nullptr,
                        nullptr, "vs_main", "vs_5_0", 0, 0, &vs_code, &errors);
    stage("vs-compile-return", -1, result);
    if (FAILED(result)) goto done;
    release(errors);
    stage("ps-compile-enter", -1, S_OK);
    result = D3DCompile(shader, sizeof(shader) - 1, "draw-control", nullptr,
                        nullptr, "ps_main", "ps_5_0", 0, 0, &ps_code, &errors);
    stage("ps-compile-return", -1, result);
    if (FAILED(result)) goto done;
  }
  result = device->CreateVertexShader(vs_code->GetBufferPointer(),
                                       vs_code->GetBufferSize(), nullptr, &vs);
  stage("vertex-shader-return", -1, result);
  if (FAILED(result)) goto done;
  result = device->CreatePixelShader(ps_code->GetBufferPointer(),
                                      ps_code->GetBufferSize(), nullptr, &ps);
  stage("pixel-shader-return", -1, result);
  if (FAILED(result)) goto done;
  {
    const D3D11_INPUT_ELEMENT_DESC element =
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
         D3D11_INPUT_PER_VERTEX_DATA, 0};
    result = device->CreateInputLayout(&element, 1, vs_code->GetBufferPointer(),
                                        vs_code->GetBufferSize(), &layout);
    stage("input-layout-return", -1, result);
    if (FAILED(result)) goto done;
  }
  {
    const float vertices[3][2] = {{-0.6f, -0.6f}, {0.0f, 0.6f}, {0.6f, -0.6f}};
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = sizeof(vertices);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA initial = {};
    initial.pSysMem = vertices;
    result = device->CreateBuffer(&desc, &initial, &vertex_buffer);
    stage("vertex-buffer-return", -1, result);
    if (FAILED(result)) goto done;
  }

  {
    const UINT stride = 2 * sizeof(float);
    const UINT offset = 0;
    const D3D11_VIEWPORT viewport = {0, 0, float(PE_WIDTH), float(PE_HEIGHT), 0, 1};
    const float backgrounds[2][4] = {
        {28.0f / 255, 76.0f / 255, 132.0f / 255, 1},
        {132.0f / 255, 76.0f / 255, 28.0f / 255, 1}};
    context->IASetInputLayout(layout);
    context->IASetVertexBuffers(0, 1, &vertex_buffer, &stride, &offset);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs, nullptr, 0);
    context->PSSetShader(ps, nullptr, 0);
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &view, nullptr);
    for (int frame = 0; frame < 2; ++frame) {
      context->ClearRenderTargetView(view, backgrounds[frame]);
      stage("draw-enter", frame, S_OK);
      context->Draw(3, 0);
      stage("draw-return", frame, S_OK);
      result = read_pixel(context, backbuffer, staging, frame, "centre",
                          PE_WIDTH / 2, PE_HEIGHT / 2, &mismatches);
      if (FAILED(result)) goto done;
      result = read_pixel(context, backbuffer, staging, frame, "corner",
                          16, 16, &mismatches);
      if (FAILED(result)) goto done;
      stage("present-enter", frame, S_OK);
      present[frame] = swapchain->Present(1, 0);
      stage("present-return", frame, present[frame]);
      if (FAILED(present[frame])) goto done;
      pe_visible_frame();
    }
  }

done:
  if (errors && FAILED(result))
    std::printf("DXVK_PE_D3D11_DRAW_COMPILE_ERROR %.*s\n",
                int(errors->GetBufferSize()),
                static_cast<const char*>(errors->GetBufferPointer()));
  const bool passed = SUCCEEDED(result) && SUCCEEDED(present[0]) &&
      SUCCEEDED(present[1]) && !mismatches &&
      selected == D3D_FEATURE_LEVEL_11_0;
  std::printf("DXVK_PE_D3D11_DRAW_RESULT hr=0x%08x level=0x%04x present0=0x%08x present1=0x%08x mismatches=%u pass=%u\n",
              unsigned(result), unsigned(selected), unsigned(present[0]),
              unsigned(present[1]), mismatches, unsigned(passed));
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
  release(context);
  release(device);
  release(adapter);
  release(factory);
  DestroyWindow(window);
  return passed ? 0 : 1;
}
