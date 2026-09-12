PYTHON ?= python3
CC ?= cc
.DEFAULT_GOAL := check
VULKAN_CFLAGS ?= -Ithird_party/vulkan-headers/include
VK_MEMORY_SOURCES = src/vk_alloc.c src/vk_memory.c
VK_IMAGE_TEST_SOURCES = $(VK_MEMORY_SOURCES) src/vk_image_view.c src/vk_render_pass.c src/vk_framebuffer.c
VK_DESCRIPTOR_SOURCES = $(VK_MEMORY_SOURCES) src/vk_descriptor.c
VK_PIPELINE_SOURCES = $(VK_DESCRIPTOR_SOURCES) src/vk_pipeline.c src/compilation_cache.c
VK_COMMAND_SOURCES = $(VK_PIPELINE_SOURCES) src/vk_command.c
VK_QUEUE_SOURCES = $(VK_COMMAND_SOURCES) src/vk_fence.c src/vk_queue.c src/vk_queue_router.c
VK_GRAPHICS_SOURCES = src/vk_image_view.c src/vk_sampler.c src/vk_render_pass.c src/vk_framebuffer.c src/vk_graphics_pipeline.c src/graphics_program.c src/vk_transfer.c src/texture_copy.c src/texture_layout.c
VK_DEVICE_SOURCES = $(VK_QUEUE_SOURCES) $(VK_GRAPHICS_SOURCES) src/vk_device.c src/vk_dispatch.c
NATIVE_PREPARE_TEST = -D_DEFAULT_SOURCE $(VULKAN_CFLAGS) -Isrc -I../ps5-agc-gears/include -I../logging_server/client native/queue_ps5.c src/descriptor_encode.c src/dispatch_encode.c src/compute_commands.c tests/test_native_prepare.c
GRAPHICS_PAIR_TEST = -Inative -Isrc -I../ps5-agc-gears/src -I../ps5-agc-gears/include native/graphics_pair.c src/shader_relocate.c ../ps5-agc-gears/src/ps5_shader_header.c tests/test_graphics_pair.c
.PHONY: check doctor compiler-control compiler-programs native-bootstrap vulkan-headers check-sanitize native-memory-check test-shaders
.PHONY: compiler-pipelines
.PHONY: native-compute native-graphics
native-compute:
	PS5VK_COMPUTE=1 $(PYTHON) tools/build_native.py
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
check-sanitize:
	mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Isrc src/vk_alloc.c src/vk_sampler.c tests/test_vk_sampler.c -o build/tests/test_vk_sampler_sanitized
	./build/tests/test_vk_sampler_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Isrc $(VK_IMAGE_TEST_SOURCES) tests/test_vk_image.c -o build/tests/test_vk_image_sanitized
	./build/tests/test_vk_image_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Isrc src/vk_alloc.c src/vk_render_pass.c tests/test_vk_render_pass.c -o build/tests/test_vk_render_pass_sanitized
	./build/tests/test_vk_render_pass_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined $(GRAPHICS_PAIR_TEST) -o build/tests/test_graphics_pair_sanitized
	./build/tests/test_graphics_pair_sanitized
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Isrc src/shader_relocate.c tests/test_shader_relocate.c -o build/tests/test_shader_relocate_sanitized
	./build/tests/test_shader_relocate_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined $(NATIVE_PREPARE_TEST) -o build/tests/test_native_prepare_sanitized
	./build/tests/test_native_prepare_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined $(VULKAN_CFLAGS) -Isrc src/descriptor_encode.c tests/test_descriptor_encode.c -o build/tests/test_descriptor_encode_sanitized
	./build/tests/test_descriptor_encode_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc src/compute_commands.c src/dispatch_encode.c tests/test_dispatch_encode.c -o build/tests/test_dispatch_encode_sanitized
	./build/tests/test_dispatch_encode_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc src/vk_alloc.c src/vk_memory.c tests/test_vk_memory.c -o build/tests/test_vk_memory_sanitized
	./build/tests/test_vk_memory_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) tests/test_vk_device.c -o build/tests/test_vk_device_sanitized
	./build/tests/test_vk_device_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_DESCRIPTOR_SOURCES) tests/test_vk_descriptor.c -o build/tests/test_vk_descriptor_sanitized
	./build/tests/test_vk_descriptor_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_PIPELINE_SOURCES) tests/test_vk_pipeline.c -o build/tests/test_vk_pipeline_sanitized
	./build/tests/test_vk_pipeline_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_COMMAND_SOURCES) tests/test_vk_command.c -o build/tests/test_vk_command_sanitized
	./build/tests/test_vk_command_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc src/vk_alloc.c src/vk_fence.c tests/test_vk_fence.c -o build/tests/test_vk_fence_sanitized
	./build/tests/test_vk_fence_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc $(VK_QUEUE_SOURCES) tests/test_vk_queue.c -o build/tests/test_vk_queue_sanitized
	./build/tests/test_vk_queue_sanitized
	$(CC) -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(VULKAN_CFLAGS) -Isrc src/compilation_cache.c tests/test_compilation_cache.c -o build/tests/test_compilation_cache_sanitized
	./build/tests/test_compilation_cache_sanitized
