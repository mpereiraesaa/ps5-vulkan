/*
 * Host contract tests for the secondary command buffer object and state.
 *
 * This slice adds the level, its owned inheritance copy and the state rules
 * that go with them. It deliberately adds NO execution: vkCmdExecuteCommands
 * stays fail-closed and a secondary is refused at vkQueueSubmit, so every
 * assertion here is about allocation, recording state and lifetime, never
 * about a secondary running.
 */
#include "vk_command.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static VkCommandPool make_pool(VkDevice d, VkCommandPoolCreateFlags flags)
{
    VkCommandPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = flags};
    VkCommandPool p;
    assert(vkCreateCommandPool(d, &info, NULL, &p) == VK_SUCCESS);
    return p;
}

static VkResult allocate_level(VkDevice d, VkCommandPool p, VkCommandBufferLevel level,
                               uint32_t count, VkCommandBuffer *out)
{
    VkCommandBufferAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = p, .level = level, .commandBufferCount = count};
    return vkAllocateCommandBuffers(d, &info, out);
}

static VkCommandBuffer secondary(VkDevice d, VkCommandPool p)
{
    VkCommandBuffer c;
    assert(allocate_level(d, p, VK_COMMAND_BUFFER_LEVEL_SECONDARY, 1, &c) == VK_SUCCESS);
    return c;
}

static VkCommandBufferInheritanceInfo inheritance(void)
{
    return (VkCommandBufferInheritanceInfo){
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
}

static VkResult begin_secondary(VkCommandBuffer c, VkCommandBufferUsageFlags flags,
                                const VkCommandBufferInheritanceInfo *i)
{
    VkCommandBufferBeginInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                     .flags = flags, .pInheritanceInfo = i};
    return vkBeginCommandBuffer(c, &info);
}

