# ps5-vulkan

An experimental, hardware-accelerated Vulkan-style graphics and compute API for
native PlayStation 5 homebrew on the console's `gfx1013` GPU. It is built and
tested on an owned PS5; the public device currently reports Vulkan 1.0. The
project documents supported operations individually rather than claiming a
complete Vulkan core version.

## Working capabilities

- Native graphics and compute submission, GPU-backed buffers and images,
  command buffers, fences, binary semaphores, and two-buffer 1080p VideoOut
  presentation with clean Close Game/relaunch.
- Runtime SPIR-V compute and vertex/fragment compilation through pinned
  PSBC/ACO, with bounded in-memory shader and pipeline caches. Compute has up
  to four descriptor sets, storage/uniform buffers, uniform texel buffers,
  push constants, specialization constants, and extension-negotiated 8/16-bit
  storage-buffer access.
- Indexed and indirect draws, typed vertex formats, depth testing, face
  culling, multiview, geometry and tessellation stages, and clip/cull distance
  export. The graphics backend also serves bounded non-solid fill, depth
  clamp/bias and multiple viewports.
- One or two BGRA8/RGBA8 colour attachments, independent and dual-source
  blending, fragment storage writes/atomics, and 2x/4x colour multisampling
  with per-sample shading, input-attachment reads and resolve.
- Sampled 1D, 2D, array, cube, cube-array and 3D images, including bounded BC
  compressed formats, mip uploads, depth sampling and extended image gather.
  The format ledger records 61 sampled texture formats: 40 filterable rows and
  20 integer rows restricted to nearest filtering.
  Selected image copy, blit, clear and readback paths have exact GPU oracles;
  unsupported resource shapes remain fail-closed.
- Occlusion queries, including precise counts, and selected Vulkan memory-model,
  standard-UBO-layout and buffer-device-address behavior through explicit KHR
  extension routes. The public API and native evidence are narrower than the
  corresponding complete core-version contracts.

These capabilities were validated through public-SDK consumers, structured
`ps5log/1` telemetry and deterministic GPU readback; visual output alone is
not the oracle. The T07 resource/query expansion is merged and its ordinary
upstream selection passed 829/829 cases on hardware. See [API.md](API.md) for
the bounded contract, [VALIDATION.md](VALIDATION.md) for exact evidence and
[BUILDING.md](BUILDING.md) to build the SDK.

## In progress

Current work adds imageless framebuffers, mirror-clamp sampling, timeline
semaphores and separate depth/stencil layouts. `VK_EXT_host_query_reset` is
public on Vulkan 1.0 with a strict native reset/reuse witness. Default-off
builds have a native witness for bounded imageless framebuffer use; eight U/V mirror-clamp
draws passed, while W sampling remains unverified after a 3D upload failure in
focused CTS. The checked DXVK 2.6.2 matrix currently has **26/62** requirements
satisfied, and a real DXVK build has not yet run on PS5. See
[the T09 measurements](VALIDATION.md#t09-host-query-reset-public-ext-promotion-2026-09-25).
Additional synchronization and descriptor capabilities will follow as needed
by real consumers.

There is no Vulkan loader/ICD or Vulkan WSI/swapchain implementation. The
native VideoOut path is separate from WSI. Format, shader, queue and resource
coverage is intentionally bounded; [API.md](API.md) records those limits.
Focused upstream CTS results are available for debugging and regression in
[UPSTREAM_CTS.md](UPSTREAM_CTS.md), but no full CTS or Vulkan conformance claim
is made.

## Development

Host validation requires Python 3, Make, a C11 compiler and Git:

```sh
make vulkan-headers
make compiler-deps
make check
make check-sanitize
```

Native compilation also requires the PS5 payload SDK and the companion
`ps5-agc-gears` support library; see [BUILDING.md](BUILDING.md).

## License

`ps5-vulkan` is free software licensed under the GNU General Public License,
version 3 or (at your option) any later version (`GPL-3.0-or-later`). See
[LICENSE](LICENSE) for the complete terms and [LICENSING.md](LICENSING.md) for
copyright, contribution, dependency and source-provenance details.

The staged SDK contains a static `libps5vk.a`. Distributing an application that
links this library creates a combined GPL work and requires providing its
corresponding source under GPL-compatible terms. Console system modules and
their link-time import facades are not distributed as part of this repository.