check:
	@mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/texture_dma.c tests/test_texture_dma.c -o build/tests/test_texture_dma
	./build/tests/test_texture_dma
	$(CC) -std=c11 -Wall -Wextra -Werror -Isrc src/color_clear.c tests/test_color_clear.c -o build/tests/test_color_clear
	./build/tests/test_color_clear
	$(CC) -std=c11 -Wall -Wextra -Werror -Isrc src/scene_geometry.c tests/test_scene_geometry.c -o build/tests/test_scene_geometry
	./build/tests/test_scene_geometry
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/image_layout_state.c tests/test_image_layout_state.c -o build/tests/test_image_layout_state
	./build/tests/test_image_layout_state
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/texture_copy.c src/texture_layout.c tests/test_texture_copy.c -o build/tests/test_texture_copy
	./build/tests/test_texture_copy
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_COMMAND_SOURCES) src/vk_transfer.c src/texture_copy.c src/vk_image_view.c src/vk_sampler.c src/texture_descriptor.c src/texture_layout.c src/depth_layout.c native/image_ps5.c tests/test_texture_descriptor.c -o build/tests/test_texture_descriptor
	./build/tests/test_texture_descriptor
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/texture_layout.c src/depth_layout.c native/image_ps5.c tests/test_texture_layout.c -o build/tests/test_texture_layout
	./build/tests/test_texture_layout
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/vk_alloc.c src/vk_sampler.c tests/test_vk_sampler.c -o build/tests/test_vk_sampler
	./build/tests/test_vk_sampler
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -Isrc -I../ps5-agc-gears/include native/index_emit_ps5.c tests/test_index_emit_ps5.c -o build/tests/test_index_emit_ps5
	./build/tests/test_index_emit_ps5
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_MEMORY_SOURCES) src/index_fetch.c tests/test_index_fetch.c -o build/tests/test_index_fetch
	./build/tests/test_index_fetch
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_MEMORY_SOURCES) src/vertex_descriptor.c src/vertex_fetch.c tests/test_vertex_fetch.c -o build/tests/test_vertex_fetch
	./build/tests/test_vertex_fetch
	$(CC) -std=c11 -Wall -Wextra -Werror -Isrc src/vertex_descriptor.c tests/test_vertex_descriptor.c -o build/tests/test_vertex_descriptor
	./build/tests/test_vertex_descriptor
	$(CC) -std=c11 -Wall -Wextra -Werror -Isrc src/triangle_readback.c tests/test_triangle_readback.c -o build/tests/test_triangle_readback
	./build/tests/test_triangle_readback
	@mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -Isrc -I../ps5-agc-gears/include native/command_arena_ps5.c tests/test_command_arena_ps5.c -o build/tests/test_command_arena_ps5
	./build/tests/test_command_arena_ps5
	@mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror -Isrc src/graphics_sync.c tests/test_graphics_sync.c -o build/tests/test_graphics_sync
	./build/tests/test_graphics_sync
	@mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -Isrc -I../ps5-agc-gears/src -I../ps5-agc-gears/include native/draw_prepare_ps5.c src/vertex_descriptor.c tests/test_draw_prepare_ps5.c -o build/tests/test_draw_prepare_ps5
	./build/tests/test_draw_prepare_ps5
	@mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -Isrc -I../ps5-agc-gears/src -I../ps5-agc-gears/include native/draw_emit_ps5.c native/index_emit_ps5.c ../ps5-agc-gears/src/ps5_agc_writer.c ../ps5-agc-gears/src/ps5_gpu_span.c tests/test_draw_emit_ps5.c -o build/tests/test_draw_emit_ps5
	./build/tests/test_draw_emit_ps5
	@mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -Isrc -I../ps5-agc-gears/src -I../ps5-agc-gears/include native/draw_state_ps5.c native/viewport_ps5.c ../ps5-agc-gears/src/ps5_pipeline.c tests/test_draw_state_ps5.c -o build/tests/test_draw_state_ps5
	./build/tests/test_draw_state_ps5
	@mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -I../ps5-agc-gears/include native/viewport_ps5.c tests/test_viewport_ps5.c -o build/tests/test_viewport_ps5
	./build/tests/test_viewport_ps5
	mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -Isrc -I../ps5-agc-gears/src -I../ps5-agc-gears/include native/graphics_pipeline_ps5.c tests/test_graphics_pipeline_backend.c -o build/tests/test_graphics_pipeline_backend
	./build/tests/test_graphics_pipeline_backend
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_PIPELINE_SOURCES) src/vk_graphics_pipeline.c src/graphics_program.c tests/test_vk_graphics_pipeline.c -o build/tests/test_vk_graphics_pipeline
	./build/tests/test_vk_graphics_pipeline
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/graphics_program.c tests/test_graphics_program.c -o build/tests/test_graphics_program
	./build/tests/test_graphics_program
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Inative -Isrc -I../ps5-agc-gears/src -I../ps5-agc-gears/include native/targets_ps5.c native/image_ps5.c src/depth_layout.c src/texture_layout.c ../ps5-agc-gears/src/ps5_color_target.c ../ps5-agc-gears/src/ps5_depth_target.c tests/test_targets_ps5.c -o build/tests/test_targets_ps5
	./build/tests/test_targets_ps5
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc -I../ps5-agc-gears/include -I../logging_server/client native/memory_ps5.c tests/test_native_memory_alignment.c -o build/tests/test_native_memory_alignment
	./build/tests/test_native_memory_alignment
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/depth_layout.c src/texture_layout.c native/image_ps5.c tests/test_depth_layout.c -o build/tests/test_depth_layout
	./build/tests/test_depth_layout
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_IMAGE_TEST_SOURCES) tests/test_vk_image.c -o build/tests/test_vk_image
	./build/tests/test_vk_image
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/vk_alloc.c src/vk_render_pass.c tests/test_vk_render_pass.c -o build/tests/test_vk_render_pass
	./build/tests/test_vk_render_pass
	$(CC) -std=c11 -Wall -Wextra -Werror $(GRAPHICS_PAIR_TEST) -o build/tests/test_graphics_pair
	./build/tests/test_graphics_pair
	$(CC) -std=c11 -Wall -Wextra -Werror -Isrc src/shader_relocate.c tests/test_shader_relocate.c -o build/tests/test_shader_relocate
	./build/tests/test_shader_relocate
	$(CC) -std=c11 -Wall -Wextra -Werror $(NATIVE_PREPARE_TEST) -o build/tests/test_native_prepare
	./build/tests/test_native_prepare
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/descriptor_encode.c tests/test_descriptor_encode.c -o build/tests/test_descriptor_encode
	./build/tests/test_descriptor_encode
	$(PYTHON) tools/prepare_vulkan_headers.py --check
	$(PYTHON) -m unittest discover -s tests -v
	$(CC) -std=c11 -Wall -Wextra -Werror -Inative -I../ps5-agc-gears/include tests/test_submit_suspend.c -o build/tests/test_submit_suspend
	./build/tests/test_submit_suspend
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/vk_queue_router.c tests/test_queue_router.c -o build/tests/test_queue_router
	./build/tests/test_queue_router
	$(CC) -std=c11 -Wall -Wextra -Werror -Isrc src/scene_region.c tests/test_scene_region.c -o build/tests/test_scene_region
	./build/tests/test_scene_region
	mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror -Isrc src/compute_check.c tests/test_compute_check.c -o build/tests/test_compute_check
	./build/tests/test_compute_check
	$(CC) -std=c11 -Wall -Wextra -Werror -Isrc src/compute_commands.c tests/test_compute_commands.c -o build/tests/test_compute_commands
	./build/tests/test_compute_commands
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/compute_commands.c src/dispatch_encode.c tests/test_dispatch_encode.c -o build/tests/test_dispatch_encode
	./build/tests/test_dispatch_encode
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/vk_alloc.c src/vk_memory.c tests/test_vk_memory.c -o build/tests/test_vk_memory
	./build/tests/test_vk_memory
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_DEVICE_SOURCES) tests/test_vk_device.c -o build/tests/test_vk_device
	./build/tests/test_vk_device
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_DESCRIPTOR_SOURCES) tests/test_vk_descriptor.c -o build/tests/test_vk_descriptor
	./build/tests/test_vk_descriptor
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_PIPELINE_SOURCES) tests/test_vk_pipeline.c -o build/tests/test_vk_pipeline
	./build/tests/test_vk_pipeline
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_COMMAND_SOURCES) tests/test_vk_command.c -o build/tests/test_vk_command
	./build/tests/test_vk_command
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/vk_alloc.c src/vk_fence.c tests/test_vk_fence.c -o build/tests/test_vk_fence
	./build/tests/test_vk_fence
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc $(VK_QUEUE_SOURCES) tests/test_vk_queue.c -o build/tests/test_vk_queue
	./build/tests/test_vk_queue
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc src/compilation_cache.c tests/test_compilation_cache.c -o build/tests/test_compilation_cache
	./build/tests/test_compilation_cache
	$(PYTHON) tools/build_sdk.py
	@if [ -d third_party/psbc-reference ]; then \
		$(MAKE) test-compiler; \
	else \
		echo "Compiler dependencies (third_party/psbc-reference) not present; skipping host runtime compiler integration tests."; \
	fi
build/libpsbc.host.a:
	$(PYTHON) tools/build_psbc.py --host
test-compiler: build/libpsbc.host.a test-shaders
	mkdir -p build/tests
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc -Iinclude -Ithird_party/psbc-reference -Ithird_party/opengnm/include src/ps5vk_compiler.c src/ps5_compiler_shims.c tests/test_runtime_compiler.c build/libpsbc.host.a -lstdc++ -lm -lpthread -o build/tests/test_runtime_compiler
	./build/tests/test_runtime_compiler
	$(CC) -std=c11 -Wall -Wextra -Werror $(VULKAN_CFLAGS) -Isrc -Iinclude -Ithird_party/psbc-reference -Ithird_party/opengnm/include $(VK_DEVICE_SOURCES) src/platform_host.c src/ps5vk_compiler.c src/ps5_compiler_shims.c tests/test_runtime_pipeline_cache.c build/libpsbc.host.a -lstdc++ -lm -lpthread -o build/tests/test_runtime_pipeline_cache
	./build/tests/test_runtime_pipeline_cache
doctor:
	$(PYTHON) tools/lab.py doctor
compiler-control:
	$(PYTHON) tools/compile_control.py
native-bootstrap:
	$(PYTHON) tools/build_native.py
