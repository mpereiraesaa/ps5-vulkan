/* DXVK262-T11: the Vulkan 1.0 routes to shaderDemoteToHelperInvocation
 * (VK_EXT_shader_demote_to_helper_invocation) and shaderTerminateInvocation
 * (VK_KHR_shader_terminate_invocation). Without the platform bits neither
 * name is enumerated, neither feature is reported and device creation refuses
 * both; with them, each route needs the instance's properties2 extension,
 * its own extension name for its own feature structure, and records exactly
 * the feature the application enabled. */
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
        .name="T11 demote host mock",.heap_size=65536,
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

static void query(VkPhysicalDevice p,VkBool32 *demote,VkBool32 *terminate)
{
    VkPhysicalDeviceShaderTerminateInvocationFeatures t={
        .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES,
        .shaderTerminateInvocation=VK_TRUE};
    VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures d={
        .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES,
        .pNext=&t,.shaderDemoteToHelperInvocation=VK_TRUE};
    VkPhysicalDeviceFeatures2 f={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&d};
    vkGetPhysicalDeviceFeatures2KHR(p,&f);
    *demote=d.shaderDemoteToHelperInvocation;*terminate=t.shaderTerminateInvocation;
}

int main(void)
{
    static const char *const demote_name=VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME;
    static const char *const terminate_name=VK_KHR_SHADER_TERMINATE_INVOCATION_EXTENSION_NAME;
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
        assert(properties.apiVersion == PS5VK_DEVICE_API_VERSION);

        float priority=1.0f;
        VkDeviceQueueCreateInfo queue={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueCount=1,.pQueuePriorities=&priority};
        const char *names[2]={demote_name,terminate_name};
        VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures demote={
            .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES,
            .shaderDemoteToHelperInvocation=VK_TRUE};
        VkPhysicalDeviceShaderTerminateInvocationFeatures terminate={
            .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES,
            .shaderTerminateInvocation=VK_TRUE};
        VkDeviceCreateInfo info={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount=1,.pQueueCreateInfos=&queue};
        VkDevice d=VK_NULL_HANDLE;
        VkBool32 has_demote,has_terminate;

        /* The shipping profile: nothing listed, reported or accepted. */
        assert(!listed(p,demote_name) && !listed(p,terminate_name));
        query(p,&has_demote,&has_terminate);
        assert(!has_demote && !has_terminate);
        for(unsigned n=0;n<2;++n) {
            info.enabledExtensionCount=1;info.ppEnabledExtensionNames=&names[n];
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_EXTENSION_NOT_PRESENT && !d);
        }
        info.enabledExtensionCount=0;info.ppEnabledExtensionNames=NULL;
        info.pNext=&demote;
        assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_FEATURE_NOT_PRESENT && !d);
        info.pNext=&terminate;
        assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_FEATURE_NOT_PRESENT && !d);
        /* A false request is no request. */
        demote.shaderDemoteToHelperInvocation=VK_FALSE;info.pNext=&demote;
        assert(vkCreateDevice(p,&info,NULL,&d)==VK_SUCCESS && d);
        assert(!d->enabled_features_t09);
        vkDestroyDevice(d,NULL);d=VK_NULL_HANDLE;
        demote.shaderDemoteToHelperInvocation=VK_TRUE;

        /* A measured platform: both routes listed and reported. */
        p->platform.supported_features_t09=
            PS5VK_T09_FEATURE_SHADER_DEMOTE_TO_HELPER_INVOCATION |
            PS5VK_T09_FEATURE_SHADER_TERMINATE_INVOCATION;
        assert(listed(p,demote_name)==1 && listed(p,terminate_name)==1);
        query(p,&has_demote,&has_terminate);
        assert(has_demote && has_terminate);
        /* The registry dependency: properties2 on a Vulkan 1.0 instance. */
        info.pNext=NULL;
        for(unsigned n=0;n<2;++n) {
            info.enabledExtensionCount=1;info.ppEnabledExtensionNames=&names[n];
            assert(vkCreateDevice(p,&info,NULL,&d)==
                   (properties2?VK_SUCCESS:VK_ERROR_EXTENSION_NOT_PRESENT));
            if(d) {
                /* The name alone enables no feature. */
                assert(!d->enabled_features_t09);
                vkDestroyDevice(d,NULL);d=VK_NULL_HANDLE;
            }
        }
        if(properties2) {
            /* Each feature needs its own extension, not the other one. */
            info.enabledExtensionCount=1;info.ppEnabledExtensionNames=&names[1];
            info.pNext=&demote;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_FEATURE_NOT_PRESENT && !d);
            info.ppEnabledExtensionNames=&names[0];info.pNext=&terminate;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_FEATURE_NOT_PRESENT && !d);
            /* Each alone, then both. */
            info.pNext=&demote;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_SUCCESS && d);
            assert(d->enabled_features_t09==PS5VK_T09_FEATURE_SHADER_DEMOTE_TO_HELPER_INVOCATION);
            vkDestroyDevice(d,NULL);d=VK_NULL_HANDLE;
            info.ppEnabledExtensionNames=&names[1];info.pNext=&terminate;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_SUCCESS && d);
            assert(d->enabled_features_t09==PS5VK_T09_FEATURE_SHADER_TERMINATE_INVOCATION);
            vkDestroyDevice(d,NULL);d=VK_NULL_HANDLE;
            demote.pNext=&terminate;info.pNext=&demote;
            info.enabledExtensionCount=2;info.ppEnabledExtensionNames=names;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_SUCCESS && d);
            assert(d->enabled_features_t09==(PS5VK_T09_FEATURE_SHADER_DEMOTE_TO_HELPER_INVOCATION|
                                             PS5VK_T09_FEATURE_SHADER_TERMINATE_INVOCATION));
            vkDestroyDevice(d,NULL);d=VK_NULL_HANDLE;
            /* A duplicated structure or name, and a non-boolean value, are
             * invalid rather than silently merged. */
            VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures twice=demote;
            twice.pNext=NULL;demote.pNext=&twice;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_UNKNOWN && !d);
            demote.pNext=NULL;demote.shaderDemoteToHelperInvocation=2;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_UNKNOWN && !d);
            demote.shaderDemoteToHelperInvocation=VK_TRUE;
            const char *repeated[2]={demote_name,demote_name};
            info.ppEnabledExtensionNames=repeated;info.pNext=NULL;
            assert(vkCreateDevice(p,&info,NULL,&d)==VK_ERROR_UNKNOWN && !d);
        }
        vkDestroyInstance(instance,NULL);
    }
    return 0;
}
