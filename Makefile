PYTHON ?= python3
CC ?= cc
CXX ?= c++
# Sibling lab projects (ps5-agc-gears, logging_server). The canonical layout has
# them next to this checkout, so the default is the parent directory; out-of-tree
# worktrees override LAB_SIBLINGS with the lab's projects directory.
LAB_SIBLINGS ?= ..
LOCAL_GLSLANG := $(abspath build/runtime-graphics/toolchain/usr/bin/glslangValidator)
GLSLANG ?= $(if $(wildcard $(LOCAL_GLSLANG)),$(LOCAL_GLSLANG),glslangValidator)
.DEFAULT_GOAL := check
.PHONY: inspect-graphics-compiler
inspect-graphics-compiler: build/libpsbc.host.a
	mkdir -p build/runtime-graphics
	$(GLSLANG) -V experiments/graphics/runtime_triangle.vert -o build/runtime-graphics/triangle.vert.spv
	$(GLSLANG) -V experiments/graphics/runtime_triangle.frag -o build/runtime-graphics/triangle.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_dual_source.frag -o build/runtime-graphics/dual_source.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_two_mrt.frag -o build/runtime-graphics/two_mrt.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_second_mrt_only.frag -o build/runtime-graphics/second_mrt_only.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_sample_id.vert -o build/runtime-graphics/sample_id.vert.spv
	$(GLSLANG) -V experiments/graphics/runtime_sample_id.frag -o build/runtime-graphics/sample_id.frag.spv
	$(GLSLANG) -V --target-env vulkan1.1 experiments/graphics/runtime_view_index.vert -o build/runtime-graphics/view_index.vert.spv
	$(GLSLANG) -V -S vert -DTEST_VERTEX=1 experiments/graphics/runtime_flat.glsl -o build/runtime-graphics/flat.vert.spv
	$(GLSLANG) -V -S frag experiments/graphics/runtime_flat.glsl -o build/runtime-graphics/flat.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_parameters.vert -o build/runtime-graphics/parameters.vert.spv
	$(GLSLANG) -V experiments/graphics/runtime_parameters.frag -o build/runtime-graphics/parameters.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_vertex_input.vert -o build/runtime-graphics/vertex_input.vert.spv
	$(GLSLANG) -V experiments/graphics/runtime_vertex_bindings.vert -o build/runtime-graphics/vertex_bindings.vert.spv
	$(GLSLANG) -V experiments/graphics/runtime_vertex_bindings_probe.vert -o build/runtime-graphics/vertex_bindings_probe.vert.spv
	$(GLSLANG) -V experiments/graphics/runtime_vertex_input.frag -o build/runtime-graphics/vertex_input.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_vertex_uint.vert -o build/runtime-graphics/vertex_uint.vert.spv
	$(GLSLANG) -V experiments/graphics/runtime_vertex_sint.vert -o build/runtime-graphics/vertex_sint.vert.spv
	$(GLSLANG) -V experiments/graphics/runtime_vertex_unorm.vert -o build/runtime-graphics/vertex_unorm.vert.spv
	$(GLSLANG) -V experiments/graphics/runtime_vertex_format.frag -o build/runtime-graphics/vertex_format.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_texture.frag -o build/runtime-graphics/texture.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_cube_array.frag -o build/runtime-graphics/cube_array.frag.spv
	$(GLSLANG) -V --target-env vulkan1.1 experiments/graphics/runtime_view_index.frag -o build/runtime-graphics/view_index.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_descriptor_arrays.frag -o build/runtime-graphics/descriptor_arrays.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_fragment_store.frag -o build/runtime-graphics/fragment_store.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_fragment_coord_store.frag -o build/runtime-graphics/fragment_coord_store.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_fragment_coord_store_after_kill.frag -o build/runtime-graphics/fragment_coord_store_after_kill.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_input_attachment.frag -o build/runtime-graphics/input_attachment.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_input_attachment.vert -o build/runtime-graphics/input_attachment_probe.vert.spv
	$(GLSLANG) -V experiments/graphics/runtime_input_attachment_pattern.frag -o build/runtime-graphics/input_attachment_pattern.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_input_attachment_transform.frag -o build/runtime-graphics/input_attachment_transform.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_shared_sets.vert -o build/runtime-graphics/shared_sets.vert.spv
	$(GLSLANG) -V experiments/graphics/runtime_shared_sets.frag -o build/runtime-graphics/shared_sets.frag.spv
	$(GLSLANG) -V -DVERTEX_SAMPLERS_ONLY=1 experiments/graphics/runtime_shared_sets.frag -o build/runtime-graphics/vertex_sets.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_mipmap.vert -o build/runtime-graphics/mipmap.vert.spv
	$(CC) -std=c11 -Wall -Wextra -Werror -Inative -I$(LAB_SIBLINGS)/ps5-agc-gears/src -I$(LAB_SIBLINGS)/ps5-agc-gears/include -Ithird_party/psbc-reference native/runtime_shader.c tools/inspect_graphics_compiler.c src/ps5_compiler_shims.c build/libpsbc.host.a -lstdc++ -lm -lpthread -o build/runtime-graphics/inspect
	./build/runtime-graphics/inspect build/runtime-graphics/triangle.vert.spv build/runtime-graphics/triangle.frag.spv
VULKAN_CFLAGS ?= -Ithird_party/vulkan-headers/include
VK_MEMORY_SOURCES = src/vk_alloc.c src/vk_memory.c src/texture_format.c
VK_IMAGE_TEST_SOURCES = $(VK_MEMORY_SOURCES) src/color_attachment_contract.c src/vk_image_view.c src/vk_render_pass.c src/vk_framebuffer.c
VK_DESCRIPTOR_SOURCES = $(VK_MEMORY_SOURCES) src/vk_descriptor.c
VK_PIPELINE_SOURCES = $(VK_DESCRIPTOR_SOURCES) src/vk_pipeline.c src/spirv_ubo_layout.c src/compilation_cache.c src/vk_pipeline_cache.c
VK_COMMAND_SOURCES = $(VK_PIPELINE_SOURCES) src/vk_command.c src/vk_indirect.c
# The command recording tests build render passes, image views and
# framebuffers through the PUBLIC entry points rather than as structs, so the
# objects they record against are the ones the driver itself accepts.
VK_COMMAND_TEST_SOURCES = $(VK_COMMAND_SOURCES) src/color_attachment_contract.c src/vk_render_pass.c \
        src/vk_image_view.c src/vk_framebuffer.c
# The queue group owns image operations and their format-aware block planner:
# linear staging readback uses 64KB_R_X after GPU completion, while BC block
# copies use the same mip-layout contract without entering the RGBA8 row path.
VK_QUEUE_SOURCES = $(VK_COMMAND_SOURCES) src/vk_fence.c src/vk_sync.c src/vk_buffer_transfer.c src/vk_image_transfer.c src/color_clear.c src/color_detile.c src/depth_detile.c src/texture_copy.c src/texture_layout.c src/vk_query_pool.c src/vk_queue.c src/vk_queue_router.c
VK_GRAPHICS_SOURCES = src/color_attachment_contract.c src/vk_image_view.c src/vk_sampler.c src/vk_render_pass.c src/vk_framebuffer.c src/vk_graphics_pipeline.c src/graphics_program.c src/vk_transfer.c src/vk_copy_commands2.c src/vk_dynamic_rendering.c
VK_DEVICE_SOURCES = $(VK_QUEUE_SOURCES) $(VK_GRAPHICS_SOURCES) src/vk_device.c src/vk_dispatch.c src/vk_swapchain.c native/wsi_present_ps5.c
NATIVE_PREPARE_TEST = -D_DEFAULT_SOURCE $(VULKAN_CFLAGS) -Isrc -I$(LAB_SIBLINGS)/ps5-agc-gears/include -I$(LAB_SIBLINGS)/logging_server/client native/queue_ps5.c src/vk_indirect.c src/descriptor_encode.c src/texture_format.c src/dispatch_encode.c src/compute_commands.c tests/test_native_prepare.c
# graphics_pair.c reads the canonical topology -> primitive mapping from
# src/graphics_program.h, so this host rule needs the pinned Vulkan headers the
# native build already passes.
GRAPHICS_PAIR_TEST = -Ithird_party/vulkan-headers/include -Inative -Isrc -I$(LAB_SIBLINGS)/ps5-agc-gears/src -I$(LAB_SIBLINGS)/ps5-agc-gears/include native/graphics_pair.c src/shader_relocate.c $(LAB_SIBLINGS)/ps5-agc-gears/src/ps5_shader_header.c tests/test_graphics_pair.c
.PHONY: check check-dxvk-ledger doctor compiler-control compiler-programs native-bootstrap vulkan-headers check-sanitize native-memory-check test-shaders
check-dxvk-ledger:
	$(PYTHON) tools/check_dxvk_profile.py --check
	$(PYTHON) tools/check_dxvk_backlog.py --check
	$(PYTHON) tools/run_python_tests.py test_dxvk_backlog test_dxvk_matrix test_subgroup_profile_contract test_t08_device_scope_cts_gate
