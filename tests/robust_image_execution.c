/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real frontend/compiler, synthetic image instructions: no GPU evidence. */
#define _POSIX_C_SOURCE 200112L
#include "vk_internal.h"
#include "vk_pipeline.h"
#include "vk_queue.h"
#include "vk_image.h"
#include "ps5vk_compiler.h"
#include "physical_device_profile.h"
#include "texture_layout.h"
#include "texture_copy.h"
#include "descriptor_encode.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../examples/robust_image_witness/compute.h"
#include "fixtures.h"

static VkResult allocate(void *ctx,VkDeviceSize n,void **a,void **b)
{(void)ctx;*a=NULL;if(posix_memalign(a,256,n))return VK_ERROR_OUT_OF_HOST_MEMORY;*b=*a;return VK_SUCCESS;}
static void deallocate(void *ctx,void *p){(void)ctx;free(p);}
static VkResult sync_memory(void *ctx,void *p,VkDeviceSize o,VkDeviceSize n)
{(void)ctx;(void)p;(void)o;(void)n;return VK_SUCCESS;}
static VkResult open_memory(void *ctx,struct ps5vk_memory_backend *b)
{(void)ctx;*b=(struct ps5vk_memory_backend){NULL,allocate,deallocate,sync_memory,sync_memory};return VK_SUCCESS;}
static void close_memory(struct ps5vk_memory_backend *b){(void)b;}
VkResult ps5vk_platform_query(struct ps5vk_platform *p)
{
 *p=(struct ps5vk_platform){.open=open_memory,.close=close_memory,.max_allocation=1u<<20,
  .queue_flags=VK_QUEUE_COMPUTE_BIT,.supported_features=PS5VK_FEATURE_ROBUST_BUFFER_ACCESS,
  .supported_features_t09=PS5VK_T09_FEATURE_ROBUST_BUFFER_ACCESS2,
  .supported_features_v13=PS5VK_V13_FEATURE_ROBUST_IMAGE_ACCESS};
 const struct ps5vk_physical_profile_info info={.name="synthetic image queue",.heap_size=1u<<20,
  .allocation_granularity=256,.buffer_image_granularity=1};
 ps5vk_physical_profile_init(&p->properties,&p->memory_properties,&info);return VK_SUCCESS;
}
static unsigned fault, launches;
static uint64_t elapsed;
struct job {uint64_t serial;const struct ps5vk_submission *submission;};
static VkResult prepare(VkDevice d,const struct ps5vk_submission *s,void **out)
{
 (void)d;struct job *j=calloc(1,sizeof(*j));assert(j);j->serial=s->serial;j->submission=s;*out=j;return VK_SUCCESS;
}
static VkResult dispatch(VkDevice d,const struct ps5vk_operation *op)
{
 ++launches;
 assert(op->groups[0]==1 && op->groups[1]==1 && op->groups[2]==1);
 assert(op->pipeline->program.code_words && op->pipeline->program.local_size[0]==32);
 assert(op->pipeline->program.descriptor_count==3);
 VkDescriptorSet set=op->sets[0];assert(set);
 uint32_t table[20]={0};VkDeviceSize dynamic[PS5VK_MAX_DYNAMIC_DESCRIPTORS]={0};
 assert(ps5vk_descriptor_encode(d,&op->pipeline->program,0,set,dynamic,table,20)==VK_SUCCESS);
 void *coordinates,*results,*resource;VkDeviceSize coord_bytes,result_bytes,resource_bytes;
 assert(ps5vk_buffer_span(d,set->buffers[1].buffer,set->buffers[1].offset,set->buffers[1].range,&coordinates,&coord_bytes)==VK_SUCCESS);
 assert(ps5vk_buffer_span(d,set->buffers[2].buffer,set->buffers[2].offset,set->buffers[2].range,&results,&result_bytes)==VK_SUCCESS);
 assert(coord_bytes==result_bytes && coord_bytes%16==0 && coord_bytes<=512);
 const VkDescriptorType type=set->signature.type[0];
 const VkBool32 texel=type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
 VkFormat format;unsigned width,height=1,slices=1,axes=1;uint64_t row=0,slice=0;
 if(texel) {
  VkBufferView view=set->texel_views[0];assert(view);format=view->format;
  assert(ps5vk_buffer_span(d,view->buffer,view->offset,view->range,&resource,&resource_bytes)==VK_SUCCESS);
  width=resource_bytes/4;
 } else {
  VkImageView view=set->images[0].imageView;VkImage image=view->image;format=view->format;
  assert(ps5vk_image_span(d,image,&resource,&resource_bytes)==VK_SUCCESS);
  width=image->info.extent.width;height=image->info.extent.height;
  slices=image->info.imageType==VK_IMAGE_TYPE_3D?image->info.extent.depth:image->info.arrayLayers;
  axes=view->view_type==VK_IMAGE_VIEW_TYPE_1D?1:(view->view_type==VK_IMAGE_VIEW_TYPE_2D || view->view_type==VK_IMAGE_VIEW_TYPE_1D_ARRAY?2:3);
  struct ps5vk_texture_layout layout;assert(!ps5vk_texture_layout_for_slices(format,width,height,slices,&layout));
  row=layout.row_pitch;slice=layout.slice_pitch;
  if(view->view_type==VK_IMAGE_VIEW_TYPE_1D_ARRAY){height=slices;slices=1;row=slice;}
 }
 /* The fixture is passed independently of its expected values. Use only the
  * write/read operation selection; fetch data from the bound resource. */
 extern VkBool32 current_write;
 for(unsigned i=0;i<coord_bytes/16;++i) {
  int32_t c[4];memcpy(c,(unsigned char*)coordinates+16*i,16);
  VkBool32 valid=c[0]>=0 && (unsigned)c[0]<width;
  if(axes>1)valid &= c[1]>=0 && (unsigned)c[1]<height;
  if(axes>2)valid &= c[2]>=0 && (unsigned)c[2]<slices;
  uint64_t offset=valid?(uint64_t)c[0]*4+(axes>1?(uint64_t)c[1]*row:0)+(axes>2?(uint64_t)c[2]*slice:0):0;
  uint32_t value[4]={0,0,0,1};
  if(fault==4)continue;
  if(current_write) {
   if(valid){uint32_t stored=UINT32_C(0x54600007)+i*257u;assert(offset+4<=resource_bytes);memcpy((unsigned char*)resource+offset,&stored,4);}
   value[0]=i;value[1]=0x7351;
  } else if(valid) {
   assert(offset+4<=resource_bytes);
   if(format==VK_FORMAT_R32_UINT)memcpy(value,(unsigned char*)resource+offset,4);
   else {assert(format==VK_FORMAT_R8G8B8A8_UINT);for(unsigned k=0;k<4;++k)value[k]=((unsigned char*)resource)[offset+k];}
  }
  if(fault==1)value[0]^=1;
  memcpy((unsigned char*)results+16*i,value,16);
 }
 if(fault==2)((unsigned char*)results)[-1]^=1;
 if(fault==3)((unsigned char*)coordinates)[0]^=1;
 return VK_SUCCESS;
}
static void copy_image(VkDevice d,const struct ps5vk_operation *op)
{
 void *image,*buffer;VkDeviceSize image_bytes,buffer_bytes;
 VkBool32 upload=op->type==PS5VK_COPY_BUFFER_IMAGE;
 assert(ps5vk_image_span(d,op->copy_image,&image,&image_bytes)==VK_SUCCESS);
 assert(ps5vk_buffer_span(d,upload?op->copy_source:op->copy_destination,0,VK_WHOLE_SIZE,&buffer,&buffer_bytes)==VK_SUCCESS);
 struct ps5vk_texture_copy p;
 assert(ps5vk_texture_copy_plan_for_image(op->copy_image,buffer_bytes,image_bytes,&op->copy_region,&p)==VK_SUCCESS);
 for(unsigned z=0;z<p.slices;++z)for(unsigned y=0;y<p.rows;++y) {
  unsigned char *a=(unsigned char*)buffer+p.source_offset+z*p.source_slice_pitch+y*p.source_pitch;
  unsigned char *b=(unsigned char*)image+p.destination_offset+z*p.destination_slice_pitch+y*p.destination_pitch;
  if(upload)memcpy(b,a,p.row_bytes);else memcpy(a,b,p.row_bytes);
 }
}
static VkResult launch(VkDevice d,void *opaque)
{
 const struct ps5vk_submission *s=((struct job*)opaque)->submission;
 for(unsigned b=0;b<s->count;++b) {
  unsigned first=ps5vk_submission_first_operation(s,b),count=ps5vk_submission_operation_count(s,b);
  for(unsigned i=first;i<first+count;++i) {
   const struct ps5vk_operation *op=&s->buffers[b]->operations[i];
   if(op->type==PS5VK_DISPATCH)assert(dispatch(d,op)==VK_SUCCESS);
   else if(op->type==PS5VK_IMAGE_BARRIER)op->image_barrier.image->layout=op->image_barrier.newLayout;
   else if(op->type==PS5VK_COPY_BUFFER_IMAGE || op->type==PS5VK_COPY_IMAGE_BUFFER)copy_image(d,op);
   else assert(op->type==PS5VK_BARRIER);
  }
 }
 return VK_SUCCESS;
}