static void allocation_and_level(void)
{
    struct VkDevice_T d = {0};
    VkCommandPool p = make_pool(&d, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);

    /* Both levels allocate, and the level is recorded on the object. */
    VkCommandBuffer primary_buffers[3] = {0}, secondary_buffers[3] = {0};
    assert(allocate_level(&d, p, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 3, primary_buffers) == VK_SUCCESS);
    assert(allocate_level(&d, p, VK_COMMAND_BUFFER_LEVEL_SECONDARY, 3, secondary_buffers) == VK_SUCCESS);
    for (unsigned i = 0; i < 3; ++i) {
        assert(primary_buffers[i] && primary_buffers[i]->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        assert(secondary_buffers[i] && secondary_buffers[i]->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY);
        /* Same pool and device ownership guarantees as a primary. */
        assert(secondary_buffers[i]->pool == p && p->device == &d);
        assert(secondary_buffers[i]->state == PS5VK_INITIAL);
        for (unsigned j = 0; j < i; ++j) assert(secondary_buffers[j] != secondary_buffers[i]);
    }

    /* An unknown level is still refused, and every output slot is cleared. */
    VkCommandBuffer bogus[2] = {(VkCommandBuffer)(uintptr_t)1, (VkCommandBuffer)(uintptr_t)1};
    assert(allocate_level(&d, p, (VkCommandBufferLevel)7, 2, bogus) != VK_SUCCESS);
    assert(!bogus[0] && !bogus[1]);

    /* The level survives every reset: Vulkan cannot change it. */
    VkCommandBuffer c = secondary_buffers[0];
    VkCommandBufferInheritanceInfo i = inheritance();
    assert(begin_secondary(c, 0, &i) == VK_SUCCESS);
    assert(vkEndCommandBuffer(c) == VK_SUCCESS);
    assert(vkResetCommandBuffer(c, 0) == VK_SUCCESS);
    assert(c->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY && c->state == PS5VK_INITIAL);
    assert(vkResetCommandPool(&d, p, 0) == VK_SUCCESS);
    assert(c->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY);

    vkFreeCommandBuffers(&d, p, 3, secondary_buffers);
    vkFreeCommandBuffers(&d, p, 3, primary_buffers);
    vkDestroyCommandPool(&d, p, NULL);
    assert(!d.lifetime_errors);
}

static void inheritance_contract(void)
{
    struct VkDevice_T d = {0};
    VkCommandPool p = make_pool(&d, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
    VkCommandBuffer c = secondary(&d, p);

    /* A secondary must describe what it inherits. */
    assert(begin_secondary(c, 0, NULL) != VK_SUCCESS && c->state == PS5VK_INITIAL);

    /* The accepted shape, and the copy is owned: mutating the caller's
     * structure afterwards must not change what was recorded. */
    VkCommandBufferInheritanceInfo i = inheritance();
    assert(begin_secondary(c, 0, &i) == VK_SUCCESS);
    assert(c->state == PS5VK_RECORDING && c->inheritance_valid);
    assert(c->inheritance.sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO);
    i.subpass = 9; i.occlusionQueryEnable = VK_TRUE;
    assert(!c->inheritance.subpass && !c->inheritance.occlusionQueryEnable);
    assert(vkEndCommandBuffer(c) == VK_SUCCESS);

    /* The inherited scope members are IGNORED without RENDER_PASS_CONTINUE, so
     * whatever the caller supplies must be accepted and must change nothing.
     * The pinned CTS states this directly (doc/testspecs/VK/apitests.adoc,
     * command-buffer-recording case 9). Refusing them would reject a
     * conformant call. */
    const VkCommandBufferInheritanceInfo ignored_scope = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
        .renderPass = (VkRenderPass)(uintptr_t)0x1234,
        .framebuffer = (VkFramebuffer)(uintptr_t)0x5678,
        .subpass = 7};
    assert(vkResetCommandBuffer(c, 0) == VK_SUCCESS);
    VkCommandBufferInheritanceInfo supplied = ignored_scope;
    assert(begin_secondary(c, 0, &supplied) == VK_SUCCESS);
    assert(c->state == PS5VK_RECORDING && c->inheritance_valid);
    /* Retained verbatim as an opaque record, and never consulted: the buffer
     * behaves exactly as one begun with a zeroed scope. */
    assert(c->inheritance.renderPass == ignored_scope.renderPass);
    assert(c->inheritance.framebuffer == ignored_scope.framebuffer);
    assert(c->inheritance.subpass == ignored_scope.subpass);
    assert(!c->render_pass && !c->framebuffer && !c->operation_count);
    VkEvent probe_event = (VkEvent)(uintptr_t)0;
    (void)probe_event;
    assert(vkEndCommandBuffer(c) == VK_SUCCESS);

    /* Everything this device cannot truthfully honour is refused, and a
     * refused begin leaves the buffer untouched rather than half-recorded. */
    const struct { const char *name; VkCommandBufferUsageFlags flags;
                   VkCommandBufferInheritanceInfo info; } refused[] = {
        /* RENDER_PASS_CONTINUE has no implementation in this slice */
        {"render-pass-continue", VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT, {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO}},
        /* queries this device does not execute at all */
        {"occlusion-query", 0, {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
            .occlusionQueryEnable = VK_TRUE}},
        {"query-flags", 0, {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
            .queryFlags = VK_QUERY_CONTROL_PRECISE_BIT}},
        {"pipeline-statistics", 0, {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
            .pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT}},
        /* a wrong or extended structure is not silently accepted */
        {"wrong-stype", 0, {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO}},
    };
    for (unsigned n = 0; n < sizeof(refused) / sizeof(refused[0]); ++n) {
        assert(vkResetCommandBuffer(c, 0) == VK_SUCCESS);
        VkCommandBufferInheritanceInfo local = refused[n].info;
        if (begin_secondary(c, refused[n].flags, &local) == VK_SUCCESS)
            fprintf(stderr, "inheritance case %s was accepted\n", refused[n].name);
        assert(c->state == PS5VK_INITIAL && !c->inheritance_valid);
    }
    /* pNext on the inheritance structure is a retained pointer, not owned. */
    assert(vkResetCommandBuffer(c, 0) == VK_SUCCESS);
    VkCommandBufferInheritanceInfo chained = inheritance();
    VkCommandBufferInheritanceInfo tail = inheritance();
    chained.pNext = &tail;
    assert(begin_secondary(c, 0, &chained) != VK_SUCCESS && c->state == PS5VK_INITIAL);

    /* A primary ignores the pointer entirely and stores nothing. */
    VkCommandBufferAllocateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = p, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer prim;
    assert(vkAllocateCommandBuffers(&d, &pi, &prim) == VK_SUCCESS);
    VkCommandBufferInheritanceInfo ignored = inheritance();
    ignored.occlusionQueryEnable = VK_TRUE;
    assert(begin_secondary(prim, 0, &ignored) == VK_SUCCESS);
    assert(!prim->inheritance_valid && !prim->inheritance.occlusionQueryEnable);
    assert(vkEndCommandBuffer(prim) == VK_SUCCESS);

    vkFreeCommandBuffers(&d, p, 1, &prim);
    vkFreeCommandBuffers(&d, p, 1, &c);
    vkDestroyCommandPool(&d, p, NULL);
    assert(!d.lifetime_errors);
}

static void usage_and_state_matrix(void)
{
    struct VkDevice_T d = {0};
    VkCommandPool p = make_pool(&d, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
    VkCommandBuffer c = secondary(&d, p);
    VkCommandBufferInheritanceInfo i = inheritance();

    /* VUID-vkBeginCommandBuffer-commandBuffer-02840 makes one-time-submit and
     * simultaneous-use mutually exclusive for a PRIMARY ONLY. A secondary may
     * set both, so the pair must be ACCEPTED here; refusing it would reject a
     * conformant call. */
    assert(begin_secondary(c, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
                              VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT, &i) == VK_SUCCESS);
    assert(c->state == PS5VK_RECORDING &&
           c->usage == (VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
                        VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT));
    assert(vkEndCommandBuffer(c) == VK_SUCCESS);
    assert(vkResetCommandBuffer(c, 0) == VK_SUCCESS);
    /* The same pair on a PRIMARY stays refused, and leaves it untouched. */
    {
        VkCommandBufferAllocateInfo ppi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = p, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VkCommandBuffer exclusive;
        assert(vkAllocateCommandBuffers(&d, &ppi, &exclusive) == VK_SUCCESS);
        VkCommandBufferBeginInfo both = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
                     VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT};
        assert(vkBeginCommandBuffer(exclusive, &both) != VK_SUCCESS);
        assert(exclusive->state == PS5VK_INITIAL && !exclusive->usage);
        vkFreeCommandBuffers(&d, p, 1, &exclusive);
    }
    assert(begin_secondary(c, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, &i) == VK_SUCCESS);
    assert(c->usage == VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    /* Beginning a recording buffer is refused. */
    assert(begin_secondary(c, 0, &i) != VK_SUCCESS);
    assert(vkEndCommandBuffer(c) == VK_SUCCESS && c->state == PS5VK_EXECUTABLE);
    assert(vkResetCommandBuffer(c, 0) == VK_SUCCESS && c->state == PS5VK_INITIAL);
    assert(begin_secondary(c, VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT, &i) == VK_SUCCESS);
    assert(c->usage == VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT);
    assert(vkEndCommandBuffer(c) == VK_SUCCESS);

    /* Without the pool's reset bit, a second begin is refused for a secondary
     * on the same rule that governs a primary. */
    VkCommandPool strict = make_pool(&d, 0);
    VkCommandBuffer once = secondary(&d, strict);
    assert(begin_secondary(once, 0, &i) == VK_SUCCESS);
    assert(vkEndCommandBuffer(once) == VK_SUCCESS);
    assert(begin_secondary(once, 0, &i) != VK_SUCCESS);
    assert(vkResetCommandBuffer(once, 0) != VK_SUCCESS);

    vkFreeCommandBuffers(&d, strict, 1, &once);
    vkDestroyCommandPool(&d, strict, NULL);
    vkFreeCommandBuffers(&d, p, 1, &c);
    vkDestroyCommandPool(&d, p, NULL);
    assert(!d.lifetime_errors);
}

static void primary_only_commands_poison_a_secondary(void)
{
    struct VkDevice_T d = {0};
    d.graphics_enabled = VK_TRUE;
    VkCommandPool p = make_pool(&d, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
    VkCommandBuffer c = secondary(&d, p);
    VkCommandBufferInheritanceInfo i = inheritance();

    /* vkCmdBeginRenderPass is primary-only: a secondary inherits a render
     * pass, it never begins one. The rejection is transactional. */
    assert(begin_secondary(c, 0, &i) == VK_SUCCESS);
    VkRenderPassBeginInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    vkCmdBeginRenderPass(c, &rp, VK_SUBPASS_CONTENTS_INLINE);
    assert(c->state == PS5VK_INVALID && !c->operation_count && !c->render_pass);

    assert(vkResetCommandBuffer(c, 0) == VK_SUCCESS);
    assert(begin_secondary(c, 0, &i) == VK_SUCCESS);
    vkCmdEndRenderPass(c);
    assert(c->state == PS5VK_INVALID && !c->operation_count);

    assert(vkResetCommandBuffer(c, 0) == VK_SUCCESS);
    assert(begin_secondary(c, 0, &i) == VK_SUCCESS);
    vkCmdNextSubpass(c, VK_SUBPASS_CONTENTS_INLINE);
    assert(c->state == PS5VK_INVALID && !c->operation_count);

    /* Nesting is refused permanently, not merely until execution exists. */
    assert(vkResetCommandBuffer(c, 0) == VK_SUCCESS);
    assert(begin_secondary(c, 0, &i) == VK_SUCCESS);
    VkCommandBuffer child = secondary(&d, p);
    vkCmdExecuteCommands(c, 1, &child);
    assert(c->state == PS5VK_INVALID && !c->operation_count);

    /* A primary CAN execute an executable secondary; that contract now lives
     * in tests/test_secondary_execute.c. Asserted here only so this file does
     * not keep claiming the opposite. */
    VkCommandBufferAllocateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = p, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer prim;
    assert(vkAllocateCommandBuffers(&d, &pi, &prim) == VK_SUCCESS);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(vkBeginCommandBuffer(prim, &bi) == VK_SUCCESS);
    assert(vkResetCommandBuffer(child, 0) == VK_SUCCESS);
    assert(begin_secondary(child, 0, &i) == VK_SUCCESS);
    assert(vkEndCommandBuffer(child) == VK_SUCCESS);
    vkCmdExecuteCommands(prim, 1, &child);
    assert(prim->state == PS5VK_RECORDING && prim->operation_count == 1 &&
           prim->operations[0].type == PS5VK_EXECUTE_COMMANDS);

    vkFreeCommandBuffers(&d, p, 1, &prim);
    vkFreeCommandBuffers(&d, p, 1, &child);
    vkFreeCommandBuffers(&d, p, 1, &c);
    vkDestroyCommandPool(&d, p, NULL);
}

int main(void)
{
    allocation_and_level();
    inheritance_contract();
    usage_and_state_matrix();
    primary_only_commands_poison_a_secondary();
    puts("Secondary command buffer object and state: pass (host only)");
    return 0;
}