.PHONY: check-thread-sanitize
# The timeline payload is the one queue state other threads may touch
# (host signal, counter query and semaphore waits), so its host contract also
# runs under ThreadSanitizer. Kernels with high mmap entropy need ASLR off for
# the TSan runtime, hence setarch.
check-thread-sanitize:
	mkdir -p build/tests
	$(CC) -std=c11 -g -O1 -Wall -Wextra -Werror -pthread -fsanitize=thread $(VULKAN_CFLAGS) -Isrc $(VK_QUEUE_SOURCES) src/vk_transfer.c tests/test_vk_timeline.c -o build/tests/test_vk_timeline_tsan
	setarch $$(uname -m) -R ./build/tests/test_vk_timeline_tsan
.PHONY: compiler-pipelines
.PHONY: native-compute native-graphics native-runtime-graphics
.PHONY: upstream-cts check-upstream-cts check-upstream-cts-sink
.PHONY: upstream-cts-run
native-runtime-graphics:
	@test -n "$(GRAPHICS_CONTROL)" || { echo "GRAPHICS_CONTROL is required" >&2; exit 2; }
	PS5VK_GLSLANG=$(GLSLANG) PS5VK_RUNTIME_GRAPHICS=1 PS5VK_SHELL_CLOSE=1 PS5VK_GRAPHICS_API=$(GRAPHICS_CONTROL) PS5VK_GRAPHICS_PRESENT=1 PS5VK_GRAPHICS_DRAW=1 $(PYTHON) tools/build_native.py
native-compute:
	PS5VK_COMPUTE=1 $(PYTHON) tools/build_native.py
.PHONY: native-dual-source upstream-cts-dual-source
native-dual-source:
	@test -n "$(GRAPHICS_CONTROL)" || { echo "GRAPHICS_CONTROL is required" >&2; exit 2; }
	PS5VK_GLSLANG=$(GLSLANG) PS5VK_USE_SDK=1 PS5VK_RUNTIME_GRAPHICS=1 PS5VK_SHELL_CLOSE=1 PS5VK_GRAPHICS_API=$(GRAPHICS_CONTROL) PS5VK_GRAPHICS_DRAW=1 PS5VK_DUAL_SOURCE_PROBE=1 $(PYTHON) tools/build_native.py
.PHONY: native-sample-rate
native-sample-rate:
	@test -n "$(GRAPHICS_CONTROL)" || { echo "GRAPHICS_CONTROL is required" >&2; exit 2; }
	PS5VK_GLSLANG=$(GLSLANG) PS5VK_USE_SDK=1 PS5VK_RUNTIME_GRAPHICS=1 PS5VK_SHELL_CLOSE=1 PS5VK_GRAPHICS_API=$(GRAPHICS_CONTROL) PS5VK_GRAPHICS_DRAW=1 PS5VK_SAMPLE_RATE_DIAGNOSTIC=1 PS5VK_SAMPLE_RATE_PROBE=1 $(PYTHON) tools/build_native.py
.PHONY: native-two-mrt
native-two-mrt:
	@test -n "$(GRAPHICS_CONTROL)" || { echo "GRAPHICS_CONTROL is required" >&2; exit 2; }
	PS5VK_GLSLANG=$(GLSLANG) PS5VK_USE_SDK=1 PS5VK_RUNTIME_GRAPHICS=1 PS5VK_SHELL_CLOSE=1 PS5VK_GRAPHICS_API=$(GRAPHICS_CONTROL) PS5VK_GRAPHICS_DRAW=1 PS5VK_TWO_MRT_PROBE=1 $(PYTHON) tools/build_native.py
native-graphics:
	@test -n "$(GRAPHICS_CONTROL)" || { echo "GRAPHICS_CONTROL is required" >&2; exit 2; }
	PS5VK_GRAPHICS_API=$(GRAPHICS_CONTROL) PS5VK_GRAPHICS_CONTINUOUS=1 PS5VK_GRAPHICS_PRESENT=1 PS5VK_GRAPHICS_DRAW=1 $(PYTHON) tools/build_native.py
compiler-pipelines:
	$(PYTHON) tools/build_program_library.py
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc -Ibuild/program-library $(VK_PIPELINE_SOURCES) src/compute_commands.c src/dispatch_encode.c tests/test_compiled_pipeline.c -o build/program-library/test_compiled_pipeline
	./build/program-library/test_compiled_pipeline
compiler-programs:
	$(PYTHON) tools/compile_program.py experiments/compute/minimal.comp
	$(PYTHON) tools/compile_program.py experiments/compute/xor.comp
native-memory-check:
	$(PYTHON) tools/check_native_memory.py
vulkan-headers:
	$(PYTHON) tools/prepare_vulkan_headers.py
compiler-deps:
	$(PYTHON) tools/prepare_compiler_deps.py
test-shaders:
	$(PYTHON) tools/prepare_test_shaders.py
