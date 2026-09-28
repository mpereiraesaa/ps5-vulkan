/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real graphics compiler/driver with synthetic rendering, not GPU evidence. */
#include "vk_internal.h"
#include "vk_pipeline.h"
#include "graphics_formats.h"
#include "graphics_limits.h"
#include "vk_queue.h"
#include "physical_device_profile.h"
#include "runtime_graphics_compiler.h"
#include "readback_commands_ps5.h"
#include "upload_commands_ps5.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void require_recording(VkCommandBuffer command,const char *call)
{
 if(command->state!=PS5VK_RECORDING)fprintf(stderr,"recording failed at %s operations=%u\n",call,command->operation_count);
 assert(command->state==PS5VK_RECORDING);
}
#define vkCmdPipelineBarrier(cb,...) do { vkCmdPipelineBarrier(cb,__VA_ARGS__);require_recording(cb,"vkCmdPipelineBarrier"); } while(0)
#define vkCmdBeginRenderPass(cb,...) do { vkCmdBeginRenderPass(cb,__VA_ARGS__);require_recording(cb,"vkCmdBeginRenderPass"); } while(0)
#define vkCmdBindPipeline(cb,...) do { vkCmdBindPipeline(cb,__VA_ARGS__);require_recording(cb,"vkCmdBindPipeline"); } while(0)
#define vkCmdBindDescriptorSets(cb,...) do { vkCmdBindDescriptorSets(cb,__VA_ARGS__);require_recording(cb,"vkCmdBindDescriptorSets"); } while(0)
#define vkCmdDraw(cb,...) do { vkCmdDraw(cb,__VA_ARGS__);require_recording(cb,"vkCmdDraw"); } while(0)
#define vkCmdCopyImageToBuffer(cb,...) do { vkCmdCopyImageToBuffer(cb,__VA_ARGS__);require_recording(cb,"vkCmdCopyImageToBuffer"); } while(0)
#define vkCmdEndRenderPass(cb) do { vkCmdEndRenderPass(cb);require_recording(cb,"vkCmdEndRenderPass"); } while(0)
#include "../examples/dxvk_render_witness/integer_dot_graphics.h"
#undef vkCmdPipelineBarrier
#undef vkCmdBeginRenderPass
#undef vkCmdBindPipeline
#undef vkCmdBindDescriptorSets
#undef vkCmdDraw
#undef vkCmdCopyImageToBuffer
#undef vkCmdEndRenderPass

