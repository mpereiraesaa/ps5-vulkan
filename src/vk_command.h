#ifndef PS5VK_COMMAND_H
#define PS5VK_COMMAND_H
#include "vk_pipeline.h"
#include "vk_framebuffer.h"
enum ps5vk_command_state { PS5VK_INITIAL, PS5VK_RECORDING, PS5VK_EXECUTABLE, PS5VK_PENDING, PS5VK_INVALID };
enum ps5vk_operation_type {
    PS5VK_DISPATCH, PS5VK_BARRIER, PS5VK_BEGIN_RENDER_PASS, PS5VK_DRAW,
    PS5VK_END_RENDER_PASS, PS5VK_DRAW_INDEXED, PS5VK_COPY_BUFFER_IMAGE,
    /* Boundary between two subpasses of one render pass. It carries no work
     * of its own; it records that the scope changed, so submission can derive
     * the subpass structure from the immutable record. */
    PS5VK_NEXT_SUBPASS,
    PS5VK_IMAGE_BARRIER, PS5VK_COPY_IMAGE_BUFFER, PS5VK_EVENT_SET,
    PS5VK_EVENT_RESET, PS5VK_EVENT_WAIT, PS5VK_COPY_BUFFER,
    PS5VK_UPDATE_BUFFER, PS5VK_FILL_BUFFER, PS5VK_DISPATCH_INDIRECT,
    PS5VK_DRAW_INDIRECT, PS5VK_DRAW_INDEXED_INDIRECT, PS5VK_QUERY_RESET,
    PS5VK_QUERY_BEGIN, PS5VK_QUERY_END, PS5VK_QUERY_COPY,
    /* Frontend image domain: executed in submission order by start_submission
     * when the segment reaches the head, like the buffer transfers. */
    PS5VK_COPY_IMAGE, PS5VK_CLEAR_COLOR_IMAGE,
    /* Backend image domain: the tiled depth target is device memory the host
     * never writes, so a depth clear is emitted as GPU packets and completes
     * against the segment's label, like the buffer-to-image upload. */
    PS5VK_CLEAR_DEPTH_STENCIL_IMAGE,
    /* Ordered references to secondary command buffers. The children are never
     * copied or flattened into the primary: this operation only NAMES them,
     * and the queue expands each name into its own submission segment, so a
     * child keeps its object identity, its pending ownership and its reuse
     * rules instead of a parallel mechanism inventing them. */
    PS5VK_EXECUTE_COMMANDS,
    PS5VK_CLEAR_ATTACHMENT
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
    /* Owned child references for PS5VK_EXECUTE_COMMANDS: the array itself is
     * the operation's owned_payload and this is its element count. */
    uint32_t child_count;
    VkQueryPool query_pool;
    uint32_t query_first;
    uint32_t query_count;
    VkQueryControlFlags query_flags;
    VkDeviceSize query_stride;
    /* Image copy / colour clear domain. Region and range arrays live in
     * owned_payload; the clear value is stored as canonical RGBA8 bytes. */
    VkImage image_source, image_destination;
    VkImageLayout image_source_layout, image_destination_layout;
    uint32_t image_region_count;
    uint32_t clear_word;
    VkClearRect clear_rect;
    VkImage copy_image;
    VkImageLayout copy_layout;
    VkBufferImageCopy copy_region;
    VkRenderPass render_pass;
    VkFramebuffer framebuffer;
    /* Contents mode recorded by PS5VK_BEGIN_RENDER_PASS. INLINE means the
     * pass carries its own draws; SECONDARY_COMMAND_BUFFERS means the only
     * work inside it is named by vkCmdExecuteCommands. Kept on the operation,
     * not only on the recording buffer, so submission can re-check the rule
     * against the immutable record instead of trusting record-time state. */
    VkSubpassContents render_pass_contents;
    /* Subpass this operation belongs to: the one a draw executes in, the one a
     * boundary opens, or the one a pass begins or ends at. */
    uint32_t subpass;
    VkRect2D render_area;
    /* The viewport/scissor arrays this draw executes with, resolved at record
     * time by value (pipeline or command-buffer arrays, index by index); the
     * single names alias index zero. */
    uint32_t viewport_count;
    union {
        VkViewport viewport;
        VkViewport viewports[PS5VK_MAX_VIEWPORTS];
    };
    union {
        VkRect2D scissor;
        VkRect2D scissors[PS5VK_MAX_VIEWPORTS];
    };
    /* Rasterization state resolved at record time (vk_pipeline.h). */
    struct ps5vk_raster_state raster;
    /* One clear value per attachment the pass may name: the pinned multisample
     * oracle clears its whole pass in one begin, so a fixed pair of slots
     * cannot carry the begin it records. The count is the caller's, bounded by
     * the same attachment bound the render pass enforces (DXVK262-T06). */
    VkClearValue clears[PS5VK_MAX_ATTACHMENTS];
    uint32_t clear_count;
    uint32_t vertex_count, instance_count, first_vertex, first_instance;
    uint32_t index_count, first_index;
    int32_t vertex_offset;
    /* DrawIndex the vertex stage observes: zero for every recorded direct
     * draw, and the index of the command inside a vkCmdDraw*Indirect call for
     * the resolved snapshot of that command (vk_indirect.c). A recorded
     * indirect operation itself always carries zero; the value is assigned
     * per command at queue-head resolution and never accumulates across
     * commands. */
    uint32_t draw_index;
    struct ps5vk_index_binding indices;
    struct ps5vk_vertex_binding vertices[PS5VK_MAX_VERTEX_BINDINGS];
    VkPipeline pipeline;
    VkDescriptorSet sets[PS5VK_MAX_SETS];
    uint64_t generations[PS5VK_MAX_SETS];
    /* Parallel to VkPipeline_T::program.descriptors.  Static descriptors carry
     * zero; dynamic buffer descriptors carry the offset captured at bind. */
    VkDeviceSize descriptor_dynamic_offsets[PS5VK_MAX_DESCRIPTORS];
    /* Graphics runtime tables use set-local flattened elements, independently
     * of the precompiled program's optional descriptor enumeration. */
    VkDeviceSize graphics_dynamic_offsets[PS5VK_MAX_SETS][PS5VK_MAX_DESCRIPTORS];
    uint32_t groups[3];
    uint32_t group_base[3];
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
    /* Fixed at allocation and never reset: Vulkan has no operation that
     * changes a command buffer's level, so vkResetCommandBuffer and pool
     * resets must leave it alone. */
    VkCommandBufferLevel level;
    /* Copy of the secondary's VkCommandBufferInheritanceInfo, taken at
     * vkBeginCommandBuffer. Owned so a caller mutating its own structure
     * afterwards cannot change what was recorded; pNext is refused rather
     * than shallow-copied, because a retained pointer is not owned data. */
    VkBool32 inheritance_valid;
    VkCommandBufferInheritanceInfo inheritance;
    VkPipeline pipeline;
    VkDescriptorSet sets[PS5VK_MAX_SETS];
    struct ps5vk_set_signature set_signatures[PS5VK_MAX_SETS];
    VkDeviceSize set_dynamic_offsets[PS5VK_MAX_SETS][PS5VK_MAX_DESCRIPTORS];
    VkPipeline graphics_pipeline;
    VkDescriptorSet graphics_sets[PS5VK_MAX_SETS];
    struct ps5vk_set_signature graphics_set_signatures[PS5VK_MAX_SETS];
    VkDeviceSize graphics_set_dynamic_offsets[PS5VK_MAX_SETS][PS5VK_MAX_DESCRIPTORS];
    VkBool32 push_constants_valid;
    VkShaderStageFlags push_constant_stages[PS5VK_MAX_PUSH_CONSTANT_DWORDS];
    uint8_t push_constants[PS5VK_MAX_PUSH_CONSTANT_BYTES];
    struct ps5vk_index_binding indices;
    struct ps5vk_vertex_binding vertices[PS5VK_MAX_VERTEX_BINDINGS];
    VkRenderPass render_pass;
    VkFramebuffer framebuffer;
    VkQueryPool active_occlusion_query_pool;
    uint32_t active_occlusion_query;
    /* The active render pass was INHERITED by a continuation secondary rather
     * than begun here. Draws record against it exactly as they would in a
     * primary, but this buffer never began it: vkCmdEndRenderPass cannot close
     * it and vkEndCommandBuffer may end with it still active. */
    VkBool32 render_pass_inherited;
    /* Contents mode of the subpass this buffer is currently recording, which
     * each vkCmdNextSubpass replaces; meaningless while the pass is inherited. */
    VkSubpassContents render_pass_contents;
    /* Index of the current subpass: advanced by vkCmdNextSubpass, taken from
     * the inheritance record by a continuation secondary, and reset with the
     * rest of the recording state. */
    uint32_t subpass;
    /* Dynamic viewport/scissor arrays. Bit i of each mask says index i was set
     * by vkCmdSetViewport/vkCmdSetScissor since the last reset; a draw needs
     * every index below its pipeline's viewport_count. The single names alias
     * index zero. */
    union {
        VkViewport viewport;
        VkViewport viewports[PS5VK_MAX_VIEWPORTS];
    };
    union {
        VkRect2D scissor;
        VkRect2D scissors[PS5VK_MAX_VIEWPORTS];
    };
    uint32_t viewport_valid, scissor_valid;
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
/* Vulkan 1.0 render-pass compatibility: matching attachment references,
 * formats and sample counts. Load/store ops and layouts are deliberately not
 * compared, and identity is not required. */
VkBool32 ps5vk_render_pass_compatible(VkRenderPass a, VkRenderPass b);
struct ps5vk_operation *ps5vk_command_reserve_operations(
    VkCommandBuffer command, enum ps5vk_operation_type type,
    enum ps5vk_operation_scope scope, uint32_t count);
struct ps5vk_operation *ps5vk_command_reserve_operation_with_payload(
    VkCommandBuffer command, enum ps5vk_operation_type type,
    enum ps5vk_operation_scope scope, const void *data, size_t size);
#endif
