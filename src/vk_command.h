#ifndef PS5VK_COMMAND_H
#define PS5VK_COMMAND_H
#include "vk_pipeline.h"
#include "vk_framebuffer.h"
enum ps5vk_command_state { PS5VK_INITIAL, PS5VK_RECORDING, PS5VK_EXECUTABLE, PS5VK_PENDING, PS5VK_INVALID };
enum { PS5VK_MAX_OPERATIONS = 64 };
enum { PS5VK_MAX_VERTEX_BINDINGS = 16 };
struct ps5vk_vertex_binding { VkBuffer buffer; VkDeviceSize offset; };
struct ps5vk_index_binding { VkBuffer buffer; VkDeviceSize offset; VkIndexType type; };
struct ps5vk_operation {
    enum { PS5VK_DISPATCH, PS5VK_BARRIER, PS5VK_BEGIN_RENDER_PASS, PS5VK_DRAW, PS5VK_END_RENDER_PASS, PS5VK_DRAW_INDEXED, PS5VK_COPY_BUFFER_IMAGE, PS5VK_IMAGE_BARRIER, PS5VK_COPY_IMAGE_BUFFER } type;
    VkImageMemoryBarrier image_barrier;
    VkBuffer copy_source;
    VkBuffer copy_destination;
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
#endif