# ThreadSanitizer for the timeline/queue lock runs as part of the sanitizer
# gate; it needs only setarch (util-linux) on the host.
check-sanitize: check-thread-sanitize
	mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Inative -Isrc src/color_detile.c src/texture_dma.c tests/test_color_rect_clear.c -o build/tests/test_color_rect_clear_sanitized
	./build/tests/test_color_rect_clear_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Isrc src/image_layout_state.c tests/test_image_layout_state.c -o build/tests/test_image_layout_state_sanitized
	./build/tests/test_image_layout_state_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Inative -Isrc src/image_layout_state.c src/color_detile.c src/depth_detile.c src/depth_layout.c tests/test_readback_commands.c -o build/tests/test_readback_commands_sanitized
	./build/tests/test_readback_commands_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Inative -Isrc src/image_layout_state.c src/texture_dma.c src/graphics_sync.c tests/test_upload_commands.c -o build/tests/test_upload_commands_sanitized
	./build/tests/test_upload_commands_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Isrc tests/test_descriptor_table_layout.c -o build/tests/test_descriptor_table_layout_sanitized
	./build/tests/test_descriptor_table_layout_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Isrc $(VK_MEMORY_SOURCES) src/vertex_descriptor.c src/vertex_fetch.c tests/test_vertex_fetch.c -o build/tests/test_vertex_fetch_sanitized
	./build/tests/test_vertex_fetch_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Inative -Isrc -I$(LAB_SIBLINGS)/ps5-agc-gears/src -I$(LAB_SIBLINGS)/ps5-agc-gears/include native/draw_prepare_ps5.c src/texture_format.c src/vertex_descriptor.c tests/test_draw_prepare_ps5.c -o build/tests/test_draw_prepare_ps5_sanitized
	./build/tests/test_draw_prepare_ps5_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Inative -Isrc -I$(LAB_SIBLINGS)/ps5-agc-gears/src -I$(LAB_SIBLINGS)/ps5-agc-gears/include native/targets_ps5.c native/image_ps5.c src/depth_layout.c src/texture_format.c src/texture_layout.c $(LAB_SIBLINGS)/ps5-agc-gears/src/ps5_color_target.c $(LAB_SIBLINGS)/ps5-agc-gears/src/ps5_depth_target.c tests/test_targets_ps5.c -o build/tests/test_targets_t07_diagnostic_sanitized
	./build/tests/test_targets_t07_diagnostic_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Inative -Isrc native/input_attachment_gate.c tests/test_input_attachment_gate.c -o build/tests/test_input_attachment_gate_sanitized
	./build/tests/test_input_attachment_gate_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Inative -Isrc -I$(LAB_SIBLINGS)/ps5-agc-gears/include native/draw_batch_ps5.c native/command_arena_ps5.c src/graphics_sync.c tests/test_draw_batch_ps5.c -o build/tests/test_draw_batch_ps5_sanitized
	./build/tests/test_draw_batch_ps5_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Inative native/input_attachment_oracle.c tests/test_input_attachment_oracle.c -o build/tests/test_input_attachment_oracle_sanitized
	./build/tests/test_input_attachment_oracle_sanitized
	@if [ -d third_party/psbc-reference ]; then $(MAKE) test-runtime-header RUNTIME_HEADER_SANITIZERS=-fsanitize=address,undefined; fi
	mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Isrc src/vk_alloc.c src/vk_sampler.c tests/test_vk_sampler.c -o build/tests/test_vk_sampler_sanitized
	./build/tests/test_vk_sampler_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Isrc src/sampler_core_probe.c tests/test_sampler_core_probe.c -o build/tests/test_sampler_core_probe_sanitized
	./build/tests/test_sampler_core_probe_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Isrc src/integer_sampled_probe.c tests/test_integer_sampled_probe.c -o build/tests/test_integer_sampled_probe_sanitized
	./build/tests/test_integer_sampled_probe_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Isrc src/color_detile.c tests/test_color_detile.c -o build/tests/test_color_detile_sanitized
	./build/tests/test_color_detile_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Isrc src/depth_detile.c src/color_detile.c tests/test_depth_detile.c -o build/tests/test_depth_detile_sanitized
	./build/tests/test_depth_detile_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Isrc src/texture_format.c tests/test_texture_format.c -o build/tests/test_texture_format_sanitized
	./build/tests/test_texture_format_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Isrc src/texture_copy.c src/texture_format.c src/texture_layout.c tests/test_texture_copy.c -o build/tests/test_texture_copy_sanitized
	./build/tests/test_texture_copy_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Isrc src/texture_copy.c src/texture_format.c src/texture_layout.c tests/test_texture_copy.c -o build/tests/test_d32_gather_copy_sanitized
	./build/tests/test_d32_gather_copy_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Isrc $(VK_IMAGE_TEST_SOURCES) tests/test_vk_image.c -o build/tests/test_vk_image_sanitized
	./build/tests/test_vk_image_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Isrc src/vk_alloc.c src/color_attachment_contract.c src/vk_render_pass.c tests/test_vk_render_pass.c -o build/tests/test_vk_render_pass_sanitized
	./build/tests/test_vk_render_pass_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Isrc src/vk_alloc.c src/color_attachment_contract.c src/vk_render_pass.c tests/test_vk_render_pass2.c -o build/tests/test_vk_render_pass2_sanitized
	./build/tests/test_vk_render_pass2_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Isrc src/multiview_witness.c tests/test_multiview_witness.c -o build/tests/test_multiview_witness_sanitized
	./build/tests/test_multiview_witness_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Isrc src/platform_host.c src/vk_alloc.c tests/test_multiview_capability.c -o build/tests/test_multiview_capability_sanitized
	./build/tests/test_multiview_capability_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Isrc -DPS5VK_MULTIVIEW_DIAGNOSTIC=1 $(VK_IMAGE_TEST_SOURCES) tests/test_framebuffer_multiview.c -o build/tests/test_framebuffer_multiview_diagnostic_sanitized
	./build/tests/test_framebuffer_multiview_diagnostic_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(GRAPHICS_PAIR_TEST) -o build/tests/test_graphics_pair_sanitized
	./build/tests/test_graphics_pair_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Isrc src/shader_relocate.c tests/test_shader_relocate.c -o build/tests/test_shader_relocate_sanitized
	./build/tests/test_shader_relocate_sanitized
	# The native queue fixture must map CPU-visible memory in the gfx1013
	# address32_hi=2 aperture (0x2xxxxxxxx). ASan reserves that aperture as
	# shadow gap on x86-64, so this one fixture is intentionally UBSan-only;
	# all surrounding queue/descriptor tests below retain ASan+UBSan.
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=undefined $(NATIVE_PREPARE_TEST) -o build/tests/test_native_prepare_sanitized
	./build/tests/test_native_prepare_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Isrc src/descriptor_encode.c src/texture_format.c tests/test_descriptor_encode.c -o build/tests/test_descriptor_encode_sanitized
	./build/tests/test_descriptor_encode_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc src/compute_commands.c src/dispatch_encode.c tests/test_dispatch_encode.c -o build/tests/test_dispatch_encode_sanitized
	./build/tests/test_dispatch_encode_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_MEMORY_SOURCES) tests/test_vk_memory.c -o build/tests/test_vk_memory_sanitized
	./build/tests/test_vk_memory_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) tests/test_vk_device.c -o build/tests/test_vk_device_sanitized
	./build/tests/test_vk_device_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) tests/test_timeline_device.c -o build/tests/test_timeline_device_sanitized
	./build/tests/test_timeline_device_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) tests/test_renderpass2_route.c -o build/tests/test_renderpass2_route_sanitized
	./build/tests/test_renderpass2_route_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) tests/test_memory_requirements2_route.c -o build/tests/test_memory_requirements2_route_sanitized
	./build/tests/test_memory_requirements2_route_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) src/depth_layout.c native/image_ps5.c tools/dump_device_reporting.c -o build/tests/dump_device_reporting_sanitized
	./build/tests/dump_device_reporting_sanitized > /dev/null
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) tests/test_pipeline_cache.c -o build/tests/test_pipeline_cache_sanitized
	./build/tests/test_pipeline_cache_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) tests/test_query_pool.c -o build/tests/test_query_pool_sanitized
	./build/tests/test_query_pool_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) src/platform_host.c tests/test_buffer_transfer.c -o build/tests/test_buffer_transfer_sanitized
	./build/tests/test_buffer_transfer_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) src/platform_host.c src/depth_layout.c native/image_ps5.c tests/test_image_copy_clear.c -o build/tests/test_image_copy_clear_sanitized
	./build/tests/test_image_copy_clear_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) src/depth_layout.c native/image_ps5.c tests/test_depth_stencil_api.c -o build/tests/test_depth_stencil_api_sanitized
	./build/tests/test_depth_stencil_api_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Inative -Isrc src/image_layout_state.c src/texture_dma.c src/graphics_sync.c src/color_detile.c src/depth_detile.c src/depth_layout.c tests/test_depth_stencil_executor.c -o build/tests/test_depth_stencil_executor_sanitized
	./build/tests/test_depth_stencil_executor_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) src/platform_host.c src/depth_layout.c native/image_ps5.c tests/test_bc_subresource_copy.c -o build/tests/test_bc_subresource_copy_sanitized
	./build/tests/test_bc_subresource_copy_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) src/platform_host.c src/depth_layout.c native/image_ps5.c tests/test_integer_readback_diagnostic.c -o build/tests/test_integer_readback_diagnostic_sanitized
	./build/tests/test_integer_readback_diagnostic_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_QUEUE_SOURCES) tests/test_indirect_queue.c -o build/tests/test_indirect_queue_sanitized
	./build/tests/test_indirect_queue_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_COMMAND_SOURCES) tests/test_vk_indirect.c -o build/tests/test_vk_indirect_sanitized
	./build/tests/test_vk_indirect_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_DESCRIPTOR_SOURCES) tests/test_vk_descriptor.c -o build/tests/test_vk_descriptor_sanitized
	./build/tests/test_vk_descriptor_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_PIPELINE_SOURCES) tests/test_vk_pipeline.c -o build/tests/test_vk_pipeline_sanitized
	./build/tests/test_vk_pipeline_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_COMMAND_SOURCES) tests/test_secondary_command_buffers.c -o build/tests/test_secondary_command_buffers_sanitized
	./build/tests/test_secondary_command_buffers_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_QUEUE_SOURCES) tests/test_secondary_execute.c -o build/tests/test_secondary_execute_sanitized
	./build/tests/test_secondary_execute_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_COMMAND_TEST_SOURCES) tests/test_vk_command.c -o build/tests/test_vk_command_sanitized
	./build/tests/test_vk_command_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_COMMAND_SOURCES) src/vk_transfer.c src/texture_copy.c src/texture_layout.c tests/test_vk_transfer.c -o build/tests/test_vk_transfer_sanitized
	./build/tests/test_vk_transfer_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc src/vk_alloc.c src/vk_fence.c tests/test_vk_fence.c -o build/tests/test_vk_fence_sanitized
	./build/tests/test_vk_fence_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc src/vk_alloc.c src/vk_sync.c tests/test_vk_sync.c -o build/tests/test_vk_sync_sanitized
	./build/tests/test_vk_sync_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_QUEUE_SOURCES) tests/test_vk_queue.c -o build/tests/test_vk_queue_sanitized
	./build/tests/test_vk_queue_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_QUEUE_SOURCES) src/vk_transfer.c tests/test_vk_timeline.c -o build/tests/test_vk_timeline_sanitized
	./build/tests/test_vk_timeline_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc src/compilation_cache.c tests/test_compilation_cache.c -o build/tests/test_compilation_cache_sanitized
	./build/tests/test_compilation_cache_sanitized
	$(MAKE) graphics-stage-shaders
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc src/spirv_graphics_interface.c src/texture_format.c tests/test_graphics_stages.c -o build/tests/test_graphics_stages_sanitized
	./build/tests/test_graphics_stages_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc src/spirv_graphics_interface.c src/texture_format.c tests/test_tessellation_stage.c -o build/tests/test_tessellation_stage_sanitized
	./build/tests/test_tessellation_stage_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc src/clip_cull_witness.c tests/test_clip_cull_witness.c -o build/tests/test_clip_cull_witness_sanitized
	./build/tests/test_clip_cull_witness_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc src/geometry_witness.c tests/test_geometry_witness.c -o build/tests/test_geometry_witness_sanitized
	./build/tests/test_geometry_witness_sanitized
