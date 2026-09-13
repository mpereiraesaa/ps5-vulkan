#ifndef PS5VK_COMMAND_H
#define PS5VK_COMMAND_H
#include "vk_pipeline.h"
#include "vk_framebuffer.h"
enum ps5vk_command_state { PS5VK_INITIAL, PS5VK_RECORDING, PS5VK_EXECUTABLE, PS5VK_PENDING, PS5VK_INVALID };
enum ps5vk_operation_type {
    PS5VK_DISPATCH, PS5VK_BARRIER, PS5VK_BEGIN_RENDER_PASS, PS5VK_DRAW,
    PS5VK_END_RENDER_PASS, PS5VK_DRAW_INDEXED, PS5VK_COPY_BUFFER_IMAGE,
    PS5VK_IMAGE_BARRIER, PS5VK_COPY_IMAGE_BUFFER, PS5VK_EVENT_SET,
    PS5VK_EVENT_RESET, PS5VK_EVENT_WAIT, PS5VK_COPY_BUFFER,
    PS5VK_UPDATE_BUFFER, PS5VK_FILL_BUFFER, PS5VK_DISPATCH_INDIRECT,
    PS5VK_DRAW_INDIRECT, PS5VK_DRAW_INDEXED_INDIRECT
};
enum ps5vk_operation_scope {
    PS5VK_OPERATION_OUTSIDE_RENDER_PASS,
    PS5VK_OPERATION_INSIDE_RENDER_PASS,
    PS5VK_OPERATION_ANYWHERE
};
enum { PS5VK_MAX_OPERATIONS = 64 };
enum { PS5VK_MAX_VERTEX_BINDINGS = 16 };
struct ps5vk_vertex_binding { VkBuffer buffer; VkDeviceSize offset; };
struct ps5vk_index_binding { VkBuffer buffer; VkDeviceSize offset; VkIndexType type; };
struct ps5vk_operation {
    enum ps5vk_operation_type type;
    void *owned_payload;
    size_t owned_payload_size;
    VkAllocationCallbacks payload_allocator;
    VkBool32 custom_payload_allocator;
    VkEvent event;
    VkImageMemoryBarrier image_barrier;
    VkBuffer copy_source;
    VkBuffer copy_destination;
    VkBufferCopy buffer_copy;
    VkDeviceSize buffer_offset;
    VkDeviceSize buffer_size;
    VkBuffer indirect_buffer;
    VkDeviceSize indirect_offset;
    uint32_t indirect_count;
    uint32_t indirect_stride;
    uint32_t fill_data;
    VkImage copy_image;
    VkImageLayout copy_layout;
    VkBufferImageCopy copy_region;
    VkRenderPass render_pass;
    VkFramebuffer framebuffer;
    VkRect2D render_area;
    VkViewport viewport;
    VkRect2D scissor;
    VkClearValue clears[2];
    uint32_t clear_count;
    uint32_t vertex_count, instance_count, first_vertex, first_instance;
    uint32_t index_count, first_index;
    int32_t vertex_offset;
    struct ps5vk_index_binding indices;
    struct ps5vk_vertex_binding vertices[PS5VK_MAX_VERTEX_BINDINGS];
    VkPipeline pipeline;
    VkDescriptorSet sets[PS5VK_MAX_SETS];
    uint64_t generations[PS5VK_MAX_SETS];
    uint32_t groups[3];
    uint32_t push_constant_size;
    uint8_t push_constants[PS5VK_MAX_PUSH_CONSTANT_BYTES];
    VkPipelineStageFlags src_stage, dst_stage;
    VkAccessFlags src_access, dst_access;
    VkBufferMemoryBarrier buffer_barrier;
};
struct VkCommandBuffer_T {
    VkCommandPool pool;
    struct VkCommandBuffer_T *next;
    enum ps5vk_command_state state;
    uint32_t pending_count;
    VkCommandBufferUsageFlags usage;
    VkPipeline pipeline;
    VkDescriptorSet sets[PS5VK_MAX_SETS];
    struct ps5vk_set_signature set_signatures[PS5VK_MAX_SETS];
    VkPipeline graphics_pipeline;
    VkDescriptorSet graphics_sets[PS5VK_MAX_SETS];
    struct ps5vk_set_signature graphics_set_signatures[PS5VK_MAX_SETS];
    VkBool32 push_constants_valid;
    VkShaderStageFlags push_constant_stages[PS5VK_MAX_PUSH_CONSTANT_DWORDS];
    uint8_t push_constants[PS5VK_MAX_PUSH_CONSTANT_BYTES];
    struct ps5vk_index_binding indices;
    struct ps5vk_vertex_binding vertices[PS5VK_MAX_VERTEX_BINDINGS];
    VkRenderPass render_pass;
    VkFramebuffer framebuffer;
    VkViewport viewport;
    VkRect2D scissor;
    VkBool32 viewport_valid, scissor_valid;
    /* Core dynamic state is retained independently of pipeline support.  A
     * value becomes executable only when pipeline creation explicitly accepts
     * the corresponding VkDynamicState; unsupported pipeline state therefore
     * remains fail-closed instead of being silently ignored. */
    float line_width;
    float depth_bias_constant, depth_bias_clamp, depth_bias_slope;
    float blend_constants[4];
    float min_depth_bounds, max_depth_bounds;
    uint32_t stencil_compare_mask[2];
    uint32_t stencil_write_mask[2];
    uint32_t stencil_reference[2];
    VkStencilFaceFlags stencil_compare_faces;
    VkStencilFaceFlags stencil_write_faces;
    VkStencilFaceFlags stencil_reference_faces;
    uint32_t dynamic_state_valid;
    unsigned operation_count;
    struct ps5vk_operation operations[PS5VK_MAX_OPERATIONS];
};
struct VkCommandPool_T {
    VkDevice device;
    VkAllocationCallbacks allocator;
    VkBool32 custom_allocator;
    VkCommandPoolCreateFlags flags;
    VkCommandBuffer buffers;
    struct VkCommandPool_T *next;
};

/* Internal recording contract shared by command-domain modules.  Validation is
 * transactional: failure invalidates the command buffer without consuming a
 * slot; success zeroes and types every reserved operation before publishing
 * the new operation_count. */
void ps5vk_command_invalidate(VkCommandBuffer command);
struct ps5vk_operation *ps5vk_command_reserve_operations(
    VkCommandBuffer command, enum ps5vk_operation_type type,
    enum ps5vk_operation_scope scope, uint32_t count);
struct ps5vk_operation *ps5vk_command_reserve_operation_with_payload(
    VkCommandBuffer command, enum ps5vk_operation_type type,
    enum ps5vk_operation_scope scope, const void *data, size_t size);
#endif
