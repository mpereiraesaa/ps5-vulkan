#define _DEFAULT_SOURCE 1
#include <ps5vk/ps5vk.h>
#include "ps5log.h"
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../dxvk_render_witness/subgroup_compute.h"
#include "subgroup_size_shader.h"

static int witness(void)
{
    VkResult result=VK_SUCCESS;
    const char *step="initialization";
    VkInstance instance=VK_NULL_HANDLE;
    VkDevice device=VK_NULL_HANDLE;
    VkBool32 pending=VK_FALSE;
#define TRY(call) do { step=#call; result=(call); if(result!=VK_SUCCESS) goto cleanup; } while(0)
#define REQUIRE(ok) do { step=#ok; if(!(ok)) { result=VK_ERROR_FEATURE_NOT_PRESENT; goto cleanup; } } while(0)
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    TRY(vkCreateInstance(&ici,NULL,&instance));
    uint32_t count=1;VkPhysicalDevice physical=VK_NULL_HANDLE;
    TRY(vkEnumeratePhysicalDevices(instance,&count,&physical));
    REQUIRE(count==1 && physical);
    VkPhysicalDeviceSubgroupSizeControlFeatures features={
        .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES};
    VkPhysicalDeviceFeatures2 f2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&features};
    vkGetPhysicalDeviceFeatures2(physical,&f2);
    REQUIRE(features.subgroupSizeControl && features.computeFullSubgroups);
    VkPhysicalDeviceSubgroupSizeControlProperties sizes={
        .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
    VkPhysicalDeviceProperties2 p2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&sizes};
    vkGetPhysicalDeviceProperties2(physical,&p2);
    REQUIRE(p2.properties.apiVersion>=VK_API_VERSION_1_3 && sizes.minSubgroupSize==32 &&
        sizes.maxSubgroupSize==32 && (sizes.requiredSubgroupSizeStages&VK_SHADER_STAGE_COMPUTE_BIT));
    float priority=1;
    VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext=&features,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci};
    TRY(vkCreateDevice(physical,&dci,NULL,&device));
    VkQueue queue=VK_NULL_HANDLE;vkGetDeviceQueue(device,0,0,&queue);REQUIRE(queue);
    ps5log_printf(PS5LOG_MARK,"SUBGROUP_SIZE_START case=%s local=%ux%ux%u flags=%u required=%u groups=2 fields=8",
        SUBGROUP_CASE_NAME,subgroup_case.dimensions[0],subgroup_case.dimensions[1],subgroup_case.dimensions[2],
        subgroup_case.flags,subgroup_case.required_size);
    struct subgroup_compute_result observed;
    result=subgroup_compute_witness(device,queue,subgroup_size_spirv,sizeof(subgroup_size_spirv),
        &subgroup_case,&pending,&observed);
    step=observed.step;
    if(!pending && observed.outputs)
        ps5log_printf(PS5LOG_MARK,"SUBGROUP_SIZE_RESULT outputs=%u mismatches=%u guards=%u digest=%08x fence=complete",
            observed.outputs,observed.mismatches,observed.guards,observed.digest);
cleanup:
    if(result!=VK_SUCCESS)
        ps5log_printf(PS5LOG_ERR,"SUBGROUP_SIZE_FAILURE result=%d step=%s",(int)result,step?step:"unknown");
    if(pending) {
        ps5log_printf(PS5LOG_ERR,"SUBGROUP_SIZE_PENDING resources=retained");
        return 1;
    }
    if(device)vkDestroyDevice(device,NULL);
    if(instance)vkDestroyInstance(instance,NULL);
    if(result==VK_SUCCESS)ps5log_printf(PS5LOG_MARK,"SUBGROUP_SIZE_RETIRED resources=clean");
    return result==VK_SUCCESS?0:1;
#undef TRY
#undef REQUIRE
}

int main(void)
{
    struct timespec now={0};clock_gettime(CLOCK_MONOTONIC,&now);
    uint64_t boot=(uint64_t)now.tv_sec*UINT64_C(1000000000)+now.tv_nsec;
    ps5log_config config;const char *loaded=NULL,*paths[]={"/app0/dev.conf"};
    ps5log_config_defaults(&config);
    if(ps5log_load_config(paths,1,&config,&loaded))_exit(1);
    config.udp=0;
    if(ps5log_init(&config,"PPSA99994","ps5vk",boot))_exit(1);
    int failed=witness();ps5log_close(failed?"subgroup-size-failed":"subgroup-size-end");
    for(;;)sleep(1);
}