check:
	@mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc tests/test_descriptor_table_layout.c -o build/tests/test_descriptor_table_layout
	./build/tests/test_descriptor_table_layout
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/texture_dma.c tests/test_texture_dma.c -o build/tests/test_texture_dma
	./build/tests/test_texture_dma
	$(CC) -std=c11 -Wall -Wextra -Werror -Isrc src/color_clear.c tests/test_color_clear.c -o build/tests/test_color_clear
	./build/tests/test_color_clear
	$(CC) -std=c11 -Wall -Wextra -Werror -Isrc src/color_detile.c tests/test_color_detile.c -o build/tests/test_color_detile
	./build/tests/test_color_detile
	$(CC) -std=c11 -Wall -Wextra -Werror -Isrc src/scene_geometry.c tests/test_scene_geometry.c -o build/tests/test_scene_geometry
	./build/tests/test_scene_geometry
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/sampler_core_probe.c tests/test_sampler_core_probe.c -o build/tests/test_sampler_core_probe
	./build/tests/test_sampler_core_probe
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/sampled_format_probe.c tests/test_sampled_format_probe.c -o build/tests/test_sampled_format_probe
	./build/tests/test_sampled_format_probe
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/integer_sampled_probe.c tests/test_integer_sampled_probe.c -o build/tests/test_integer_sampled_probe
	./build/tests/test_integer_sampled_probe
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/vertex_format_probe.c src/texture_format.c tests/test_vertex_format_probe.c -o build/tests/test_vertex_format_probe
	./build/tests/test_vertex_format_probe
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/image_layout_state.c tests/test_image_layout_state.c -o build/tests/test_image_layout_state
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -Isrc src/image_layout_state.c src/texture_dma.c src/graphics_sync.c tests/test_upload_commands.c -o build/tests/test_upload_commands
	./build/tests/test_upload_commands
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -Isrc src/image_layout_state.c src/color_detile.c src/depth_detile.c src/depth_layout.c tests/test_readback_commands.c -o build/tests/test_readback_commands
	./build/tests/test_readback_commands
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Inative -Isrc src/image_layout_state.c src/color_detile.c src/depth_detile.c src/depth_layout.c tests/test_readback_regions.c -o build/tests/test_readback_regions
	./build/tests/test_readback_regions
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -Isrc src/color_detile.c src/texture_dma.c tests/test_color_rect_clear.c -o build/tests/test_color_rect_clear
	./build/tests/test_color_rect_clear
	./build/tests/test_image_layout_state
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/texture_copy.c src/texture_format.c src/texture_layout.c tests/test_texture_copy.c -o build/tests/test_texture_copy
	./build/tests/test_texture_copy
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/texture_copy.c src/texture_format.c src/texture_layout.c tests/test_texture_copy.c -o build/tests/test_d32_gather_copy
	./build/tests/test_d32_gather_copy
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/texture_format.c tests/test_texture_format.c -o build/tests/test_texture_format
	./build/tests/test_texture_format
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/texture_format.c tests/test_bc_diagnostic_profile.c -o build/tests/test_bc_diagnostic_profile
	./build/tests/test_bc_diagnostic_profile
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -Isrc -I$(LAB_SIBLINGS)/ps5-agc-gears/include -I$(LAB_SIBLINGS)/ps5-agc-gears/src src/texture_format.c src/depth_layout.c src/texture_layout.c src/color_attachment_contract.c native/image_ps5.c native/targets_ps5.c $(LAB_SIBLINGS)/ps5-agc-gears/src/ps5_depth_target.c $(LAB_SIBLINGS)/ps5-agc-gears/src/ps5_color_target.c tests/test_d16_diagnostic_profile.c -o build/tests/test_d16_diagnostic_profile
	./build/tests/test_d16_diagnostic_profile
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_COMMAND_SOURCES) src/vk_transfer.c src/texture_copy.c src/vk_image_view.c src/vk_sampler.c src/texture_descriptor.c src/texture_layout.c src/depth_layout.c native/image_ps5.c tests/test_texture_descriptor.c -o build/tests/test_texture_descriptor
	./build/tests/test_texture_descriptor
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/texture_format.c src/texture_layout.c src/depth_layout.c native/image_ps5.c tests/test_texture_layout.c -o build/tests/test_texture_layout
	./build/tests/test_texture_layout
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/vk_alloc.c src/vk_sampler.c tests/test_vk_sampler.c -o build/tests/test_vk_sampler
	./build/tests/test_vk_sampler
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/vk_alloc.c src/vk_sampler.c tests/test_vk_sampler.c -o build/tests/test_vk_sampler_bc_diagnostic
	./build/tests/test_vk_sampler_bc_diagnostic
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -Isrc -I$(LAB_SIBLINGS)/ps5-agc-gears/include native/index_emit_ps5.c tests/test_index_emit_ps5.c -o build/tests/test_index_emit_ps5
	./build/tests/test_index_emit_ps5
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_MEMORY_SOURCES) src/index_fetch.c tests/test_index_fetch.c -o build/tests/test_index_fetch
	./build/tests/test_index_fetch
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_MEMORY_SOURCES) src/vertex_descriptor.c src/vertex_fetch.c tests/test_vertex_fetch.c -o build/tests/test_vertex_fetch
	./build/tests/test_vertex_fetch
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/vertex_descriptor.c src/texture_format.c tests/test_vertex_descriptor.c -o build/tests/test_vertex_descriptor
	./build/tests/test_vertex_descriptor
	$(CC) -std=c11 -Wall -Wextra -Werror -Isrc src/triangle_readback.c tests/test_triangle_readback.c -o build/tests/test_triangle_readback
	./build/tests/test_triangle_readback
	@mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -Isrc -I$(LAB_SIBLINGS)/ps5-agc-gears/include native/command_arena_ps5.c tests/test_command_arena_ps5.c -o build/tests/test_command_arena_ps5
	./build/tests/test_command_arena_ps5
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -Isrc -I$(LAB_SIBLINGS)/ps5-agc-gears/include native/draw_batch_ps5.c native/command_arena_ps5.c src/graphics_sync.c tests/test_draw_batch_ps5.c -o build/tests/test_draw_batch_ps5
	./build/tests/test_draw_batch_ps5
	@mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror -Isrc src/graphics_sync.c tests/test_graphics_sync.c -o build/tests/test_graphics_sync
	./build/tests/test_graphics_sync
	@mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -Isrc -I$(LAB_SIBLINGS)/ps5-agc-gears/src -I$(LAB_SIBLINGS)/ps5-agc-gears/include native/draw_prepare_ps5.c src/vertex_descriptor.c src/texture_format.c tests/test_draw_prepare_ps5.c -o build/tests/test_draw_prepare_ps5
	./build/tests/test_draw_prepare_ps5
	@mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -Isrc native/input_attachment_gate.c tests/test_input_attachment_gate.c -o build/tests/test_input_attachment_gate
	./build/tests/test_input_attachment_gate
	@mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror -Inative native/input_attachment_oracle.c tests/test_input_attachment_oracle.c -o build/tests/test_input_attachment_oracle
	./build/tests/test_input_attachment_oracle
	@mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -Isrc -I$(LAB_SIBLINGS)/ps5-agc-gears/src -I$(LAB_SIBLINGS)/ps5-agc-gears/include native/draw_emit_ps5.c native/index_emit_ps5.c $(LAB_SIBLINGS)/ps5-agc-gears/src/ps5_agc_writer.c $(LAB_SIBLINGS)/ps5-agc-gears/src/ps5_gpu_span.c tests/test_draw_emit_ps5.c -o build/tests/test_draw_emit_ps5
	./build/tests/test_draw_emit_ps5
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -Isrc tests/test_draw_parameters.c -o build/tests/test_draw_parameters
	./build/tests/test_draw_parameters
	@mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -Isrc -I$(LAB_SIBLINGS)/ps5-agc-gears/src -I$(LAB_SIBLINGS)/ps5-agc-gears/include native/draw_state_ps5.c src/color_attachment_contract.c native/viewport_ps5.c $(LAB_SIBLINGS)/ps5-agc-gears/src/ps5_pipeline.c tests/test_draw_state_ps5.c -o build/tests/test_draw_state_ps5
	./build/tests/test_draw_state_ps5
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -Isrc -I$(LAB_SIBLINGS)/ps5-agc-gears/src -I$(LAB_SIBLINGS)/ps5-agc-gears/include native/draw_state_ps5.c src/color_attachment_contract.c native/viewport_ps5.c $(LAB_SIBLINGS)/ps5-agc-gears/src/ps5_pipeline.c tests/test_eds_draw_state_ps5.c -o build/tests/test_eds_draw_state_ps5
	./build/tests/test_eds_draw_state_ps5
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -Isrc -I$(LAB_SIBLINGS)/ps5-agc-gears/src -I$(LAB_SIBLINGS)/ps5-agc-gears/include native/draw_state_ps5.c src/color_attachment_contract.c native/viewport_ps5.c $(LAB_SIBLINGS)/ps5-agc-gears/src/ps5_pipeline.c tests/test_draw_state_ps5.c -o build/tests/test_draw_state_ps5_d16_diagnostic
	./build/tests/test_draw_state_ps5_d16_diagnostic
	@mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -I$(LAB_SIBLINGS)/ps5-agc-gears/include native/viewport_ps5.c tests/test_viewport_ps5.c -o build/tests/test_viewport_ps5
	./build/tests/test_viewport_ps5
	mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -Isrc -I$(LAB_SIBLINGS)/ps5-agc-gears/src -I$(LAB_SIBLINGS)/ps5-agc-gears/include native/graphics_pipeline_ps5.c native/tess_shared_storage.c tests/test_graphics_pipeline_backend.c -pthread -o build/tests/test_graphics_pipeline_backend
	./build/tests/test_graphics_pipeline_backend
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_PIPELINE_SOURCES) src/color_attachment_contract.c src/vk_graphics_pipeline.c src/graphics_program.c tests/test_vk_graphics_pipeline.c -o build/tests/test_vk_graphics_pipeline
	./build/tests/test_vk_graphics_pipeline
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/graphics_program.c tests/test_graphics_program.c -o build/tests/test_graphics_program
	./build/tests/test_graphics_program
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -Isrc -I$(LAB_SIBLINGS)/ps5-agc-gears/src -I$(LAB_SIBLINGS)/ps5-agc-gears/include native/targets_ps5.c native/image_ps5.c src/depth_layout.c src/texture_format.c src/texture_layout.c $(LAB_SIBLINGS)/ps5-agc-gears/src/ps5_color_target.c $(LAB_SIBLINGS)/ps5-agc-gears/src/ps5_depth_target.c tests/test_targets_ps5.c -o build/tests/test_targets_ps5
	./build/tests/test_targets_ps5
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -Isrc -I$(LAB_SIBLINGS)/ps5-agc-gears/src -I$(LAB_SIBLINGS)/ps5-agc-gears/include native/targets_ps5.c native/image_ps5.c src/depth_layout.c src/texture_format.c src/texture_layout.c $(LAB_SIBLINGS)/ps5-agc-gears/src/ps5_color_target.c $(LAB_SIBLINGS)/ps5-agc-gears/src/ps5_depth_target.c tests/test_targets_ps5.c -o build/tests/test_targets_t07_diagnostic
	./build/tests/test_targets_t07_diagnostic
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc -I$(LAB_SIBLINGS)/ps5-agc-gears/include -I$(LAB_SIBLINGS)/logging_server/client native/memory_ps5.c tests/test_native_memory_alignment.c -o build/tests/test_native_memory_alignment
	./build/tests/test_native_memory_alignment
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/depth_layout.c src/texture_format.c src/texture_layout.c native/image_ps5.c tests/test_depth_layout.c -o build/tests/test_depth_layout
	./build/tests/test_depth_layout
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_IMAGE_TEST_SOURCES) tests/test_vk_image.c -o build/tests/test_vk_image
	./build/tests/test_vk_image
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/vk_alloc.c src/color_attachment_contract.c src/vk_render_pass.c tests/test_vk_render_pass.c -o build/tests/test_vk_render_pass
	./build/tests/test_vk_render_pass
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/vk_alloc.c src/color_attachment_contract.c src/vk_render_pass.c tests/test_vk_render_pass2.c -o build/tests/test_vk_render_pass2
	./build/tests/test_vk_render_pass2
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/multiview_witness.c tests/test_multiview_witness.c -o build/tests/test_multiview_witness
	./build/tests/test_multiview_witness
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/platform_host.c src/vk_alloc.c tests/test_multiview_capability.c -o build/tests/test_multiview_capability
	./build/tests/test_multiview_capability
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_IMAGE_TEST_SOURCES) tests/test_framebuffer_multiview.c -o build/tests/test_framebuffer_multiview
	./build/tests/test_framebuffer_multiview
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc -DPS5VK_MULTIVIEW_DIAGNOSTIC=1 $(VK_IMAGE_TEST_SOURCES) tests/test_framebuffer_multiview.c -o build/tests/test_framebuffer_multiview_diagnostic
	./build/tests/test_framebuffer_multiview_diagnostic
	$(CC) -std=c11 -Wall -Wextra -Werror $(GRAPHICS_PAIR_TEST) -o build/tests/test_graphics_pair
	./build/tests/test_graphics_pair
	$(CC) -std=c11 -Wall -Wextra -Werror -Isrc src/shader_relocate.c tests/test_shader_relocate.c -o build/tests/test_shader_relocate
	./build/tests/test_shader_relocate
	$(CC) -std=c11 -Wall -Wextra -Werror $(NATIVE_PREPARE_TEST) -o build/tests/test_native_prepare
	./build/tests/test_native_prepare
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/descriptor_encode.c src/texture_format.c tests/test_descriptor_encode.c -o build/tests/test_descriptor_encode
	./build/tests/test_descriptor_encode
	$(PYTHON) tools/prepare_vulkan_headers.py --check
	$(PYTHON) tools/check_command_surface.py --check
	$(PYTHON) tools/derive_dxvk_profile.py --check
	# Build the reporting fixture before Python discovery: reporting-matrix
	# regression tests invoke the checker in process and must not skip for a
	# fixture that this same target only planned to create later.
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) src/depth_layout.c native/image_ps5.c tools/dump_device_reporting.c -o build/tests/dump_device_reporting
	$(PYTHON) tools/run_python_tests.py --exclude-ledger
	$(CC) -std=c11 -Wall -Wextra -Werror -Inative -I$(LAB_SIBLINGS)/ps5-agc-gears/include tests/test_submit_suspend.c -o build/tests/test_submit_suspend
	./build/tests/test_submit_suspend
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/vk_queue_router.c tests/test_queue_router.c -o build/tests/test_queue_router
	./build/tests/test_queue_router
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc tests/test_physical_device_profile.c -o build/tests/test_physical_device_profile
	./build/tests/test_physical_device_profile
	$(CC) -std=c11 -Wall -Wextra -Werror -Isrc src/scene_region.c tests/test_scene_region.c -o build/tests/test_scene_region
	./build/tests/test_scene_region
	mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror -Isrc src/compute_check.c tests/test_compute_check.c -o build/tests/test_compute_check
	./build/tests/test_compute_check
	$(CC) -std=c11 -Wall -Wextra -Werror -Isrc src/compute_commands.c tests/test_compute_commands.c -o build/tests/test_compute_commands
	./build/tests/test_compute_commands
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/compute_commands.c src/dispatch_encode.c tests/test_dispatch_encode.c -o build/tests/test_dispatch_encode
	./build/tests/test_dispatch_encode
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_MEMORY_SOURCES) tests/test_vk_memory.c -o build/tests/test_vk_memory
	./build/tests/test_vk_memory
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) tests/test_vk_device.c -o build/tests/test_vk_device
	./build/tests/test_vk_device

	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) tests/test_t09_sampler_negotiation.c -o build/tests/test_t09_sampler_negotiation
	./build/tests/test_t09_sampler_negotiation
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) tests/test_t11_demote_negotiation.c -o build/tests/test_t11_demote_negotiation
	./build/tests/test_t11_demote_negotiation

	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) tests/test_timeline_device.c -o build/tests/test_timeline_device
	./build/tests/test_timeline_device
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) tests/test_renderpass2_route.c -o build/tests/test_renderpass2_route
	./build/tests/test_renderpass2_route
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) tests/test_memory_requirements2_route.c -o build/tests/test_memory_requirements2_route
	./build/tests/test_memory_requirements2_route

	# Reporting audit: dump what the public query paths report and check it
	# against the pinned specification tables and the pinned CTS consumer rules.
	$(PYTHON) tools/check_reporting_matrix.py --check
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) tests/test_pipeline_cache.c -o build/tests/test_pipeline_cache
	./build/tests/test_pipeline_cache
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) tests/test_query_pool.c -o build/tests/test_query_pool
	./build/tests/test_query_pool
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) src/platform_host.c tests/test_buffer_transfer.c -o build/tests/test_buffer_transfer
	./build/tests/test_buffer_transfer
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) src/platform_host.c src/depth_layout.c native/image_ps5.c tests/test_image_copy_clear.c -o build/tests/test_image_copy_clear
	./build/tests/test_image_copy_clear
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) src/depth_layout.c native/image_ps5.c tests/test_depth_stencil_api.c -o build/tests/test_depth_stencil_api
	./build/tests/test_depth_stencil_api
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -Isrc src/image_layout_state.c src/texture_dma.c src/graphics_sync.c src/color_detile.c src/depth_detile.c src/depth_layout.c tests/test_depth_stencil_executor.c -o build/tests/test_depth_stencil_executor
	./build/tests/test_depth_stencil_executor
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) src/platform_host.c src/depth_layout.c native/image_ps5.c tests/test_bc_subresource_copy.c -o build/tests/test_bc_subresource_copy
	./build/tests/test_bc_subresource_copy
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) src/platform_host.c src/depth_layout.c native/image_ps5.c tests/test_integer_readback_diagnostic.c -o build/tests/test_integer_readback_diagnostic
	./build/tests/test_integer_readback_diagnostic
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) src/platform_host.c src/depth_layout.c native/image_ps5.c tests/test_cts_draw_case_trace.c -o build/tests/test_cts_draw_case_trace
	./build/tests/test_cts_draw_case_trace
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) src/depth_layout.c native/image_ps5.c tests/test_dxvk_render_eds.c -o build/tests/test_dxvk_render_eds
	./build/tests/test_dxvk_render_eds
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) src/depth_layout.c native/image_ps5.c tests/test_dxvk_maintenance1.c -o build/tests/test_dxvk_maintenance1
	./build/tests/test_dxvk_maintenance1
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) src/depth_layout.c native/image_ps5.c tests/test_dxvk_copy_commands2.c -o build/tests/test_dxvk_copy_commands2
	./build/tests/test_dxvk_copy_commands2
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) src/depth_layout.c native/image_ps5.c tests/test_dxvk_dynamic_rendering.c -o build/tests/test_dxvk_dynamic_rendering
	./build/tests/test_dxvk_dynamic_rendering
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Isrc -Inative $(VK_DEVICE_SOURCES) src/depth_layout.c src/image_layout_state.c native/image_ps5.c tests/test_dxvk_render_witness_trace.c -o build/tests/test_dxvk_render_witness_trace
	./build/tests/test_dxvk_render_witness_trace
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) src/platform_host.c src/depth_layout.c native/image_ps5.c tests/test_render_pass_initialization_trace.c -o build/tests/test_render_pass_initialization_trace
	./build/tests/test_render_pass_initialization_trace
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_QUEUE_SOURCES) tests/test_indirect_queue.c -o build/tests/test_indirect_queue
	./build/tests/test_indirect_queue
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_COMMAND_SOURCES) tests/test_vk_indirect.c -o build/tests/test_vk_indirect
	./build/tests/test_vk_indirect
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_DESCRIPTOR_SOURCES) tests/test_vk_descriptor.c -o build/tests/test_vk_descriptor
	./build/tests/test_vk_descriptor
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_PIPELINE_SOURCES) tests/test_vk_pipeline.c -o build/tests/test_vk_pipeline
	./build/tests/test_vk_pipeline
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_COMMAND_SOURCES) tests/test_secondary_command_buffers.c -o build/tests/test_secondary_command_buffers
	./build/tests/test_secondary_command_buffers
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_QUEUE_SOURCES) tests/test_secondary_execute.c -o build/tests/test_secondary_execute
	./build/tests/test_secondary_execute
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_COMMAND_TEST_SOURCES) tests/test_vk_command.c -o build/tests/test_vk_command
	./build/tests/test_vk_command
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_COMMAND_SOURCES) src/vk_transfer.c src/texture_copy.c src/texture_layout.c tests/test_vk_transfer.c -o build/tests/test_vk_transfer
	./build/tests/test_vk_transfer
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/vk_alloc.c src/vk_fence.c tests/test_vk_fence.c -o build/tests/test_vk_fence
	./build/tests/test_vk_fence
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/vk_alloc.c src/vk_sync.c tests/test_vk_sync.c -o build/tests/test_vk_sync
	./build/tests/test_vk_sync
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_QUEUE_SOURCES) tests/test_vk_queue.c -o build/tests/test_vk_queue
	./build/tests/test_vk_queue
	$(CC) -std=c11 -Wall -Wextra -Werror -pthread $(VULKAN_CFLAGS) -Isrc $(VK_QUEUE_SOURCES) src/vk_transfer.c tests/test_vk_timeline.c -o build/tests/test_vk_timeline
	./build/tests/test_vk_timeline
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/compilation_cache.c tests/test_compilation_cache.c -o build/tests/test_compilation_cache
	./build/tests/test_compilation_cache
	$(CC) -std=c11 -Wall -Wextra -Werror -Isrc src/clip_cull_witness.c tests/test_clip_cull_witness.c -o build/tests/test_clip_cull_witness
	./build/tests/test_clip_cull_witness
	$(CC) -std=c11 -Wall -Wextra -Werror -Isrc src/geometry_witness.c tests/test_geometry_witness.c -o build/tests/test_geometry_witness
	./build/tests/test_geometry_witness
	$(CC) -std=c11 -Wall -Wextra -Werror -Isrc src/dual_source_oracle.c tests/test_dual_source_oracle.c -o build/tests/test_dual_source_oracle
	./build/tests/test_dual_source_oracle
	$(CC) -std=c11 -Wall -Wextra -Werror -Isrc src/two_mrt_oracle.c tests/test_two_mrt_oracle.c -o build/tests/test_two_mrt_oracle
	./build/tests/test_two_mrt_oracle
	$(CC) -std=c11 -Wall -Wextra -Werror -Ithird_party/vulkan-headers/include -Isrc src/color_attachment_contract.c tests/test_color_attachment_contract.c -o build/tests/test_color_attachment_contract
	./build/tests/test_color_attachment_contract
	$(CC) -std=c11 -Wall -Wextra -Werror -Ithird_party/vulkan-headers/include -Isrc tests/test_sample_rate_contract.c -o build/tests/test_sample_rate_contract
	./build/tests/test_sample_rate_contract
	$(CC) -std=c11 -Wall -Wextra -Werror -Isrc src/sample_rate_oracle.c tests/test_sample_rate_oracle.c -o build/tests/test_sample_rate_oracle
	./build/tests/test_sample_rate_oracle
	$(PYTHON) tools/build_sdk.py
	$(CC) -std=c11 -Wall -Wextra -Werror -I./dist-sdk/include -I./cts cts/cts_adapter.c dist-sdk/lib/libps5vk_host.a -o build/tests/test_cts_host
	./build/tests/test_cts_host
	$(MAKE) check-graphics-stages
	$(MAKE) check-upstream-cts-sink
	$(PYTHON) tools/check_upstream_selection.py
	@if [ -d third_party/psbc-reference ]; then \
		$(MAKE) test-compiler; \
	else \
		echo "Compiler dependencies (third_party/psbc-reference) not present; skipping host runtime compiler integration tests."; \
	fi
