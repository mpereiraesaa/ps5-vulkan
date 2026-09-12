#include "vk_internal.h"
#include <stdlib.h>
#include <string.h>

void *ps5vk_object_alloc(const VkAllocationCallbacks *fallback,
    const VkAllocationCallbacks *given, size_t size, VkSystemAllocationScope scope,
    VkAllocationCallbacks *saved, VkBool32 *custom)
{
    const VkAllocationCallbacks *a = given ? given : fallback;
    void *object;
    *custom = a != NULL;
    if (a) {
        if (!a->pfnAllocation || !a->pfnFree || !a->pfnReallocation) return NULL;
        *saved = *a;
        object = a->pfnAllocation(a->pUserData, size, _Alignof(max_align_t), scope);
    } else object = malloc(size);
    if (object) memset(object, 0, size);
    return object;
}
void ps5vk_object_free(void *object, const VkAllocationCallbacks *a, VkBool32 custom)
{
    if (custom) a->pfnFree(a->pUserData, object);
    else free(object);
}
