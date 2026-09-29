// SPDX-License-Identifier: MIT
// DXVK 2.6.2 PE control: four visible 1080p quadrants for bounded TV inspection.
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
  std::printf("DXVK_PE_D3D11_SCANOUT_STAGE name=%s frame=%d hr=0x%08x\n",
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
  std::printf("DXVK_PE_D3D11_SCANOUT_PIXEL quadrant=%s bgra=%02x%02x%02x%02x mismatches=%u\n",
              place, unsigned(bgra[0]), unsigned(bgra[1]),
              unsigned(bgra[2]), unsigned(bgra[3]), mismatches);
  std::fflush(stdout);
  return mismatches;
}

static HRESULT read_pixel(ID3D11DeviceContext* context, ID3D11Texture2D* backbuffer,
                          ID3D11Texture2D* staging, const char* place,
                          UINT x, UINT y, unsigned* mismatches) {
  const D3D11_BOX box = {x, y, 0, x + 1, y + 1, 1};
  context->CopySubresourceRegion(staging, 0, 0, 0, 0, backbuffer, 0, &box);
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  const HRESULT hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
  stage("pixel-map-return", 0, hr);
  if (SUCCEEDED(hr)) {
    *mismatches += check_pixel(place, mapped.pData);
    context->Unmap(staging, 0);
  }
  return hr;
}

int main() {
  HWND window = pe_window(L"DXVK D3D11 scanout / red green blue yellow");
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
  HRESULT present = E_FAIL;
  int frames = 0;
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
        "float4 ps_main(VOut input) : SV_Target {"
        "float2 p = input.pos.xy;"
        "if (p.x < 960) { if (p.y < 540) return float4(1,0,0,1); return float4(0,0,1,1); }"
        "if (p.y < 540) return float4(0,1,0,1); return float4(1,1,0,1);"
        "}";
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
    const float vertices[3][2] = {{-1.0f, -1.0f}, {-1.0f, 3.0f}, {3.0f, -1.0f}};
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
    context->IASetInputLayout(layout);
    context->IASetVertexBuffers(0, 1, &vertex_buffer, &stride, &offset);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs, nullptr, 0);
    context->PSSetShader(ps, nullptr, 0);
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &view, nullptr);
    const float background[4] = {0, 0, 0, 1};
    for (int frame = 0; frame < 30; ++frame) {
      context->ClearRenderTargetView(view, background);
      context->Draw(3, 0);
      if (frame == 0) {
        result = read_pixel(context, backbuffer, staging, "TL",
                            PE_WIDTH / 4, PE_HEIGHT / 4, &mismatches);
        if (FAILED(result)) goto done;
        result = read_pixel(context, backbuffer, staging, "TR",
                            PE_WIDTH * 3 / 4, PE_HEIGHT / 4, &mismatches);
        if (FAILED(result)) goto done;
        result = read_pixel(context, backbuffer, staging, "BL",
                            PE_WIDTH / 4, PE_HEIGHT * 3 / 4, &mismatches);
        if (FAILED(result)) goto done;
        result = read_pixel(context, backbuffer, staging, "BR",
                            PE_WIDTH * 3 / 4, PE_HEIGHT * 3 / 4, &mismatches);
        if (FAILED(result) || mismatches) goto done;
      }
      present = swapchain->Present(1, 0);
      if (FAILED(present)) {
        stage("present-return", frame, present);
        goto done;
      }
      ++frames;
      if ((frame + 1) % 10 == 0) {
        std::printf("DXVK_PE_D3D11_SCANOUT_PROGRESS frames=%d\n", frames);
        std::fflush(stdout);
      }
      pe_visible_frame();
      Sleep(250);
    }
  }

done:
  if (errors && FAILED(result))
    std::printf("DXVK_PE_D3D11_SCANOUT_COMPILE_ERROR %.*s\n",
                int(errors->GetBufferSize()),
                static_cast<const char*>(errors->GetBufferPointer()));
  const bool passed = SUCCEEDED(result) && SUCCEEDED(present) &&
      !mismatches && frames == 30 && selected == D3D_FEATURE_LEVEL_11_0;
  std::printf("DXVK_PE_D3D11_SCANOUT_RESULT hr=0x%08x level=0x%04x frames=%d present=0x%08x mismatches=%u pass=%u\n",
              unsigned(result), unsigned(selected), frames,
              unsigned(present), mismatches, unsigned(passed));
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