build/libpsbc.host.a:
	$(PYTHON) tools/build_psbc.py --host
.PHONY: test-runtime-header
.PHONY: test-tessellation-compiler
# The tessellation compiler contract asserts what the pinned dependency really
# produces for the pinned tessellation fixtures; it needs the compiler, not the
# native payload, and is wired into the dependency-gated chain below.
test-tessellation-compiler: build/libpsbc.host.a graphics-stage-shaders
	mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror $(RUNTIME_HEADER_SANITIZERS) $(VULKAN_CFLAGS) -Isrc -Inative -I$(LAB_SIBLINGS)/ps5-agc-gears/src -Ithird_party/psbc-reference native/runtime_shader.c tests/test_tessellation_compiler.c build/libpsbc.host.a -lstdc++ -lm -lpthread -o build/tests/test_tessellation_compiler
	./build/tests/test_tessellation_compiler
.PHONY: test-runtime-graphics-compiler
test-runtime-graphics-compiler: inspect-graphics-compiler graphics-stage-shaders
	mkdir -p build/tests
	$(PYTHON) tools/build_resolve_shaders.py --out build/resolve/resolve_spirv.h \
		--compiler $(GLSLANG)
	$(CC) -std=c11 -Wall -Wextra -Werror $(RUNTIME_HEADER_SANITIZERS) $(VULKAN_CFLAGS) -Isrc -Inative -Ibuild/resolve -I$(LAB_SIBLINGS)/ps5-agc-gears/src -I$(LAB_SIBLINGS)/ps5-agc-gears/include -Ithird_party/psbc-reference native/runtime_shader.c native/runtime_graphics_compiler.c native/runtime_graphics_cache.c native/resolve_program.c src/color_attachment_contract.c src/spirv_graphics_interface.c src/vertex_format_probe.c src/texture_format.c src/compilation_cache.c src/ps5_compiler_shims.c tests/test_runtime_graphics_compiler.c build/libpsbc.host.a -lstdc++ -lm -lpthread -o build/tests/test_runtime_graphics_compiler
	./build/tests/test_runtime_graphics_compiler
