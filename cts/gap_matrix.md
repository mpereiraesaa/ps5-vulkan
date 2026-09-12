# Vulkan CTS Gap Matrix & Coverage Analysis

## 1. Upstream CTS Baseline
- **Upstream Repository:** [KhronosGroup/VK-GL-CTS](https://github.com/KhronosGroup/VK-GL-CTS)
- **Pinned Tag:** `vulkan-cts-1.3.8.4`
- **Pinned Commit:** `a0270c1897597e6c77679870e10415398a13001c`
- **License:** Apache-2.0
- **Upstream Mustpass Source:** `external/vulkancts/mustpass/main/vk-default/*.txt`

## 2. Selection Criteria & Rationale
Bare-metal PlayStation 5 runs in a static, embedded environment without dynamic linkers (`dlopen`/`dlsym`), POSIX fork/exec, or filesystem-based shader test runners. Full upstream CTS depends on CMake, Python code generators, Amber, and SPIRV-Tools.
To provide reproducible, honest conformance evidence, we select a focused subset of 26 mustpass test cases representing the fundamental core of the Vulkan 1.0 specification:
1. **Device Initialization & Discovery (12 cases):** Verifies physical device enumeration, limit queries, memory property layout, queue family properties, and extensions.
2. **Core API & Smoke Tests (4 cases):** Verifies device creation/destruction, sampler creation, SPIR-V shader module ingestion, and triangle pipeline setup.
3. **Memory Allocation & Mapping (5 cases):** Exercises small allocations, alignment boundaries, unified memory mapping, and non-coherent cache maintenance (`vkFlushMappedMemoryRanges`, `vkInvalidateMappedMemoryRanges`).
4. **Compute Execution (2 cases):** Exercises SPIR-V runtime compilation and single-invocation SSBO data transformation.
5. **Synchronization & Queue Submission (3 cases):** Exercises signaled/unsignaled fence lifecycle, queue submission, and CPU-GPU synchronization.

---

## 3. Capability & Coverage Matrix

| Vulkan API / Capability | Advertised API | Native Implementation | Public SDK Export | Focused CTS Coverage | Host Mock Status | Native PS5 Status | Notes / Hardware Rationale |
|:---|:---|:---|:---|:---|:---|:---|:---|
| Instance & Device Enumeration | Vulkan 1.0 | `src/vk_device.c`, `src/vk_dispatch.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.info.build`<br>`dEQP-VK.info.device`<br>`dEQP-VK.info.platform`<br>`dEQP-VK.info.physical_devices` | PASS | PASS | Static dispatch adapter via `vkGetInstanceProcAddr` |
| Device Properties & Features | Vulkan 1.0 | `src/vk_device.c`, `native/platform_ps5.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.info.device_properties`<br>`dEQP-VK.info.device_features` | PASS | PASS | GFX1013 profile, driver version 1 |
| Device Memory Properties | Vulkan 1.0 | `src/vk_memory.c`, `native/memory_ps5.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.info.device_memory_properties` | PASS | PASS | Host visible + device local unified memory heap |
| Memory Limits & Atom Size | Vulkan 1.0 | `src/vk_device.c`, `native/platform_ps5.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.info.memory_limits` | PASS | PASS | `nonCoherentAtomSize = 64`, SSBO offset alignment 256 |
| Queue Family Properties | Vulkan 1.0 | `src/vk_device.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.info.device_queue_family_properties` | PASS | PASS | Combined Graphics/Compute queue family |
| Extension & Layer Queries | Vulkan 1.0 | `src/vk_dispatch.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.info.instance_extensions`<br>`dEQP-VK.info.instance_layers`<br>`dEQP-VK.info.device_extensions` | PASS | PASS | Static embedded profile, 0 layers |
| Logical Device Creation | Vulkan 1.0 | `src/vk_device.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.api.device_init.create_device.basic` | PASS | PASS | Validates clean create and destroy cycles |
| Sampler Object Creation | Vulkan 1.0 | `src/vk_sampler.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.api.smoke.create_sampler` | NotSupported | PASS | Host mock lacks graphics pipeline; native validates GFX10 S# descriptor |
| Shader Module Ingestion | Vulkan 1.0 | `src/compilation_cache.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.api.smoke.create_shader` | PASS | PASS | Validates SPIR-V binary validation and parsing |
| Graphics Triangle Pipeline | Vulkan 1.0 | `native/graphics_pipeline_ps5.c`, `src/graphics_program.c` | `<ps5vk/ps5vk.h>` | `dEQP-VK.api.smoke.triangle` | NotSupported | PASS | Host mock lacks AGC graphics backend; native executes on GFX1013 |
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
- **Total Selected Cases:** 26
- **Native PS5 Execution:** 26/26 PASS (100% pass rate on target hardware)
- **Host Mock Execution:** 22 PASS, 4 NotSupported (GPU shader and graphics pipeline execution unported on host mock harness), 0 FAIL
- **Harness Port Status:** Static dispatch adapter implemented; zero reliance on dynamic loading (`dlopen`).
