#include "vk_internal.h"
#include "physical_device_profile.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static VkResult alloc_memory(void *ctx,VkDeviceSize size,void **address,void **backing)
{
    (void)ctx;
    *address=malloc((size_t)size);
    *backing=*address;
    return *address?VK_SUCCESS:VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void free_memory(void *ctx,void *backing)
{ (void)ctx;free(backing); }
static VkResult cache(void *ctx,void *backing,VkDeviceSize offset,VkDeviceSize size)
{ (void)ctx;(void)backing;(void)offset;(void)size;return VK_SUCCESS; }
static VkResult open_backend(void *ctx,struct ps5vk_memory_backend *backend)
{
    (void)ctx;
    *backend=(struct ps5vk_memory_backend){NULL,alloc_memory,free_memory,cache,cache};
    return VK_SUCCESS;
}
static void close_backend(struct ps5vk_memory_backend *backend)
{ (void)backend; }
VkResult ps5vk_platform_query(struct ps5vk_platform *p)
{
    *p=(struct ps5vk_platform){.open=open_backend,.close=close_backend,
        .max_allocation=65536,.queue_flags=VK_QUEUE_COMPUTE_BIT,
        .supported_features=PS5VK_FEATURE_ROBUST_BUFFER_ACCESS};
    const struct ps5vk_physical_profile_info profile={
        .name="T09 sampler host mock",.heap_size=65536,
        .allocation_granularity=1,.buffer_image_granularity=1};
    ps5vk_physical_profile_init(&p->properties,&p->memory_properties,&profile);
    return VK_SUCCESS;
}

static VkBool32 lists_mirror(VkPhysicalDevice p)
{
    uint32_t count=0;
    assert(vkEnumerateDeviceExtensionProperties(p,NULL,&count,NULL)==VK_SUCCESS);
    assert(count<32);
    VkExtensionProperties extensions[32];
    memset(extensions,0,sizeof(extensions));
    uint32_t capacity=32;
    assert(vkEnumerateDeviceExtensionProperties(p,NULL,&capacity,extensions)==VK_SUCCESS);
    assert(capacity==count);
    for(uint32_t i=0;i<count;++i)
        if(!strcmp(extensions[i].extensionName,
                   VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME))
            return VK_TRUE;
    return VK_FALSE;
}

int main(void)
{
    VkInstanceCreateInfo instance_info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkInstance instance=VK_NULL_HANDLE;
    assert(vkCreateInstance(&instance_info,NULL,&instance)==VK_SUCCESS);
    uint32_t physical_count=1;
    VkPhysicalDevice p=VK_NULL_HANDLE;
    assert(vkEnumeratePhysicalDevices(instance,&physical_count,&p)==VK_SUCCESS);
    assert(physical_count==1 && p);
    VkPhysicalDeviceProperties properties={0};
    vkGetPhysicalDeviceProperties(p,&properties);
    assert(properties.apiVersion == PS5VK_DEVICE_API_VERSION);

    float priority=1.0f;
    VkDeviceQueueCreateInfo queue={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount=1,.pQueuePriorities=&priority};
    const char *mirror=VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME;
    VkDeviceCreateInfo info={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount=1,.pQueueCreateInfos=&queue,
        .enabledExtensionCount=1,.ppEnabledExtensionNames=&mirror};
    VkDevice d=VK_NULL_HANDLE;
    assert(!lists_mirror(p));
    assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_EXTENSION_NOT_PRESENT && !d);

    p->platform.supported_features_t09=
        PS5VK_T09_FEATURE_SAMPLER_MIRROR_CLAMP_TO_EDGE;
    assert(lists_mirror(p));
    info.enabledExtensionCount=0;
    info.ppEnabledExtensionNames=NULL;
    assert(vkCreateDevice(p,&info,NULL,&d)==VK_SUCCESS && d);
    assert(!(d->enabled_features_t09 &
             PS5VK_T09_FEATURE_SAMPLER_MIRROR_CLAMP_TO_EDGE));
    vkDestroyDevice(d,NULL);
    info.enabledExtensionCount=1;
    info.ppEnabledExtensionNames=&mirror;
    assert(vkCreateDevice(p,&info,NULL,&d)==VK_SUCCESS && d);
    assert(d->enabled_features_t09 &
           PS5VK_T09_FEATURE_SAMPLER_MIRROR_CLAMP_TO_EDGE);
    vkDestroyDevice(d,NULL);
    vkDestroyInstance(instance,NULL);
}