.PHONY: test-runtime-graphics-native
test-runtime-graphics-native:
	mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc -Inative -I$(LAB_SIBLINGS)/ps5-agc-gears/src -I$(LAB_SIBLINGS)/ps5-agc-gears/include -Ithird_party/psbc-reference native/runtime_shader.c native/runtime_graphics_compiler.c src/color_attachment_contract.c src/spirv_graphics_interface.c native/runtime_graphics_ps5.c native/graphics_pipeline_ps5.c native/tess_shared_storage.c src/texture_format.c src/ps5_compiler_shims.c tests/test_runtime_graphics_native.c build/libpsbc.host.a -lstdc++ -lm -lpthread -o build/tests/test_runtime_graphics_native
	./build/tests/test_runtime_graphics_native
test-runtime-header:
	mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror $(RUNTIME_HEADER_SANITIZERS) -Inative -I$(LAB_SIBLINGS)/ps5-agc-gears/src -I$(LAB_SIBLINGS)/ps5-agc-gears/include -Ithird_party/psbc-reference native/runtime_shader.c tests/test_runtime_shader.c -o build/tests/test_runtime_shader
	./build/tests/test_runtime_shader
test-compiler: build/libpsbc.host.a test-shaders
	$(MAKE) test-runtime-header
	$(MAKE) test-tessellation-compiler
	$(MAKE) test-runtime-graphics-compiler
	$(MAKE) test-runtime-graphics-native
	mkdir -p build/tests
	$(GLSLANG) -V --target-env vulkan1.0 -S comp experiments/compute/cts_ssbo_local_barrier.comp -o build/test-shaders/cts_ssbo_local_barrier.spv
	$(GLSLANG) -V --target-env vulkan1.2 -S comp experiments/compute/t08_subgroup_int8_iadd_runtime.comp -o build/test-shaders/t08_subgroup_int8_iadd_runtime.spv
	$(GLSLANG) -V --target-env vulkan1.2 -S comp experiments/compute/t08_subgroup_iadd_runtime.comp -o build/test-shaders/t08_subgroup_iadd_runtime.spv
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc -Iinclude -Ithird_party/psbc-reference -Ithird_party/opengnm/include src/ps5vk_compiler.c src/ps5_compiler_shims.c tests/test_runtime_compiler.c build/libpsbc.host.a -lstdc++ -lm -lpthread -o build/tests/test_runtime_compiler
	./build/tests/test_runtime_compiler
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc -Iinclude -Ithird_party/psbc-reference -Ithird_party/opengnm/include $(VK_DEVICE_SOURCES) src/platform_host.c src/ps5vk_compiler.c src/ps5_compiler_shims.c tests/test_runtime_pipeline_cache.c build/libpsbc.host.a -lstdc++ -lm -lpthread -o build/tests/test_runtime_pipeline_cache
	./build/tests/test_runtime_pipeline_cache
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc -Iinclude -Ithird_party/psbc-reference -Ithird_party/opengnm/include $(VK_DEVICE_SOURCES) src/platform_host.c src/ps5vk_compiler.c src/ps5_compiler_shims.c tests/test_local_size_id.c build/libpsbc.host.a -lstdc++ -lm -lpthread -o build/tests/test_local_size_id
	./build/tests/test_local_size_id
