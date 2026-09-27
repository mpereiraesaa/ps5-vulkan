/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _DEFAULT_SOURCE 1
#include <ps5vk/ps5vk.h>
#include "ps5log.h"
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "compute.h"
#include "robust_image_fixture.h"

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
    VkPhysicalDeviceImageRobustnessFeatures image_feature={
        .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES};
    VkPhysicalDeviceRobustness2FeaturesEXT buffer_features={
        .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    const VkBool32 texel=robust_fixture.type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
        robust_fixture.type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    VkPhysicalDeviceFeatures2 f2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext=texel?(void*)&buffer_features:(void*)&image_feature};
    vkGetPhysicalDeviceFeatures2(physical,&f2);
    REQUIRE(texel ? buffer_features.robustBufferAccess2 : image_feature.robustImageAccess);
    buffer_features.nullDescriptor=buffer_features.robustImageAccess2=VK_FALSE;
    VkPhysicalDeviceFeatures core={.robustBufferAccess=texel};
    const char *extension=VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;
    float priority=1;
    VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext=f2.pNext,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci,
        .pEnabledFeatures=&core,.enabledExtensionCount=texel?1:0,.ppEnabledExtensionNames=texel?&extension:NULL};
    TRY(vkCreateDevice(physical,&dci,NULL,&device));
    VkQueue queue=VK_NULL_HANDLE;vkGetDeviceQueue(device,0,0,&queue);REQUIRE(queue);
    ps5log_printf(PS5LOG_MARK,"ROBUST_IMAGE_START case=%s count=%u write=%u",RI_CASE_NAME,
        robust_fixture.count,robust_fixture.write);
    struct robust_image_result observed;
    result=robust_image_compute(device,queue,robust_image_spirv,sizeof(robust_image_spirv),
        &robust_fixture,&pending,&observed);
    step=observed.step;
    if(!pending && observed.outputs)
        ps5log_printf(PS5LOG_MARK,"ROBUST_IMAGE_RESULT outputs=%u mismatches=%u image_changes=%u input_changes=%u guards=%u digest=%08x fence=complete",
            observed.outputs,observed.mismatches,observed.image_changes,observed.input_changes,observed.guards,observed.digest);
cleanup:
    if(result!=VK_SUCCESS)
        ps5log_printf(PS5LOG_ERR,"ROBUST_IMAGE_FAILURE result=%d step=%s",(int)result,step?step:"unknown");
    if(pending) {
        ps5log_printf(PS5LOG_ERR,"ROBUST_IMAGE_PENDING resources=retained");
        return 1;
    }
    if(device)vkDestroyDevice(device,NULL);
    if(instance)vkDestroyInstance(instance,NULL);
    if(result==VK_SUCCESS)ps5log_printf(PS5LOG_MARK,"ROBUST_IMAGE_RETIRED resources=clean");
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
    int failed=witness();ps5log_close(failed?"robust-image-failed":"robust-image-end");
    for(;;)sleep(1);
}
