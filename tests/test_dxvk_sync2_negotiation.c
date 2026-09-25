/* DXVK262: the Vulkan 1.0 route to VK_KHR_synchronization2. Without the
 * platform bit the name is not enumerated, the feature is not reported and
 * device creation refuses both; with it, the extension needs the instance's
 * properties2 extension, the feature needs the extension, and the six KHR
 * commands are reachable only on a device that enabled the extension. */
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
        .name="sync2 host mock",.heap_size=65536,
        .allocation_granularity=1,.buffer_image_granularity=1};
    ps5vk_physical_profile_init(&p->properties,&p->memory_properties,&profile);
    return VK_SUCCESS;
}

static unsigned listed(VkPhysicalDevice p,const char *name)
{
    uint32_t count=0;
    assert(vkEnumerateDeviceExtensionProperties(p,NULL,&count,NULL)==VK_SUCCESS);
    VkExtensionProperties extensions[32];
    assert(count<=32);
    uint32_t capacity=count;
    assert(vkEnumerateDeviceExtensionProperties(p,NULL,&capacity,extensions)==VK_SUCCESS);
    unsigned found=0;
    for(uint32_t i=0;i<capacity;++i)found+=!strcmp(extensions[i].extensionName,name);
    return found;
}

static VkBool32 reported(VkPhysicalDevice p)
{
    VkPhysicalDeviceSynchronization2Features s={
        .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
        .synchronization2=VK_TRUE};
    VkPhysicalDeviceFeatures2 f={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&s};
    vkGetPhysicalDeviceFeatures2KHR(p,&f);
    return s.synchronization2;
}

static const char *const commands[]={"vkCmdPipelineBarrier2KHR","vkCmdSetEvent2KHR",
    "vkCmdResetEvent2KHR","vkCmdWaitEvents2KHR","vkCmdWriteTimestamp2KHR","vkQueueSubmit2KHR"};

static void reachable(VkDevice d,VkBool32 expected)
{
    for(unsigned n=0;n<sizeof(commands)/sizeof(commands[0]);++n)
        assert(!!vkGetDeviceProcAddr(d,commands[n])==!!expected);
    /* Vulkan 1.0 has no core names for them. */
    assert(!vkGetDeviceProcAddr(d,"vkCmdPipelineBarrier2"));
    assert(!vkGetDeviceProcAddr(d,"vkQueueSubmit2"));
}

int main(void)
{
    static const char *const name=VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME;
    for(unsigned properties2=0;properties2<2;++properties2) {
        const char *instance_extension=VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
        VkInstanceCreateInfo instance_info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .enabledExtensionCount=properties2,.ppEnabledExtensionNames=&instance_extension};
        VkInstance instance=VK_NULL_HANDLE;
        assert(vkCreateInstance(&instance_info,NULL,&instance)==VK_SUCCESS);
        uint32_t physical_count=1;
        VkPhysicalDevice p=VK_NULL_HANDLE;
        assert(vkEnumeratePhysicalDevices(instance,&physical_count,&p)==VK_SUCCESS && p);
        VkPhysicalDeviceProperties properties={0};
        vkGetPhysicalDeviceProperties(p,&properties);
        assert(VK_VERSION_MINOR(properties.apiVersion)==0);

        float priority=1.0f;
        VkDeviceQueueCreateInfo queue={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueCount=1,.pQueuePriorities=&priority};
        VkPhysicalDeviceSynchronization2Features feature={
            .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
            .synchronization2=VK_TRUE};
        VkDeviceCreateInfo info={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount=1,.pQueueCreateInfos=&queue};
        VkDevice d=VK_NULL_HANDLE;

        /* The shipping profile: not listed, not reported, not accepted. */
        assert(!listed(p,name) && !reported(p));
        info.enabledExtensionCount=1;info.ppEnabledExtensionNames=&name;
        assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_EXTENSION_NOT_PRESENT && !d);
        info.enabledExtensionCount=0;info.ppEnabledExtensionNames=NULL;info.pNext=&feature;
        assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_FEATURE_NOT_PRESENT && !d);
        feature.synchronization2=VK_FALSE;
        assert(vkCreateDevice(p,&info,NULL,&d)==VK_SUCCESS && d);
        assert(!d->enabled_features_t09);
        reachable(d,VK_FALSE);
        vkDestroyDevice(d,NULL);d=VK_NULL_HANDLE;
        feature.synchronization2=VK_TRUE;

        /* The diagnostic platform: listed and reported. */
        p->platform.supported_features_t09=PS5VK_T09_FEATURE_SYNCHRONIZATION2;
        assert(listed(p,name)==1 && reported(p));
        /* The registry dependency: properties2 on a Vulkan 1.0 instance. */
        info.pNext=NULL;info.enabledExtensionCount=1;info.ppEnabledExtensionNames=&name;
        assert(vkCreateDevice(p,&info,NULL,&d)==
               (properties2?VK_SUCCESS:VK_ERROR_EXTENSION_NOT_PRESENT));
        if(d) {
            /* The name alone exposes the commands but enables no feature. */
            assert(!d->enabled_features_t09 && d->synchronization2_extension_enabled);
            reachable(d,VK_TRUE);
            vkDestroyDevice(d,NULL);d=VK_NULL_HANDLE;
        }
        if(properties2) {
            info.pNext=&feature;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_SUCCESS && d);
            assert(d->enabled_features_t09==PS5VK_T09_FEATURE_SYNCHRONIZATION2);
            reachable(d,VK_TRUE);
            vkDestroyDevice(d,NULL);d=VK_NULL_HANDLE;
            /* The feature without the name, a duplicated structure or name,
             * and a non-boolean value are refused. */
            info.enabledExtensionCount=0;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_FEATURE_NOT_PRESENT && !d);
            info.enabledExtensionCount=1;
            VkPhysicalDeviceSynchronization2Features twice=feature;
            feature.pNext=&twice;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_UNKNOWN && !d);
            feature.pNext=NULL;feature.synchronization2=2;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_UNKNOWN && !d);
            feature.synchronization2=VK_TRUE;
            const char *repeated[2]={name,name};
            info.enabledExtensionCount=2;info.ppEnabledExtensionNames=repeated;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_UNKNOWN && !d);
        }
        vkDestroyInstance(instance,NULL);
    }
    return 0;
}
