/* SPDX-License-Identifier: GPL-3.0-or-later
 * Host compiler interface inspection; no GPU execution.
 */
#include "runtime_graphics_compiler.h"
#include "spirv_graphics_interface.h"
#include "vk_descriptor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct ps5vk_graphics_module_key read_module(const char *path) {
    struct ps5vk_graphics_module_key m={0};
    if(!strcmp(path,"-"))return m;
    FILE *f=fopen(path,"rb"); if(!f)exit(2);
    fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
    uint32_t *w=malloc(n);if(!w || n<=0 || n%4 || fread(w,1,n,f)!=(size_t)n)exit(3);
    fclose(f);m.words=w;m.word_count=n/4;m.entry="main";return m;
}
int main(int argc,char **argv) {
    if(argc!=6)return 4;
    struct ps5vk_graphics_key k={
        .vertex=read_module(argv[1]),.tess_control=read_module(argv[2]),
        .tess_eval=read_module(argv[3]),.geometry=read_module(argv[4]),
        .fragment=read_module(argv[5]),.maintenance4=VK_TRUE,
        .feature_mask=PS5VK_FEATURE_GEOMETRY_SHADER|PS5VK_FEATURE_TESSELLATION_SHADER,
        .color_format={VK_FORMAT_B8G8R8A8_UNORM},.color_attachment_count=1,
        .samples=VK_SAMPLE_COUNT_1_BIT,.color_write_mask={15}};
    k.topology=k.tess_control.words?VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    k.patch_control_points=k.tess_control.words?3:0;
    int enabled=ps5vk_spirv_graphics_interface(&k);
    k.maintenance4=VK_FALSE;int disabled=ps5vk_spirv_graphics_interface(&k);k.maintenance4=VK_TRUE;
    const void *out=NULL;VkResult rc=ps5vk_runtime_graphics_compile(NULL,&k,&out);
    int code=0;
    if(out) {
        const struct ps5vk_runtime_graphics_program *program=out;
        code=program->fragment.machine_code_size && (k.tess_control.words?
            (program->hull.machine_code_size && program->domain.machine_code_size):
            program->vertex.machine_code_size);
    }
    printf("{\"enabled\":%d,\"disabled\":%d,\"result\":%d,\"program\":%d,\"code\":%d}\n",
        enabled,disabled,rc,out!=NULL,code);
    if(out)ps5vk_runtime_graphics_free(NULL,out);
    free((void*)k.vertex.words);free((void*)k.tess_control.words);free((void*)k.tess_eval.words);
    free((void*)k.geometry.words);free((void*)k.fragment.words);
    return rc==VK_SUCCESS?0:1;
}