static VkResult allocate(void *ctx,VkDeviceSize size,void **address,void **backing)
{ (void)ctx;*address=malloc(size);*backing=*address;return *address?VK_SUCCESS:VK_ERROR_OUT_OF_HOST_MEMORY; }
static void deallocate(void *ctx,void *backing) { (void)ctx;free(backing); }
static VkResult cache(void *ctx,void *backing,VkDeviceSize offset,VkDeviceSize size)
{ (void)ctx;(void)backing;(void)offset;(void)size;return VK_SUCCESS; }
static VkResult open_memory(void *ctx,struct ps5vk_memory_backend *backend)
{ (void)ctx;*backend=(struct ps5vk_memory_backend){NULL,allocate,deallocate,cache,cache};return VK_SUCCESS; }
static void close_memory(struct ps5vk_memory_backend *backend) { (void)backend; }
VkResult ps5vk_platform_query(struct ps5vk_platform *p)
{
 *p=(struct ps5vk_platform){.open=open_memory,.close=close_memory,.max_allocation=1u<<22,
  .queue_flags=VK_QUEUE_COMPUTE_BIT|VK_QUEUE_GRAPHICS_BIT,
  .format_properties=ps5vk_graphics_format_properties,.image_properties=ps5vk_graphics_image_properties,
  .supported_features=PS5VK_FEATURE_GEOMETRY_SHADER|PS5VK_FEATURE_TESSELLATION_SHADER,.supported_features_v13=PS5VK_V13_FEATURE_SHADER_INTEGER_DOT_PRODUCT};
 const struct ps5vk_physical_profile_info profile={.name="synthetic integer-dot queue",.heap_size=1u<<22,
  .allocation_granularity=1,.buffer_image_granularity=1};
 ps5vk_physical_profile_init(&p->properties,&p->memory_properties,&profile);
 ps5vk_graphics_limits(&p->properties.limits);return VK_SUCCESS;
}
static VkResult graphics_create(VkDevice d,const void *data,uint32_t primitive,void **out)
{
 (void)d;const struct ps5vk_runtime_graphics_program *p=data;
 assert(p && p->fragment.machine_code_size && (p->vertex.machine_code_size || p->hull.machine_code_size));
 assert(primitive==p->primitive_type);
 unsigned *mask=malloc(sizeof(*mask));assert(mask);
 *mask=!!(p->vertex.metadata.descriptor_set_valid[0] || p->hull.metadata.descriptor_set_valid[0] ||
          p->domain.metadata.descriptor_set_valid[0] || p->fragment.metadata.descriptor_set_valid[0]);
 *out=mask;return VK_SUCCESS;
}
static void graphics_release(VkDevice d,void *data) { (void)d;free(data); }
static VkResult used_sets(VkDevice d,const void *state,uint32_t *mask)
{ (void)d;*mask=*(const unsigned*)state;return VK_SUCCESS; }
static unsigned mode,components,saturating,packed,fault,launches;
static uint64_t elapsed_ns;
struct job { uint64_t serial;unsigned char *input,*output; };
static void flush_prelude(const void *address,size_t size) { (void)address;(void)size;assert(0); }
static VkResult prepare(VkDevice d,const struct ps5vk_submission *s,void **out)
{
 assert(s->count==1);VkCommandBuffer c=s->buffers[0];
 const enum ps5vk_operation_type types[]={PS5VK_IMAGE_BARRIER,PS5VK_BEGIN_RENDER_PASS,
  PS5VK_DRAW,PS5VK_END_RENDER_PASS,PS5VK_IMAGE_BARRIER,PS5VK_COPY_IMAGE_BUFFER,PS5VK_BARRIER};
 assert(c->operation_count==7);
 for(unsigned i=0;i<7;++i)assert(c->operations[i].type==types[i]);
 const struct ps5vk_operation *ops=c->operations,*draw=&ops[2],*copy=&ops[5];
 assert(draw->vertex_count==6 && draw->instance_count==128 && !draw->first_vertex && !draw->first_instance);
 assert(draw->viewport.width==16 && draw->viewport.height==8 && draw->scissor.extent.width==16 && draw->scissor.extent.height==8);
 assert(draw->raster.cull_mode==VK_CULL_MODE_NONE && !draw->raster.depth_test && !draw->pipeline->rasterizer_discard);
 VkDescriptorSet set=draw->sets[0];assert(set && set->buffers[0].offset==256 && set->buffers[0].range==32768);
 struct job *job=calloc(1,sizeof(*job));assert(job);job->serial=s->serial;
 void *address;VkDeviceSize size;
 assert(ps5vk_buffer_span(d,set->buffers[0].buffer,0,VK_WHOLE_SIZE,&address,&size)==VK_SUCCESS && size==33280);
 job->input=address;
 assert(copy->copy_region.bufferOffset==256 && copy->copy_region.imageExtent.width==16 && copy->copy_region.imageExtent.height==8);
 assert(ps5vk_buffer_span(d,copy->copy_destination,0,VK_WHOLE_SIZE,&address,&size)==VK_SUCCESS && size==1024);job->output=address;
 struct ps5vk_layout_state layouts={0};
 uint32_t packets[256]={0},*cursor=packets;
 assert(ps5vk_upload_commands(d,ops,1,copy->copy_image,&layouts,&cursor,packets+256,flush_prelude)==VK_SUCCESS);
 assert(cursor>packets && cursor<=packets+256);
 VkImageLayout current;
 assert(ps5vk_layout_current(&layouts,copy->copy_image,VK_IMAGE_ASPECT_COLOR_BIT,&current)==VK_SUCCESS && current==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
 struct ps5vk_readback_regions regions;unsigned site=0;
 VkResult r=ps5vk_readback_regions_commands(d,&ops[4],3,&layouts,&regions,&site);
 if(r!=VK_SUCCESS)fprintf(stderr,"readback planner result=%d site=%u\n",r,site);
 assert(r==VK_SUCCESS && regions.count==1 && regions.target[0].region.bufferOffset==256);
 *out=job;return VK_SUCCESS;
}
static uint32_t word(const unsigned char *p) { uint32_t v;memcpy(&v,p,4);return v; }
static int64_t decode(uint32_t v,unsigned bits,unsigned sign)
{ return sign && (v&(1u<<(bits-1)))?(int64_t)v-((int64_t)1<<bits):v; }
static VkResult launch(VkDevice d,void *data)
{
 (void)d;struct job *job=data;++launches;
 for(unsigned i=0;i<128;++i) {
  const unsigned char *row=job->input+256+i*256;__int128 value=0;
  for(unsigned lane=0;lane<components;++lane) {
   uint32_t x=packed?row[lane]:word(row+lane*4),y=packed?row[16+lane]:word(row+16+lane*4);
   value+=(__int128)decode(x,packed?8:32,mode!=0)*decode(y,packed?8:32,mode==1);
  }
  if(saturating) {
   value+=decode(word(row+32),32,mode!=0);
   __int128 lo=mode?-(INT64_C(1)<<31):0,hi=mode?INT32_MAX:UINT32_MAX;
   if(value<lo)value=lo;
   if(value>hi)value=hi;
  }
  uint32_t v=(uint32_t)value;if(fault==1)v^=1;
  unsigned char rgba[4]={v,v>>8,v>>16,v>>24};
  if(fault!=4)memcpy(job->output+256+4*i,rgba,4);
 }
 if(fault==2)job->output[255]^=1;
 if(fault==3)job->input[256]^=1;
 return VK_SUCCESS;
}
static VkResult poll(VkDevice d,void *data,uint64_t *serial)
{ (void)d;*serial=fault==5?0:((struct job*)data)->serial;return VK_SUCCESS; }
static void release(VkDevice d,void *data) { (void)d;free(data); }
static uint64_t clock_now(void *ctx) { (void)ctx;return elapsed_ns; }
static void pause_poll(void *ctx,uint64_t timeout)
{ (void)ctx;assert(fault==5 && timeout<=UINT64_C(300000000));elapsed_ns+=timeout; }
static uint32_t read32(void) { uint32_t v;assert(fread(&v,4,1,stdin)==1);return v; }
int main(int argc,char **argv)
{
 unsigned timeout_test=argc==2 && !strcmp(argv[1],"--timeout");
 VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
 VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
 VkInstance instance;assert(vkCreateInstance(&ici,NULL,&instance)==VK_SUCCESS);
 uint32_t count=1;VkPhysicalDevice physical;assert(vkEnumeratePhysicalDevices(instance,&count,&physical)==VK_SUCCESS);
 float priority=1;VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueCount=1,.pQueuePriorities=&priority};
 VkPhysicalDeviceShaderIntegerDotProductFeatures dot={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES,.shaderIntegerDotProduct=VK_TRUE};
 VkPhysicalDeviceFeatures features={.geometryShader=VK_TRUE,.tessellationShader=VK_TRUE};
 const char *extension=VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME;
 VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&dot,.pEnabledFeatures=&features,
  .queueCreateInfoCount=1,.pQueueCreateInfos=&qci,.enabledExtensionCount=1,.ppEnabledExtensionNames=&extension};
 VkDevice d;assert(vkCreateDevice(physical,&dci,NULL,&d)==VK_SUCCESS);
 d->graphics_enabled=VK_TRUE;d->graphics_submit_enabled=VK_TRUE;d->image_requirements=ps5vk_native_image_requirements;
 d->graphics_acquire=ps5vk_runtime_graphics_compile;d->graphics_compiled_release=ps5vk_runtime_graphics_free;
 d->graphics_create=graphics_create;d->graphics_release=graphics_release;d->graphics_used_sets=used_sets;
 d->submit_backend=(struct ps5vk_queue_backend){prepare,launch,poll,release};
 d->progress=(struct ps5vk_progress){NULL,ps5vk_queue_poll,clock_now,pause_poll};
 VkQueue queue;vkGetDeviceQueue(d,0,0,&queue);
 unsigned runs=read32(),executions=0;
 for(unsigned c=0;c<runs;++c) {
  mode=read32();components=read32();saturating=read32();packed=read32();
  struct integer_dot_graphics_case data={0};
  for(unsigned stage=0;stage<5;++stage) {
   unsigned bytes=read32();data.bytes[stage]=bytes;
   if(bytes) {uint32_t *words=malloc(bytes);assert(words && fread(words,1,bytes,stdin)==bytes);data.words[stage]=words;}
  }
  unsigned char records[32768],image[512];assert(fread(records,1,32768,stdin)==32768 && fread(image,1,512,stdin)==512);
  data.records=records;data.expected_rgba=image;
  for(fault=timeout_test?5:0;fault<(timeout_test?6:c<6?5:1);++fault) {
   launches=0;VkBool32 pending=VK_TRUE;struct integer_dot_graphics_result observed;
   VkResult r=integer_dot_graphics_witness(d,queue,&data,&pending,&observed);
   if(timeout_test && r==VK_TIMEOUT) {
    assert(pending && launches==1 && elapsed_ns==UINT64_C(300000000) && !observed.pixels);
    assert(d->submission && d->graphics_objects && d->buffers && d->memories && d->command_pools && !d->lifetime_errors);
    puts("bounded graphics timeout retained resources");fflush(stdout);_Exit(0);
   }
   if(r!=(fault?VK_ERROR_UNKNOWN:VK_SUCCESS))fprintf(stderr,"case=%u fault=%u result=%d step=%s\n",c,fault,r,observed.step);
   assert(r==(fault?VK_ERROR_UNKNOWN:VK_SUCCESS) && !pending && launches==1);
   assert(observed.pixels==128 && observed.guards==(fault==2) && observed.input_changes==(fault==3));
   assert(observed.mismatches==((fault==1 || fault==4)?128u:0u));
   assert(!d->graphics_objects && !d->pipeline_objects && !d->descriptor_objects && !d->buffers && !d->memories && !d->command_pools && !d->fences && !d->lifetime_errors);
   ++executions;
  }
  for(unsigned stage=0;stage<5;++stage)free((void*)data.words[stage]);
 }
 vkDestroyDevice(d,NULL);vkDestroyInstance(instance,NULL);
 printf("%u graphics executions with real compiler and synthetic pixels; no GPU evidence\n",executions);
 return 0;
}
