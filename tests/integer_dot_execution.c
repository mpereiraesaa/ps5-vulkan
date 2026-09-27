/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real driver/compiler with synthetic execution: never GPU evidence. */
#include "vk_internal.h"
#include "vk_pipeline.h"
#include "vk_queue.h"
#include "ps5vk_compiler.h"
#include "physical_device_profile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../examples/dxvk_render_witness/integer_dot_compute.h"

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
 *p=(struct ps5vk_platform){.open=open_memory,.close=close_memory,.max_allocation=131072,
  .queue_flags=VK_QUEUE_COMPUTE_BIT,.supported_features_v13=PS5VK_V13_FEATURE_SHADER_INTEGER_DOT_PRODUCT};
 const struct ps5vk_physical_profile_info profile={.name="synthetic integer-dot queue",.heap_size=131072,
  .allocation_granularity=1,.buffer_image_granularity=1};
 ps5vk_physical_profile_init(&p->properties,&p->memory_properties,&profile);return VK_SUCCESS;
}
static unsigned mode,components,saturating,packed,fault,launches;
static uint64_t elapsed_ns;
struct dot_job { uint64_t serial; unsigned char *mapped[4]; };
static VkResult prepare(VkDevice d,const struct ps5vk_submission *s,void **out)
{
 assert(s->count==1 && s->buffers[0]->operation_count==3);
 const struct ps5vk_operation *ops=s->buffers[0]->operations;
 assert(ops[0].type==PS5VK_BARRIER && ops[2].type==PS5VK_BARRIER);
 assert(ops[0].src_stage==VK_PIPELINE_STAGE_HOST_BIT && ops[0].dst_stage==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
 assert(ops[0].src_access==VK_ACCESS_HOST_WRITE_BIT && ops[0].dst_access==(VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT));
 assert(ops[2].src_stage==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT && ops[2].dst_stage==VK_PIPELINE_STAGE_HOST_BIT);
 assert(ops[2].src_access==VK_ACCESS_SHADER_WRITE_BIT && ops[2].dst_access==VK_ACCESS_HOST_READ_BIT);
 const struct ps5vk_operation *op=&ops[1];
 assert(op->type==PS5VK_DISPATCH && op->groups[0]==2 && op->groups[1]==1 && op->groups[2]==1);
 assert(op->pipeline->program.code_words && op->pipeline->program.wave_size==32);
 assert(op->pipeline->program.local_size[0]==64 && op->pipeline->program.local_size[1]==1 && op->pipeline->program.local_size[2]==1);
 struct dot_job *job=calloc(1,sizeof(*job));assert(job);job->serial=s->serial;
 VkDescriptorSet set=op->sets[0];assert(set);
 for(unsigned i=0;i<4;i++) {
  unsigned bytes=i>=2?512:128*(packed?4:components==2?8:16);
  assert(set->buffers[i].offset==256 && set->buffers[i].range==bytes);
  void *address;VkDeviceSize size;
  assert(ps5vk_buffer_span(d,set->buffers[i].buffer,0,VK_WHOLE_SIZE,&address,&size)==VK_SUCCESS);
  assert(size==bytes+512);job->mapped[i]=address;
 }
 *out=job;return VK_SUCCESS;
}
static uint32_t word(const unsigned char *p) { uint32_t v;memcpy(&v,p,4);return v; }
static int64_t decode(uint32_t v,unsigned bits,unsigned sign)
{ return sign && (v&(1u<<(bits-1)))?(int64_t)v-((int64_t)1<<bits):v; }
static VkResult launch(VkDevice d,void *data)
{
 (void)d;struct dot_job *job=data;launches++;
 unsigned stride=packed?4:components==2?8:16;
 for(unsigned i=0;i<128;i++) {
  __int128 value=0;
  for(unsigned lane=0;lane<components;lane++) {
   const unsigned char *a=job->mapped[0]+256+i*stride,*b=job->mapped[1]+256+i*stride;
   uint32_t x=packed?a[lane]:word(a+lane*4),y=packed?b[lane]:word(b+lane*4);
   value+=(__int128)decode(x,packed?8:32,mode!=0)*decode(y,packed?8:32,mode==1);
  }
  if(saturating) {
   value+=decode(word(job->mapped[2]+256+i*4),32,mode!=0);
   __int128 lo=mode?-(INT64_C(1)<<31):0,hi=mode?INT32_MAX:UINT32_MAX;
   if(value<lo)value=lo;
   if(value>hi)value=hi;
  }
  uint32_t output=(uint32_t)value;if(fault==1)output^=1;
  if(fault!=4)memcpy(job->mapped[3]+256+i*4,&output,4);
 }
 if(fault==2)job->mapped[3][255]^=1;
 if(fault==3)job->mapped[0][256]^=1;
 return VK_SUCCESS;
}
static VkResult poll(VkDevice d,void *data,uint64_t *serial)
{ (void)d;*serial=fault==5?0:((struct dot_job*)data)->serial;return VK_SUCCESS; }
static void release(VkDevice d,void *data) { (void)d;free(data); }
static uint64_t clock_now(void *ctx) { (void)ctx;return elapsed_ns; }
static void pause_poll(void *ctx,uint64_t timeout) { (void)ctx;assert(fault==5 && timeout<=UINT64_C(300000000));elapsed_ns+=timeout; }
static uint32_t read32(void) { uint32_t v;assert(fread(&v,4,1,stdin)==1);return v; }
int main(int argc,char **argv)
{
 unsigned timeout_test=argc==2 && !strcmp(argv[1],"--timeout");
 VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
 VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
 VkInstance instance;assert(vkCreateInstance(&ici,NULL,&instance)==VK_SUCCESS);
 VkPhysicalDevice physical;uint32_t count=1;assert(vkEnumeratePhysicalDevices(instance,&count,&physical)==VK_SUCCESS);
 float priority=1;VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueCount=1,.pQueuePriorities=&priority};
 VkPhysicalDeviceShaderIntegerDotProductFeatures feature={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES,.shaderIntegerDotProduct=VK_TRUE};
 const char *extension=VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME;
 VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&feature,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci,
  .enabledExtensionCount=1,.ppEnabledExtensionNames=&extension};
 VkDevice d;assert(vkCreateDevice(physical,&dci,NULL,&d)==VK_SUCCESS);
 d->compiler.compile=ps5vk_compiler_adapter_compile;ps5vk_device_enable_runtime_compiler(d);
 d->submit_backend=(struct ps5vk_queue_backend){prepare,launch,poll,release};
 d->progress=(struct ps5vk_progress){NULL,ps5vk_queue_poll,clock_now,pause_poll};
 VkQueue queue;vkGetDeviceQueue(d,0,0,&queue);
 for(unsigned c=0;c<42;c++) {
  mode=read32();components=read32();saturating=read32();packed=read32();unsigned bytes=read32();
  uint32_t *spirv=malloc(bytes);assert(spirv && fread(spirv,1,bytes,stdin)==bytes);
  struct integer_dot_data data={0};unsigned char *allocations[4];
  for(unsigned b=0;b<4;b++) {
   unsigned size=b>=2?512:128*(packed?4:components==2?8:16);
   allocations[b]=malloc(size);assert(allocations[b] && fread(allocations[b],1,size,stdin)==size);
   if(b<3) {data.inputs[b]=allocations[b];data.sizes[b]=size;} else data.expected=allocations[b];
  }
  for(fault=timeout_test?5:0;fault<(timeout_test?6:5);fault++) {
   launches=0;VkBool32 pending=VK_TRUE;struct integer_dot_result observed;
   VkResult r=integer_dot_compute_witness(d,queue,spirv,bytes,&data,&pending,&observed);
   if(timeout_test) {
    assert(r==VK_TIMEOUT && pending && launches==1 && elapsed_ns==UINT64_C(300000000));
    assert(!observed.outputs && !observed.mismatches && d->submission && d->pipeline_objects &&
      d->descriptor_objects && d->buffers && d->memories && d->command_pools && d->fences && !d->lifetime_errors);
    /* Deliberately retained in-flight objects belong to this isolated process.
     * Exiting is the test's teardown; never pretend a pending GPU has retired. */
    puts("bounded timeout retained live resources without reading results");fflush(stdout);_Exit(0);
   }
   if(r!=(fault?VK_ERROR_UNKNOWN:VK_SUCCESS))fprintf(stderr,"case=%u fault=%u result=%d step=%s\n",c,fault,r,observed.step);
   assert(r==(fault?VK_ERROR_UNKNOWN:VK_SUCCESS) && !pending && launches==1);
   assert(observed.outputs==128 && observed.guards==(fault==2) && observed.input_changes==(fault==3));
   assert(observed.mismatches==((fault==1 || fault==4)?128u:0u));
   assert(!d->pipeline_objects && !d->descriptor_objects && !d->buffers && !d->memories && !d->command_pools && !d->fences && !d->lifetime_errors);
  }
  for(unsigned b=0;b<4;b++)free(allocations[b]);
  free(spirv);
 }
 vkDestroyDevice(d,NULL);vkDestroyInstance(instance,NULL);
 puts("42 integer-dot variants, real compiler, synthetic queue and four fault classes passed; no GPU evidence");
 return 0;
}
