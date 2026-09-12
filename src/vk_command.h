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
    enum { PS5VK_DISPATCH, PS5VK_BARRIER, PS5VK_BEGIN_RENDER_PASS, PS5VK_DRAW, PS5VK_END_RENDER_PASS, PS5VK_DRAW_INDEXED, PS5VK_COPY_BUFFER_IMAGE, PS5VK_IMAGE_BARRIER } type;
    VkImageMemoryBarrier image_barrier;
    VkBuffer copy_source;
    VkImage copy_image;
    VkImageLayout copy_layout;
    VkBufferImageCopy copy_region;
    VkRenderPass render_pass;
    VkFramebuffer framebuffer;
    VkRect2D render_area;
    VkClearValue clears[2];
    uint32_t clear_count;
    uint32_t vertex_count, instance_count, first_vertex, first_instance;
    uint32_t index_count, first_index;
    int32_t vertex_offset;
    struct ps5vk_index_binding indices;
    struct ps5vk_vertex_binding vertices[PS5VK_MAX_VERTEX_BINDINGS];
    VkPipeline pipeline;
    VkDescriptorSet set;
    uint64_t generation;
    uint32_t groups[3];
    VkPipelineStageFlags src_stage, dst_stage;
    VkAccessFlags src_access, dst_access;
};
struct VkCommandBuffer_T {
    VkCommandPool pool;
    struct VkCommandBuffer_T *next;
    enum ps5vk_command_state state;
    VkCommandBufferUsageFlags usage;
    VkPipeline pipeline;
    VkDescriptorSet set;
    struct ps5vk_set_signature set_signature;
    VkPipeline graphics_pipeline;
    VkDescriptorSet graphics_set;
    struct ps5vk_set_signature graphics_set_signature;
    struct ps5vk_index_binding indices;
    struct ps5vk_vertex_binding vertices[PS5VK_MAX_VERTEX_BINDINGS];
    VkRenderPass render_pass;
    VkFramebuffer framebuffer;
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
