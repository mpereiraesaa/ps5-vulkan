# Vulkan API Contract Tests & Upstream CTS Gap Matrix

## 1. Upstream CTS Baseline & Status
- **Upstream Repository:** [KhronosGroup/VK-GL-CTS](https://github.com/KhronosGroup/VK-GL-CTS)
- **Pinned Tag:** `vulkan-cts-1.3.8.4`
- **Pinned Commit:** `a0270c1897597e6c77679870e10415398a13001c`
- **License:** Apache-2.0
- **Upstream Mustpass Source:** `external/vulkancts/mustpass/main/vk-default/*.txt`
- **Porting Status:** **PENDING / IN PROGRESS**. Full upstream Khronos VK-GL-CTS framework execution (building the upstream C++ test runner against ps5vk) is an open milestone.

## 2. Synthetic API Contract Suite Scope & Rationale
To validate driver ABI, descriptor state, memory mapping, compiler interfaces, and queue submission ahead of full upstream CTS porting, we implement a focused set of 26 **synthetic API contract tests** (`contract tests`) modeled after the Khronos CTS mustpass selection:
1. **Device Initialization & Discovery (12 cases):** Verifies physical device enumeration, limit queries, memory property layout, queue family properties, and extensions.
2. **Core API & Smoke Tests (4 cases):** Verifies device initialization, sampler creation, SPIR-V shader module ingestion, and triangle pipeline setup.
3. **Memory Allocation & Mapping (5 cases):** Exercises small allocations, alignment boundaries, unified memory mapping, and non-coherent cache maintenance (`vkFlushMappedMemoryRanges`, `vkInvalidateMappedMemoryRanges`).
4. **Compute Execution (2 cases):** Exercises SPIR-V runtime compilation and single-invocation SSBO data transformation.
5. **Synchronization & Queue Submission (3 cases):** Exercises signaled/unsignaled fence lifecycle, queue submission, and CPU-GPU synchronization.

### Important Test Limitations
- **Session Device Re-use (Lifecycle):** Due to PS5 AGC driver reinitialization limitations within a single process, the contract suite uses a process-scoped shared session device across tests (`helper_destroy_device` is a no-op across tests). Repeated independent device creation and destruction cycles per test case are **not** validated by this suite.
- **Triangle Pipeline Scope:** `dEQP-VK.api.smoke.triangle` validates vertex/fragment SPIR-V translation, render pass creation, descriptor layout, and pipeline compilation on GFX1013. It does **not** issue draw commands or verify rasterized pixels. Full draw submission, VideoOut presentation, and deterministic frame buffer readback are verified by the standalone native consumer in `examples/native_consumer/`.

---

## 3. Capability & Coverage Matrix

| Vulkan API / Capability | Advertised API | Native Implementation | Public SDK Export | Modeled Contract Case | Host Mock Status | Native PS5 Status | Notes / Hardware Rationale |
|:---|:---|:---|:---|:---|:---|:---|:---|
| Instance & Device Enumeration | Vulkan 1.0 | `src/vk_device.c`, `src/vk_dispatch.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.info.build`<br>`dEQP-VK.info.device`<br>`dEQP-VK.info.platform`<br>`dEQP-VK.info.physical_devices` | PASS | PASS | Static dispatch adapter via `vkGetInstanceProcAddr` |
| Device Properties & Features | Vulkan 1.0 | `src/vk_device.c`, `native/platform_ps5.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.info.device_properties`<br>`dEQP-VK.info.device_features` | PASS | PASS | GFX1013 profile, driver version 1 |
| Device Memory Properties | Vulkan 1.0 | `src/vk_memory.c`, `native/memory_ps5.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.info.device_memory_properties` | PASS | PASS | Host visible + device local unified memory heap |
| Memory Limits & Atom Size | Vulkan 1.0 | `src/vk_device.c`, `native/platform_ps5.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.info.memory_limits` | PASS | PASS | `nonCoherentAtomSize = 64`, SSBO offset alignment 256 |
| Queue Family Properties | Vulkan 1.0 | `src/vk_device.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.info.device_queue_family_properties` | PASS | PASS | Combined Graphics/Compute queue family |
| Extension & Layer Queries | Vulkan 1.0 | `src/vk_dispatch.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.info.instance_extensions`<br>`dEQP-VK.info.instance_layers`<br>`dEQP-VK.info.device_extensions` | PASS | PASS | Static embedded profile, 0 layers |
| Logical Device Initialization | Vulkan 1.0 | `src/vk_device.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.api.device_init.create_device.basic` | PASS | PASS | Validates session device init; per-test teardown is restricted by AGC driver |
| Sampler Object Creation | Vulkan 1.0 | `src/vk_sampler.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.api.smoke.create_sampler` | NotSupported | PASS | Host mock lacks graphics pipeline; native validates GFX10 S# descriptor |
| Shader Module Ingestion | Vulkan 1.0 | `src/compilation_cache.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.api.smoke.create_shader` | PASS | PASS | Validates SPIR-V binary validation and parsing |
| Graphics Triangle Pipeline | Vulkan 1.0 | `native/graphics_pipeline_ps5.c`, `src/graphics_program.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.api.smoke.triangle` | NotSupported | PASS | Pipeline compilation only; draw/raster verified in native consumer |
| Memory Allocation (256B) | Vulkan 1.0 | `src/vk_memory.c`, `native/memory_ps5.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.memory.allocation.basic.size_256.forward.count_1` | PASS | PASS | Sub-page allocation with 256B alignment |
| Memory Allocation (1KiB) | Vulkan 1.0 | `src/vk_memory.c`, `native/memory_ps5.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.memory.allocation.basic.size_1KiB.forward.count_1` | PASS | PASS | 1024B aligned device memory allocation |
| Mapped Memory Read/Write | Vulkan 1.0 | `src/vk_memory.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.memory.mapping.suballocation.full.257.simple` | PASS | PASS | Unified memory direct pointer mapping |
| Non-Coherent Flush | Vulkan 1.0 | `src/vk_memory.c`, `native/memory_ps5.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.memory.mapping.suballocation.full.257.flush` | PASS | PASS | Strict 64-byte atom range rounding and validation |
| Non-Coherent Invalidate | Vulkan 1.0 | `src/vk_memory.c`, `native/memory_ps5.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.memory.mapping.suballocation.full.257.invalidate` | PASS | PASS | Strict 64-byte atom cache invalidation |
| Single SSBO Compute Dispatch | Vulkan 1.0 | `src/compute_commands.c`, `native/queue_ps5.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.compute.pipeline.basic.copy_ssbo_single_invocation` | NotSupported | PASS | Host mock lacks ACO/PSBC shader execution; native compiles & runs |
| Empty Compute Pipeline | Vulkan 1.0 | `src/vk_pipeline.c`, `native/queue_ps5.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.compute.pipeline.basic.empty_shader` | NotSupported | PASS | Validates no-op kernel dispatch |
| Unsignaled Fence Lifecycle | Vulkan 1.0 | `src/vk_fence.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.synchronization.basic.fence.one` | PASS | PASS | Initial unsignaled state, reset, destroy |
| Signaled Fence Lifecycle | Vulkan 1.0 | `src/vk_fence.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.synchronization.basic.fence.one_signaled` | PASS | PASS | `VK_FENCE_CREATE_SIGNALED_BIT` query and reset |
| Empty Queue Submit | Vulkan 1.0 | `src/vk_queue.c`, `native/queue_ps5.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.synchronization.basic.empty_submit` | PASS | PASS | Fence signal on null submission |

---

## 4. Status Summary
- **Synthetic Contract Suite:** 26 test cases modeled after the Khronos CTS selection.
- **Native PS5 Execution:** 26/26 PASS within the session device context.
- **Host Mock Execution:** 22 PASS, 4 NotSupported (GPU shader and graphics pipeline compilation unported on host mock harness), 0 FAIL.
- **Upstream CTS Status:** Pending. Full upstream Khronos VK-GL-CTS C++ framework execution is an open milestone.