doctor:
	$(PYTHON) tools/lab.py doctor
compiler-control:
	$(PYTHON) tools/compile_control.py
native-bootstrap:
	$(PYTHON) tools/build_native.py
# Clip/cull distance declarations are an interface-policy contract: the
# fixtures need the pinned front end and the pinned headers, not PSBC, so this
# gate runs with the host contracts instead of the compiler integration tests.
.PHONY: check-graphics-stages graphics-stage-shaders
graphics-stage-shaders:
	mkdir -p build/runtime-graphics
	$(GLSLANG) -V experiments/graphics/runtime_triangle.vert -o build/runtime-graphics/triangle.vert.spv
	$(GLSLANG) -V experiments/graphics/runtime_triangle.frag -o build/runtime-graphics/triangle.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_dual_source.frag -o build/runtime-graphics/dual_source.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_two_mrt.frag -o build/runtime-graphics/two_mrt.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_gather_core.frag -o build/runtime-graphics/gather_core.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_gather_const_offset.frag -o build/runtime-graphics/gather_const_offset.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_gather_dynamic_offset.frag -o build/runtime-graphics/gather_dynamic_offset.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_gather_four_offsets.frag -o build/runtime-graphics/gather_four_offsets.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_gather_component_0.frag -o build/runtime-graphics/gather_component_0.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_gather_component_1.frag -o build/runtime-graphics/gather_component_1.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_gather_component_2.frag -o build/runtime-graphics/gather_component_2.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_gather_component_3.frag -o build/runtime-graphics/gather_component_3.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_gather_dref.frag -o build/runtime-graphics/gather_dref.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_gather_uint.frag -o build/runtime-graphics/gather_uint.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_gather_sint.frag -o build/runtime-graphics/gather_sint.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_clip_distance.vert -o build/runtime-graphics/clip_distance.vert.spv
	$(GLSLANG) -V experiments/graphics/runtime_cull_distance.vert -o build/runtime-graphics/cull_distance.vert.spv
	$(GLSLANG) -V experiments/graphics/runtime_clip_cull_distance.vert -o build/runtime-graphics/clip_cull_distance.vert.spv
	$(GLSLANG) -V experiments/graphics/runtime_clip_distance_read.frag -o build/runtime-graphics/clip_distance_read.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_frag_coord.frag -o build/runtime-graphics/frag_coord.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_subpass_write.frag -o build/runtime-graphics/runtime_subpass_write.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_subpass_write_spread.frag -o build/runtime-graphics/runtime_subpass_write_spread.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_subpass_fetch.frag -o build/runtime-graphics/runtime_subpass_fetch.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_subpass_fetch_const.frag -o build/runtime-graphics/runtime_subpass_fetch_const.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_subpass_resolve.frag -o build/runtime-graphics/runtime_subpass_resolve.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_depth_only.frag -o build/runtime-graphics/depth_only.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_depth_kill.frag -o build/runtime-graphics/depth_kill.frag.spv
	$(GLSLANG) -V --target-env vulkan1.3 experiments/graphics/runtime_depth_kill.frag -o build/runtime-graphics/depth_terminate.frag.spv
	$(GLSLANG) -V --target-env vulkan1.3 -DDEMOTE=1 experiments/graphics/runtime_depth_kill.frag -o build/runtime-graphics/depth_demote.frag.spv
	$(GLSLANG) -V -DREMOVE=0 experiments/graphics/runtime_helper_derivative.frag -o build/runtime-graphics/helper_control.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_helper_derivative.frag -o build/runtime-graphics/helper_kill.frag.spv
	$(GLSLANG) -V --target-env vulkan1.3 -DDEMOTE=1 experiments/graphics/runtime_helper_derivative.frag -o build/runtime-graphics/helper_demote.frag.spv
	$(GLSLANG) -V --target-env vulkan1.1 -DDEMOTE=1 experiments/graphics/runtime_helper_derivative.frag -o build/runtime-graphics/helper_demote_ext.frag.spv
	$(GLSLANG) -V --target-env vulkan1.3 experiments/graphics/runtime_helper_derivative.frag -o build/runtime-graphics/helper_terminate.frag.spv
	$(GLSLANG) -V -DTERMINATE_KHR=1 experiments/graphics/runtime_helper_derivative.frag -o build/runtime-graphics/helper_terminate_khr.frag.spv
	$(GLSLANG) -V -DWITH_DISTANCES=1 experiments/graphics/runtime_clip_cull_probe.vert -o build/runtime-graphics/clip_cull_probe.vert.spv
	$(GLSLANG) -V experiments/graphics/runtime_clip_cull_probe.vert -o build/runtime-graphics/clip_cull_control.vert.spv
	$(GLSLANG) -V experiments/graphics/runtime_geometry_probe.vert -o build/runtime-graphics/geometry_probe.vert.spv
	$(GLSLANG) -V experiments/graphics/runtime_geometry_identity.vert -o build/runtime-graphics/geometry_identity.vert.spv
	$(GLSLANG) -V -S geom experiments/graphics/runtime_geometry_probe.geom -o build/runtime-graphics/geometry_probe.geom.spv
	$(GLSLANG) -V -S geom experiments/graphics/runtime_geometry_envelope.geom -o build/runtime-graphics/geometry_envelope.geom.spv
	$(GLSLANG) -V -S geom experiments/graphics/runtime_geometry_invocations.geom -o build/runtime-graphics/geometry_invocations.geom.spv
	$(GLSLANG) -V -S geom experiments/graphics/runtime_geometry_primitive_id.geom -o build/runtime-graphics/geometry_primitive_id.geom.spv
	$(GLSLANG) -V -S geom experiments/graphics/runtime_geometry_points.geom -o build/runtime-graphics/geometry_points.geom.spv
	$(GLSLANG) -V -S geom experiments/graphics/runtime_geometry_lines.geom -o build/runtime-graphics/geometry_lines.geom.spv
	$(GLSLANG) -V experiments/graphics/runtime_primitive_restart.vert -o build/runtime-graphics/primitive_restart.vert.spv
	$(GLSLANG) -V experiments/graphics/runtime_primitive_restart.frag -o build/runtime-graphics/primitive_restart.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_geometry_family.vert -o build/runtime-graphics/geometry_family.vert.spv
	$(GLSLANG) -V experiments/graphics/runtime_geometry_components.vert -o build/runtime-graphics/geometry_components.vert.spv
	$(GLSLANG) -V -S geom experiments/graphics/runtime_geometry_components.geom -o build/runtime-graphics/geometry_components.geom.spv
	$(GLSLANG) -V experiments/graphics/runtime_geometry_output_components.frag -o build/runtime-graphics/geometry_output_components.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_geometry_uniform.vert -o build/runtime-graphics/geometry_uniform.vert.spv
	$(GLSLANG) -V -S geom experiments/graphics/runtime_geometry_uniform.geom -o build/runtime-graphics/geometry_uniform.geom.spv
	$(GLSLANG) -V experiments/graphics/runtime_raster_witness.vert -o build/runtime-graphics/raster_witness.vert.spv
	$(GLSLANG) -V -S geom experiments/graphics/runtime_raster_viewport_index.geom -o build/runtime-graphics/raster_viewport_index.geom.spv
	$(GLSLANG) -V experiments/graphics/runtime_tess.vert -o build/runtime-graphics/tess.vert.spv
	$(GLSLANG) -V -S tesc experiments/graphics/runtime_tess.tesc -o build/runtime-graphics/tess.tesc.spv
	$(GLSLANG) -V -S tese experiments/graphics/runtime_tess.tese -o build/runtime-graphics/tess.tese.spv
	$(GLSLANG) -V experiments/graphics/runtime_tess_output_envelope.tese -o build/runtime-graphics/tess_output_envelope.tese.spv
	$(GLSLANG) -V experiments/graphics/runtime_tess_output_envelope.frag -o build/runtime-graphics/tess_output_envelope.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_tess_quad.tesc -o build/runtime-graphics/tess_quad.tesc.spv
	$(GLSLANG) -V experiments/graphics/runtime_tess_coord.vert -o build/runtime-graphics/tess_coord.vert.spv
	$(GLSLANG) -V experiments/graphics/runtime_tess_coord.frag -o build/runtime-graphics/tess_coord.frag.spv
	$(GLSLANG) -V experiments/graphics/runtime_tess_points.tese -o build/runtime-graphics/tess_points.tese.spv
	$(GLSLANG) -V experiments/graphics/runtime_tess_points.geom -o build/runtime-graphics/tess_points.geom.spv
	$(GLSLANG) -V experiments/graphics/runtime_tess.frag -o build/runtime-graphics/tess.frag.spv
