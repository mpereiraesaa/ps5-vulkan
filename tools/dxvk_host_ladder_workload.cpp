// Minimal offscreen D3D11 workload for tools/run_dxvk_ps5vk_host_ladder.py.
//
// It uses only DXVK's native D3D11 headers: a feature-level 11_0 device, a
// 64x64 R8G8B8A8_UNORM render target and a STAGING copy, a render-target
// view, a DXBC vertex shader that builds a full-screen triangle from
// SV_VertexID and a constant-colour pixel shader, one clear, Draw(3),
// CopyResource, Map(READ) of the staging copy and a pixel check, then every
// release. No swapchain is created. The tool supplies the DXBC blobs in
// ladder_shaders.h. Each step prints one LADDER_STEP line; the first failing
// step ends the run so that a refusal is attributed to exactly one call.
#include <d3d11.h>
#include <SDL2/SDL.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "ladder_shaders.h"

namespace {

bool step(const char* name, HRESULT hr) {
  std::printf("LADDER_STEP %s hr=0x%08x\n", name, static_cast<unsigned>(hr));
  std::fflush(stdout);
  return SUCCEEDED(hr);
}

void note(const char* name) {
  std::printf("LADDER_STEP %s hr=0x00000000\n", name);
  std::fflush(stdout);
}

template <typename T> void release(T*& object) {
  if (object) object->Release();
  object = nullptr;
}

}  // namespace

int main() {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::fprintf(stderr, "SDL video initialization failed: %s\n", SDL_GetError());
    return 2;
  }
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  ID3D11Texture2D* target = nullptr;
  ID3D11Texture2D* staging = nullptr;
  ID3D11RenderTargetView* view = nullptr;
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  int status = 1;
  do {
    D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_11_0;
    D3D_FEATURE_LEVEL selected = static_cast<D3D_FEATURE_LEVEL>(0);
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        &requested, 1, D3D11_SDK_VERSION, &device, &selected, &context);
    std::printf("LADDER_FEATURE_LEVEL 0x%04x\n", static_cast<unsigned>(selected));
    if (!step("D3D11CreateDevice", hr) || !device || !context) break;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 64;
    desc.Height = 64;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (!step("CreateTexture2D.render_target", device->CreateTexture2D(&desc, nullptr, &target)))
      break;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (!step("CreateTexture2D.staging", device->CreateTexture2D(&desc, nullptr, &staging)))
      break;
    if (!step("CreateRenderTargetView", device->CreateRenderTargetView(target, nullptr, &view)))
      break;
    if (!step("CreateVertexShader", device->CreateVertexShader(
            kLadderVertexShader, sizeof(kLadderVertexShader), nullptr, &vs)))
      break;
    if (!step("CreatePixelShader", device->CreatePixelShader(
            kLadderPixelShader, sizeof(kLadderPixelShader), nullptr, &ps)))
      break;

    const FLOAT clear[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    context->ClearRenderTargetView(view, clear);
    note("ClearRenderTargetView");
    D3D11_VIEWPORT viewport = {0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f};
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs, nullptr, 0);
    context->PSSetShader(ps, nullptr, 0);
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &view, nullptr);
    context->Draw(3, 0);
    note("Draw");
    context->CopyResource(staging, target);
    note("CopyResource");

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (!step("Map.read", context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) break;
    const auto* row = static_cast<const uint8_t*>(mapped.pData) + 32 * mapped.RowPitch;
    const uint8_t* pixel = row + 32 * 4;
    std::printf("LADDER_PIXEL x=32 y=32 rgba=%u,%u,%u,%u row_pitch=%u\n", pixel[0], pixel[1],
        pixel[2], pixel[3], mapped.RowPitch);
    context->Unmap(staging, 0);
    note("Unmap");
    status = 0;
  } while (false);

  release(ps);
  release(vs);
  release(view);
  release(staging);
  release(target);
  if (context) {
    context->ClearState();
    context->Flush();
  }
  release(context);
  ULONG remaining = device ? device->Release() : 0;
  device = nullptr;
  std::printf("LADDER_RELEASE device_refs=%lu\n", static_cast<unsigned long>(remaining));
  SDL_Quit();
  std::printf("LADDER_EXIT status=%d\n", status);
  return status;
}
