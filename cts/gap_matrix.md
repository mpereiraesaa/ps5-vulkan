# Vulkan API Contract Tests & Upstream CTS Gap Matrix

## 1. Upstream CTS Baseline & Status
- **Upstream Repository:** [KhronosGroup/VK-GL-CTS](https://github.com/KhronosGroup/VK-GL-CTS)
- **Pinned Tag:** `vulkan-cts-1.3.8.4`
- **Pinned Commit:** `a0270c1897597e6c77679870e10415398a13001c`
- **License:** Apache-2.0
- **Upstream Mustpass Source:** `external/vulkancts/mustpass/main/vk-default/*.txt`
- **Porting Status:** **PENDING / IN PROGRESS**. Full upstream Khronos VK-GL-CTS framework execution (building the upstream C++ test runner against ps5vk) is an open milestone.

## 2. Synthetic API Contract Suite Scope & Rationale
To validate driver ABI, descriptor state, memory mapping, compiler interfaces, and queue submission ahead of full upstream CTS porting, we implement a focused set of 26 **synthetic API contract tests** (`contract tests`) named with the `contract.*` prefix and modeled after the Khronos CTS mustpass selection:
1. **Device Initialization & Discovery (12 cases):** Verifies physical device enumeration, limit queries, memory property layout, queue family properties, and extensions.
2. **Core API & Smoke Tests (4 cases):** Verifies device initialization, sampler creation, SPIR-V shader module ingestion, and triangle pipeline setup.
3. **Memory Allocation & Mapping (5 cases):** Exercises small allocations, alignment boundaries, unified memory mapping, and non-coherent cache maintenance (`vkFlushMappedMemoryRanges`, `vkInvalidateMappedMemoryRanges`).
4. **Compute Execution (2 cases):** Exercises SPIR-V runtime compilation and single-invocation SSBO data transformation.
5. **Synchronization & Queue Submission (3 cases):** Exercises signaled/unsignaled fence lifecycle, queue submission, and CPU-GPU synchronization.

### Important Test Limitations
- **Session Device Re-use (Lifecycle):** Restricción del backend/harness actual; reinicialización independiente no validada. The contract suite uses a process-scoped shared session device across tests (`helper_destroy_device` is a no-op across tests). Repeated independent device creation and destruction cycles per test case are **not** validated by this suite.
- **Triangle Pipeline Scope:** `contract.api.smoke.triangle` (ref: `dEQP-VK.api.smoke.triangle`) validates vertex/fragment SPIR-V translation, render pass creation, descriptor layout, and pipeline compilation on GFX1013. It does **not** issue draw commands or verify rasterized pixels. Full draw submission, VideoOut presentation, and deterministic frame buffer readback are verified by the standalone native consumer in `examples/native_consumer/`.

---

## 3. Capability & Coverage Matrix

| Vulkan API / Capability | Advertised API | Native Implementation | Public SDK Export | Contract Case Name | Upstream dEQP-VK Reference | Host Mock Status | Native PS5 Status | Notes / Rationale |
|:---|:---|:---|:---|:---|:---|:---|:---|:---|
| Instance Build Query | Vulkan 1.0 | `src/vk_device.c` | `<ps5vk/ps5vk.h>` | `contract.info.build` | `dEQP-VK.info.build` | PASS | PASS | Static contract dispatch adapter |
| Device Info Query | Vulkan 1.0 | `src/vk_device.c` | `<ps5vk/ps5vk.h>` | `contract.info.device` | `dEQP-VK.info.device` | PASS | PASS | Physical device introspection |
| Platform Identification | Vulkan 1.0 | `src/vk_device.c` | `<ps5vk/ps5vk.h>` | `contract.info.platform` | `dEQP-VK.info.platform` | PASS | PASS | PS5 GFX1013 platform signature |
| Memory Limits & Atom Size | Vulkan 1.0 | `src/vk_device.c`, `native/platform_ps5.c` | `<ps5vk/ps5vk.h>` | `contract.info.memory_limits` | `dEQP-VK.info.memory_limits` | PASS | PASS | `nonCoherentAtomSize = 64`, SSBO offset alignment 256 |
| Device Properties | Vulkan 1.0 | `src/vk_device.c` | `<ps5vk/ps5vk.h>` | `contract.info.device_properties` | `dEQP-VK.info.device_properties` | PASS | PASS | GFX1013 device properties matching driver caps |
| Device Features | Vulkan 1.0 | `src/vk_device.c` | `<ps5vk/ps5vk.h>` | `contract.info.device_features` | `dEQP-VK.info.device_features` | PASS | PASS | Core 1.0 features |
| Device Memory Properties | Vulkan 1.0 | `src/vk_memory.c`, `native/memory_ps5.c` | `<ps5vk/ps5vk.h>` | `contract.info.device_memory_properties` | `dEQP-VK.info.device_memory_properties` | PASS | PASS | Host visible + device local unified memory heap |
| Queue Family Properties | Vulkan 1.0 | `src/vk_device.c` | `<ps5vk/ps5vk.h>` | `contract.info.device_queue_family_properties` | `dEQP-VK.info.device_queue_family_properties` | PASS | PASS | Combined Graphics/Compute queue family |
| Instance Extensions | Vulkan 1.0 | `src/vk_dispatch.c` | `<ps5vk/ps5vk.h>` | `contract.info.instance_extensions` | `dEQP-VK.info.instance_extensions` | PASS | PASS | Static embedded profile |
| Instance Layers | Vulkan 1.0 | `src/vk_dispatch.c` | `<ps5vk/ps5vk.h>` | `contract.info.instance_layers` | `dEQP-VK.info.instance_layers` | PASS | PASS | 0 layers reported |
| Device Extensions | Vulkan 1.0 | `src/vk_dispatch.c` | `<ps5vk/ps5vk.h>` | `contract.info.device_extensions` | `dEQP-VK.info.device_extensions` | PASS | PASS | Static embedded profile |
| Physical Devices Enum | Vulkan 1.0 | `src/vk_device.c` | `<ps5vk/ps5vk.h>` | `contract.info.physical_devices` | `dEQP-VK.info.physical_devices` | PASS | PASS | Physical device enumeration count |
| Logical Device Init | Vulkan 1.0 | `src/vk_device.c` | `<ps5vk/ps5vk.h>` | `contract.api.device_init.create_device` | `dEQP-VK.api.device_init.create_device.basic` | PASS | PASS | Session device init; restricción del backend/harness actual, reinicialización independiente no validada |
| Sampler Object Creation | Vulkan 1.0 | `src/vk_sampler.c` | `<ps5vk/ps5vk.h>` | `contract.api.smoke.create_sampler` | `dEQP-VK.api.smoke.create_sampler` | NotSupported | PASS | Host mock lacks graphics pipeline; native validates GFX10 S# descriptor |
| Shader Module Ingestion | Vulkan 1.0 | `src/compilation_cache.c` | `<ps5vk/ps5vk.h>` | `contract.api.smoke.create_shader` | `dEQP-VK.api.smoke.create_shader` | PASS | PASS | Validates SPIR-V binary validation and parsing |
| Graphics Triangle Pipeline | Vulkan 1.0 | `native/graphics_pipeline_ps5.c`, `src/graphics_program.c` | `<ps5vk/ps5vk.h>` | `contract.api.smoke.triangle` | `dEQP-VK.api.smoke.triangle` | NotSupported | PASS | Pipeline compilation only; draw/raster verified in native consumer |
| Memory Allocation (256B) | Vulkan 1.0 | `src/vk_memory.c`, `native/memory_ps5.c` | `<ps5vk/ps5vk.h>` | `contract.memory.allocation.size_256` | `dEQP-VK.memory.allocation.basic.size_256.forward.count_1` | PASS | PASS | Sub-page allocation with 256B alignment |
| Memory Allocation (1KiB) | Vulkan 1.0 | `src/vk_memory.c`, `native/memory_ps5.c` | `<ps5vk/ps5vk.h>` | `contract.memory.allocation.size_1KiB` | `dEQP-VK.memory.allocation.basic.size_1KiB.forward.count_1` | PASS | PASS | 1024B aligned device memory allocation |
| Mapped Memory Read/Write | Vulkan 1.0 | `src/vk_memory.c` | `<ps5vk/ps5vk.h>` | `contract.memory.mapping.suballocation_257_simple` | `dEQP-VK.memory.mapping.suballocation.full.257.simple` | PASS | PASS | Unified memory direct pointer mapping |
| Non-Coherent Flush | Vulkan 1.0 | `src/vk_memory.c`, `native/memory_ps5.c` | `<ps5vk/ps5vk.h>` | `contract.memory.mapping.suballocation_257_flush` | `dEQP-VK.memory.mapping.suballocation.full.257.flush` | PASS | PASS | Strict 64-byte atom range rounding and validation |
| Non-Coherent Invalidate | Vulkan 1.0 | `src/vk_memory.c`, `native/memory_ps5.c` | `<ps5vk/ps5vk.h>` | `contract.memory.mapping.suballocation_257_invalidate` | `dEQP-VK.memory.mapping.suballocation.full.257.invalidate` | PASS | PASS | Strict 64-byte atom cache invalidation |
| Single SSBO Compute | Vulkan 1.0 | `src/compute_commands.c`, `native/queue_ps5.c` | `<ps5vk/ps5vk.h>` | `contract.compute.pipeline.copy_ssbo_single_invocation` | `dEQP-VK.compute.pipeline.basic.copy_ssbo_single_invocation` | NotSupported | PASS | Host mock lacks ACO/PSBC shader execution; native compiles & runs |
| Empty Compute Pipeline | Vulkan 1.0 | `src/vk_pipeline.c`, `native/queue_ps5.c` | `<ps5vk/ps5vk.h>` | `contract.compute.pipeline.empty_shader` | `dEQP-VK.compute.pipeline.basic.empty_shader` | NotSupported | PASS | Validates no-op kernel dispatch |
| Unsignaled Fence Lifecycle | Vulkan 1.0 | `src/vk_fence.c` | `<ps5vk/ps5vk.h>` | `contract.synchronization.fence_unsignaled` | `dEQP-VK.synchronization.basic.fence.one` | PASS | PASS | Initial unsignaled state, reset, destroy |
| Signaled Fence Lifecycle | Vulkan 1.0 | `src/vk_fence.c` | `<ps5vk/ps5vk.h>` | `contract.synchronization.fence_signaled` | `dEQP-VK.synchronization.basic.fence.one_signaled` | PASS | PASS | `VK_FENCE_CREATE_SIGNALED_BIT` query and reset |
| Empty Queue Submit | Vulkan 1.0 | `src/vk_queue.c`, `native/queue_ps5.c` | `<ps5vk/ps5vk.h>` | `contract.synchronization.empty_submit` | `dEQP-VK.synchronization.basic.empty_submit` | PASS | PASS | Fence signal on null submission |

---

## 4. Status Summary
- **Synthetic Contract Suite:** 26 test cases with dedicated `contract.*` names, retaining dEQP-VK IDs as upstream references.
- **Native PS5 Execution:** 26/26 PASS within the session device context.
- **Host Mock Execution:** 22 PASS, 4 NotSupported (GPU shader and graphics pipeline compilation unported on host mock harness), 0 FAIL.
- **Upstream CTS Status:** Pending. Full upstream Khronos VK-GL-CTS C++ framework execution is an open milestone.