check-graphics-stages: graphics-stage-shaders
	mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/spirv_graphics_interface.c src/texture_format.c tests/test_graphics_stages.c -o build/tests/test_graphics_stages
	./build/tests/test_graphics_stages
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/spirv_graphics_interface.c src/texture_format.c tests/test_tessellation_stage.c -o build/tests/test_tessellation_stage
	./build/tests/test_tessellation_stage
# Genuine upstream VK-GL-CTS: cross-compile the focused native payload.
# Host-only contract checks for the upstream CTS selection and verifier. These
# never require the console, so CI can run them.
# check runs the Python half itself through tools/run_python_tests.py, so it
# only needs the QPA sink test from here.
check-upstream-cts-sink:
	mkdir -p build/tests
	$(CXX) -std=c++17 -Wall -Wextra -Werror -I$(LAB_SIBLINGS)/logging_server/client cts/upstream/log_sink_ps5.cpp tests/test_qpa_sink.cpp -Wl,--wrap=fopen,--wrap=fprintf,--wrap=fputs,--wrap=fputc,--wrap=fwrite,--wrap=fseek,--wrap=fflush,--wrap=fclose -o build/tests/test_qpa_sink
	./build/tests/test_qpa_sink

check-upstream-cts: check-upstream-cts-sink
	$(PYTHON) tools/check_upstream_selection.py
	$(PYTHON) -m unittest tests.test_upstream_runner tests.test_upstream_run_orchestrator -v
	$(PYTHON) -m unittest tests.test_color_attachment_offsets -v
upstream-cts:
	$(PYTHON) tools/build_upstream_cts.py
# One native acceptance run against the console. Both values are lab-specific,
# so they are passed in rather than hard-coded here.
upstream-cts-run:
	@test -n "$(PS5_HOST)" || { echo "PS5_HOST is required" >&2; exit 2; }
	@test -n "$(LOGS_RUNS_DIR)" || { echo "LOGS_RUNS_DIR is required" >&2; exit 2; }
	$(PYTHON) tools/run_upstream_cts.py --host "$(PS5_HOST)" --runs-dir "$(LOGS_RUNS_DIR)"

.PHONY: check-bc-subresource-copy
check-bc-subresource-copy:
	mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) src/platform_host.c src/depth_layout.c native/image_ps5.c tests/test_bc_subresource_copy.c -o build/tests/test_bc_subresource_copy
	./build/tests/test_bc_subresource_copy