VkBool32 current_write;
static VkResult poll(VkDevice d,void *j,uint64_t *serial){(void)d;*serial=(fault==5 && launches)?0:((struct job*)j)->serial;return VK_SUCCESS;}
static void release(VkDevice d,void *j){(void)d;free(j);}
static uint64_t now(void *c){(void)c;return elapsed;}
static void pause_poll(void *c,uint64_t ns){(void)c;assert(ns<=300000000);elapsed+=(fault==5 && launches)?ns:(ns<1000?ns:1000);}
int main(int argc,char **argv)
{
 VkBool32 timeout=argc==2 && !strcmp(argv[1],"--timeout");
 VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
 VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};VkInstance instance;
 assert(vkCreateInstance(&ici,NULL,&instance)==VK_SUCCESS);VkPhysicalDevice physical;uint32_t count=1;
 assert(vkEnumeratePhysicalDevices(instance,&count,&physical)==VK_SUCCESS);
 float priority=1;VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueCount=1,.pQueuePriorities=&priority};
 VkPhysicalDeviceImageRobustnessFeatures f={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES,.robustImageAccess=VK_TRUE};
 VkPhysicalDeviceRobustness2FeaturesEXT r2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,.pNext=&f,.robustBufferAccess2=VK_TRUE};
 VkPhysicalDeviceFeatures core={.robustBufferAccess=VK_TRUE};const char *ext=VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;
 VkDeviceCreateInfo ci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&r2,.pEnabledFeatures=&core,
  .queueCreateInfoCount=1,.pQueueCreateInfos=&qci,.enabledExtensionCount=1,.ppEnabledExtensionNames=&ext};VkDevice d;
 assert(vkCreateDevice(physical,&ci,NULL,&d)==VK_SUCCESS);
 d->graphics_enabled=VK_TRUE;d->graphics_submit_enabled=VK_TRUE;d->image_requirements=ps5vk_native_image_requirements;
 d->compiler.compile=ps5vk_compiler_adapter_compile;ps5vk_device_enable_runtime_compiler(d);
 d->submit_backend=(struct ps5vk_queue_backend){prepare,launch,poll,release};
 d->progress=(struct ps5vk_progress){NULL,ps5vk_queue_poll,now,pause_poll};VkQueue queue;vkGetDeviceQueue(d,0,0,&queue);
 for(unsigned i=0;i<11;++i)for(fault=timeout?5:0;fault<(timeout?6:5);++fault) {
  current_write=fixtures[i]->write;launches=0;elapsed=0;VkBool32 pending;struct robust_image_result observed;
  VkResult result=robust_image_compute(d,queue,shaders[i],shader_bytes[i],fixtures[i],&pending,&observed);
  if(timeout && result==VK_TIMEOUT) {
   assert(pending && launches==1 && elapsed==300000000 && !observed.outputs && d->submission && d->memories && !d->lifetime_errors);
   puts("bounded timeout retains in-flight image resources");fflush(stdout);_Exit(0);
  }
  if(result!=(fault?VK_ERROR_UNKNOWN:VK_SUCCESS))fprintf(stderr,"case=%u fault=%u result=%d step=%s\n",i,fault,result,observed.step);
  assert(result==(fault?VK_ERROR_UNKNOWN:VK_SUCCESS) && !pending && launches==1);
  assert(observed.outputs==fixtures[i]->count && observed.guards==(fault==2) && observed.input_changes==(fault==3));
  assert((observed.mismatches!=0)==(fault==1 || fault==4));
  assert(!d->pipeline_objects && !d->descriptor_objects && !d->buffers && !d->memories && !d->command_pools && !d->fences && !d->lifetime_errors);
 }
 vkDestroyDevice(d,NULL);vkDestroyInstance(instance,NULL);puts("11 variants: real compiler, synthetic image execution, corruption controls; no GPU evidence");return 0;
}
