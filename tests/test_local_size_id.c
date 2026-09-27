#include "vk_internal.h"
#include "vk_pipeline.h"
#include "compilation_cache.h"
#include "ps5vk_compiler.h"
#include "physical_device_profile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t *read_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (sz % 4) != 0) { fclose(f); return NULL; }
    uint32_t *buf = malloc(sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_size = (size_t)sz;
    return buf;
}

static VkResult alloc_memory(void *ctx, VkDeviceSize size, void **address, void **backing)
{
    (void)ctx; *address = malloc(size); *backing = *address;
    return *address ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void free_memory(void *ctx, void *backing) { (void)ctx; free(backing); }
static VkResult cache_flush(void *ctx, void *backing, VkDeviceSize offset, VkDeviceSize size)
{ (void)ctx; (void)backing; (void)offset; (void)size; return VK_SUCCESS; }
static VkResult open_backend(void *ctx, struct ps5vk_memory_backend *backend)
{
    (void)ctx;
    *backend = (struct ps5vk_memory_backend){NULL, alloc_memory, free_memory, cache_flush, cache_flush};
    return VK_SUCCESS;
}
static void close_backend(struct ps5vk_memory_backend *backend) { (void)backend; }

VkResult ps5vk_platform_query(struct ps5vk_platform *p)
{
    *p = (struct ps5vk_platform){.open = open_backend, .close = close_backend,
                                 .max_allocation = 65536, .queue_flags = VK_QUEUE_COMPUTE_BIT};
    const struct ps5vk_physical_profile_info profile = {
        .name = "host mock, not a GPU",
        .heap_size = 65536,
        .allocation_granularity = 1,
        .buffer_image_granularity = 1,
    };
    ps5vk_physical_profile_init(&p->properties, &p->memory_properties, &profile);
    return VK_SUCCESS;
}

/* LocalSizeId (maintenance4): the pinned minimal compute module rewritten in
 * place from OpExecutionMode LocalSize 64 1 1 to OpExecutionModeId
 * LocalSizeId naming its own 32-bit OpConstants 64 and 1 (same length), with
 * the header raised to SPIR-V 1.2, where OpExecutionModeId exists. */
static int rewrite_local_size_id(uint32_t *w, size_t count, uint32_t x_id, uint32_t one_id)
{
    for (size_t i = 5; i < count; i += w[i] >> 16) {
        if ((w[i] & 0xffff) == 16 && w[i] >> 16 == 6 && w[i + 2] == 17) {
            if (w[i + 3] != 64 || w[i + 4] != 1 || w[i + 5] != 1) return 0;
            w[i] = 6u << 16 | 331u; w[i + 2] = 38;
            w[i + 3] = x_id; w[i + 4] = one_id; w[i + 5] = one_id;
            w[1] = 0x00010200u;
            return 1;
        }
    }
    return 0;
}
static uint32_t constant_id(const uint32_t *w, size_t count, uint32_t value)
{
    uint32_t type = 0;
    for (size_t i = 5; i < count; i += w[i] >> 16)
        if ((w[i] & 0xffff) == 21 && w[i] >> 16 == 4 && w[i + 2] == 32 && w[i + 3] == 0)
            type = w[i + 1];
    for (size_t i = 5; i < count; i += w[i] >> 16)
        if ((w[i] & 0xffff) == 43 && w[i] >> 16 == 4 && w[i + 1] == type && w[i + 3] == value)
            return w[i + 2];
    return 0;
}
static VkResult build_specialized(VkDevice device, VkPipelineLayout layout, const uint32_t *words,
                      size_t bytes, const VkSpecializationInfo *specialization, VkPipeline *out)
{
    VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = bytes, .pCode = words};
    VkShaderModule module;
    assert(vkCreateShaderModule(device, &smci, NULL, &module) == VK_SUCCESS);
    VkComputePipelineCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .layout = layout, .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main",
            .pSpecializationInfo = specialization}};
    *out = VK_NULL_HANDLE;
    VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL, out);
    vkDestroyShaderModule(device, module, NULL);
    return result;
}
static VkResult build(VkDevice device, VkPipelineLayout layout, const uint32_t *words,
                      size_t bytes, VkPipeline *out)
{
    return build_specialized(device, layout, words, bytes, NULL, out);
}

/* Turn the X dimension into a decorated OpSpecConstant without changing the
 * module's ID space. Annotations precede types, as required by SPIR-V. */
static uint32_t *specialize_dimension(const uint32_t *words, size_t count, uint32_t id,
                                      uint32_t spec_id)
{
    size_t types = 5;
    while (types < count && (words[types] & 0xffff) != 19)
        types += words[types] >> 16;
    assert(types < count);
    uint32_t *out = malloc((count + 4) * sizeof(*out));
    assert(out);
    memcpy(out, words, types * sizeof(*out));
    uint32_t decoration[4] = {4u << 16 | 71u, id, 1u, spec_id};
    memcpy(out + types, decoration, sizeof(decoration));
    memcpy(out + types + 4, words + types, (count - types) * sizeof(*out));
    unsigned found = 0;
    for (size_t i = 5; i < count + 4; i += out[i] >> 16)
        if (((out[i] & 0xffff) == 43 || (out[i] & 0xffff) == 41 ||
             (out[i] & 0xffff) == 42) && out[i + 2] == id) {
            unsigned opcode = (out[i] & 0xffff) + 7;
            out[i] = (out[i] & 0xffff0000u) | opcode;
            ++found;
        }
    assert(found == 1);
    return out;
}

/* Append a scalar OpSpecConstantOp before functions and make X consume it.
 * Both admission and the real compiler must independently derive its value. */
static uint32_t *expression_dimension(const uint32_t *words, size_t count,
                                     uint32_t lhs, uint32_t rhs, unsigned op)
{
    size_t functions = 5;
    uint32_t type = 0;
    for (; functions < count; functions += words[functions] >> 16) {
        if (((words[functions] & 0xffff) == 50 || (words[functions] & 0xffff) == 52) &&
            words[functions + 2] == lhs)
            type = words[functions + 1];
        if ((words[functions] & 0xffff) == 54) break;
    }
    assert(type && functions < count);
    uint32_t *out = malloc((count + 6) * sizeof(*out));
    assert(out);
    memcpy(out, words, functions * sizeof(*out));
    uint32_t expression[] = {6u << 16 | 52u, type, words[3], op, lhs, rhs};
    if (op == 126 || op == 200) {
        expression[0] = 5u << 16 | 52u;
        expression[5] = 1u << 16; /* OpNop padding. */
    }
    memcpy(out + functions, expression, sizeof(expression));
    memcpy(out + functions + 6, words + functions, (count - functions) * sizeof(*out));
    out[3]++;
    for (size_t i = 5; i < functions;) {
        unsigned length = out[i] >> 16;
        if ((out[i] & 0xffff) == 331 && out[i + 2] == 38) out[i + 3] = words[3];
        /* The legacy fixture also declares BuiltIn WorkgroupSize, which
         * overrides execution modes. Remove that decoration for this test. */
        if ((out[i] & 0xffff) == 71 && length == 4 && out[i + 2] == 11 && out[i + 3] == 25)
            for (unsigned j = 0; j < length; ++j) out[i + j] = 1u << 16;
        i += length;
    }
    return out;
}

/* Select a literal launch size from a specialized integer comparison or a
 * boolean expression. Keep the selected sizes independent of the compared
 * value so negative signed inputs exercise comparisons without huge launches. */
static uint32_t *conditional_dimension(const uint32_t *words, size_t count,
                                      uint32_t lhs, uint32_t one, unsigned op)
{
    size_t functions = 5;
    uint32_t type = 0, boolean_type = 0;
    for (; functions < count; functions += words[functions] >> 16) {
        if ((words[functions] & 0xffff) == 50 && words[functions + 2] == lhs)
            type = words[functions + 1];
        if ((words[functions] & 0xffff) == 20) boolean_type = words[functions + 1];
        if ((words[functions] & 0xffff) == 54) break;
    }
    assert(type && functions < count);
    uint32_t base = words[3];
    uint32_t boolean = boolean_type ? boolean_type : base;
    uint32_t declarations[] = {
        2u << 16 | 20u, base,                         /* bool */
        4u << 16 | 43u, type, base + 1, 32,          /* uint 32 */
        3u << 16 | 41u, boolean, base + 2,              /* true */
        3u << 16 | 42u, boolean, base + 3,              /* false */
        6u << 16 | 52u, boolean, base + 4, op, lhs, one,
        7u << 16 | 52u, type, base + 5, 169, base + 4, base + 1, one,
    };
    if (boolean_type) declarations[0] = declarations[1] = 1u << 16;
    if (op >= 164 && op <= 168) {
        declarations[16] = base + 2;
        declarations[17] = base + 3;
        if (op == 168) {
            declarations[12] = 5u << 16 | 52u;
            declarations[17] = 1u << 16;
        }
    }
    const size_t extra = sizeof(declarations) / sizeof(*declarations);
    assert(extra == 25);
    uint32_t *out = malloc((count + extra) * sizeof(*out));
    assert(out);
    memcpy(out, words, functions * sizeof(*out));
    memcpy(out + functions, declarations, sizeof(declarations));
    memcpy(out + functions + extra, words + functions, (count - functions) * sizeof(*out));
    out[3] += 6;
    for (size_t i = 5; i < functions;) {
        unsigned length = out[i] >> 16;
        if ((out[i] & 0xffff) == 331 && out[i + 2] == 38) out[i + 3] = base + 5;
        if ((out[i] & 0xffff) == 71 && length == 4 && out[i + 2] == 11 && out[i + 3] == 25)
            for (unsigned j = 0; j < length; ++j) out[i + j] = 1u << 16;
        i += length;
    }
    return out;
}

/* Construct, insert, shuffle, then extract a vector lane. All indices are
 * literal operands; the source components still carry scalar specialization. */
static uint32_t *vector_dimension(const uint32_t *words, size_t count,
                                  uint32_t x, uint32_t one, unsigned source, unsigned lane)
{
    size_t functions = 5;
    uint32_t type = 0, vector_type = 0;
    for (; functions < count; functions += words[functions] >> 16) {
        if (((words[functions] & 0xffff) == 50 || (words[functions] & 0xffff) == 48 ||
             (words[functions] & 0xffff) == 49) && words[functions + 2] == x)
            type = words[functions + 1];
        if ((words[functions] & 0xffff) == 54) break;
    }
    assert(type && functions < count && source < 3);
    for (size_t i = 5; i < functions; i += words[i] >> 16)
        if ((words[i] & 0xffff) == 23 && words[i + 2] == type && words[i + 3] == 4)
            vector_type = words[i + 1];
    uint32_t base = words[3], vt = vector_type ? vector_type : base;
    uint32_t declarations[] = {
        4u << 16 | 23u, base, type, 4,
        7u << 16 | 51u, vt, base + 1, x, one, x, one,
        7u << 16 | 52u, vt, base + 2, 82, one, base + 1, 0,
        10u << 16 | 52u, vt, base + 3, 79, base + 1, base + 2, 6, 4, 1, 0,
        6u << 16 | 52u, type, base + 4, 81, base + 1 + source, lane,
    };
    if (vector_type)
        for (unsigned i = 0; i < 4; ++i) declarations[i] = 1u << 16;
    assert(sizeof(declarations) == 34 * sizeof(uint32_t));
    uint32_t *out = malloc((count + 34) * sizeof(*out));
    assert(out);
    memcpy(out, words, functions * sizeof(*out));
    memcpy(out + functions, declarations, sizeof(declarations));
    memcpy(out + functions + 34, words + functions, (count - functions) * sizeof(*out));
    out[3] += 5;
    for (size_t i = 5; i < functions;) {
        unsigned length = out[i] >> 16;
        if ((out[i] & 0xffff) == 331 && out[i + 2] == 38) out[i + 3] = base + 4;
        if ((out[i] & 0xffff) == 71 && length == 4 && out[i + 2] == 11 && out[i + 3] == 25)
            for (unsigned j = 0; j < length; ++j) out[i + j] = 1u << 16;
        i += length;
    }
    return out;
}

/* Feed a boolean vector extraction to an integer select instead of using
 * the boolean directly as a launch dimension. */
static uint32_t *boolean_vector_dimension(const uint32_t *words, size_t count,
                                          uint32_t yes, uint32_t no, unsigned source, unsigned lane)
{
    uint32_t *vector = vector_dimension(words, count, yes, no, source, lane);
    size_t functions = 5;
    uint32_t one = constant_id(words, count, 1), size32 = constant_id(words, count, 32), type = 0;
    for (; functions < count + 34; functions += vector[functions] >> 16) {
        if ((vector[functions] & 0xffff) == 43 && vector[functions + 2] == one)
            type = vector[functions + 1];
        if ((vector[functions] & 0xffff) == 54) break;
    }
    assert(one && size32 && type && functions < count + 34);
    uint32_t *out = malloc((count + 41) * sizeof(*out));
    assert(out);
    memcpy(out, vector, functions * sizeof(*out));
    uint32_t select[] = {7u << 16 | 52u, type, vector[3], 169, vector[3] - 1, size32, one};
    memcpy(out + functions, select, sizeof(select));
    memcpy(out + functions + 7, vector + functions, (count + 34 - functions) * sizeof(*out));
    out[3]++;
    for (size_t i = 5; i < functions; i += out[i] >> 16)
        if ((out[i] & 0xffff) == 331 && out[i + 2] == 38) out[i + 3] = vector[3];
    free(vector);
    return out;
}

/* form: scalar integer, scalar boolean, integer vector, boolean vector. */
static uint32_t *null_form_dimension(const uint32_t *words,size_t count,uint32_t lhs,
                                      uint32_t one,unsigned form,size_t *out_count)
{
    size_t functions=5;uint32_t integer=0,boolean=0,vector=0,next=words[3];
    for(;functions<count;functions+=words[functions]>>16) {
        if((words[functions]&0xffff)==43 && words[functions+2]==lhs)integer=words[functions+1];
        if((words[functions]&0xffff)==20)boolean=words[functions+1];
        if((words[functions]&0xffff)==54)break;
    }
    assert(integer && functions<count);
    uint32_t extra[32];unsigned n=0;
#define WORD(v) do {extra[n++]=(v);} while(0)
    if((form&1) && !boolean) {boolean=next++;WORD(2u<<16|20u);WORD(boolean);}
    uint32_t element=(form&1)?boolean:integer;
    if(form>=2) {
        for(size_t i=5;i<functions;i+=words[i]>>16)
            if((words[i]&0xffff)==23 && words[i]>>16==4 && words[i+2]==element && words[i+3]==2)vector=words[i+1];
        if(!vector) {vector=next++;WORD(4u<<16|23u);WORD(vector);WORD(element);WORD(2);}
    }
    uint32_t null=next++;WORD(3u<<16|46u);WORD(form>=2?vector:element);WORD(null);
    uint32_t operand=null;
    if(form>=2) {operand=next++;WORD(6u<<16|52u);WORD(element);WORD(operand);WORD(81);WORD(null);WORD(1);}
    uint32_t result=next++;
    if(form&1) {WORD(7u<<16|52u);WORD(integer);WORD(result);WORD(169);WORD(operand);WORD(one);WORD(lhs);}
    else {WORD(6u<<16|52u);WORD(integer);WORD(result);WORD(128);WORD(lhs);WORD(operand);}
#undef WORD
    *out_count=count+n;uint32_t *out=malloc(*out_count*4);assert(out);
    memcpy(out,words,functions*4);memcpy(out+functions,extra,n*4);
    memcpy(out+functions+n,words+functions,(count-functions)*4);out[3]=next;
    for(size_t i=5;i<functions;) {
        unsigned length=out[i]>>16;
        if((out[i]&0xffff)==331 && out[i+2]==38)out[i+3]=result;
        if((out[i]&0xffff)==71 && length==4 && out[i+2]==11 && out[i+3]==25)
            for(unsigned j=0;j<length;++j)out[i+j]=1u<<16;
        i+=length;
    }
    return out;
}

int main(void)
{
    size_t bytes = 0;
    uint32_t *literal = read_file("build/test-shaders/minimal.spv", &bytes);
    assert(literal);
    const size_t count = bytes / 4;
    uint32_t *by_id = malloc(bytes), *spec = malloc(bytes);
    assert(by_id && spec);
    memcpy(by_id, literal, bytes);
    const uint32_t id64 = constant_id(literal, count, 64), id1 = constant_id(literal, count, 1);
    assert(id64 && id1 && rewrite_local_size_id(by_id, count, id64, id1));
    /* An operand that is not an OpConstant (here the uint type id) is refused. */
    memcpy(spec, literal, bytes);
    uint32_t type = 0;
    for (size_t i = 5; i < count; i += literal[i] >> 16)
        if ((literal[i] & 0xffff) == 21) { type = literal[i + 1]; break; }
    assert(type && rewrite_local_size_id(spec, count, type, id1));

    VkInstance instance;
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    assert(vkCreateInstance(&ici, NULL, &instance) == VK_SUCCESS);
    uint32_t physical_count = 1;
    VkPhysicalDevice physical;
    assert(vkEnumeratePhysicalDevices(instance, &physical_count, &physical) == VK_SUCCESS);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    VkDevice device;
    assert(vkCreateDevice(physical, &dci, NULL, &device) == VK_SUCCESS);
    device->compiler.compile = ps5vk_compiler_adapter_compile;
    ps5vk_device_enable_runtime_compiler(device);
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo slci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings};
    VkDescriptorSetLayout sl;
    assert(vkCreateDescriptorSetLayout(device, &slci, NULL, &sl) == VK_SUCCESS);
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &sl};
    VkPipelineLayout layout;
    assert(vkCreatePipelineLayout(device, &plci, NULL, &layout) == VK_SUCCESS);

    VkPipeline reference, pipeline;
    assert(build(device, layout, literal, bytes, &reference) == VK_SUCCESS);
    assert(reference->program.local_size[0] == 64 && reference->program.local_size[1] == 1 &&
           reference->program.local_size[2] == 1);
    /* Without maintenance4 the LocalSizeId form is refused. */
    assert(build(device, layout, by_id, bytes, &pipeline) != VK_SUCCESS && !pipeline);
    /* Set the feature directly to isolate shader admission from device
     * negotiation. The compiled workgroup/code equal the literal form. */
    device->enabled_features_t09 |= PS5VK_T09_FEATURE_MAINTENANCE4;
    /* Null integer/bool scalars and vectors may participate in a nonzero
     * LocalSizeId expression. The old frontend refused these while the real
     * compiler independently resolved every form to 64x1x1. */
    for (unsigned form=0; form<4; ++form) {
        size_t expr_count;
        uint32_t *expr=null_form_dimension(by_id,count,id64,id1,form,&expr_count);
        VkPipeline selected=NULL;
        assert(build(device,layout,expr,expr_count*4,&selected)==VK_SUCCESS);
        assert(selected->program.local_size[0]==64 && selected->program.local_size[1]==1 &&
               selected->program.local_size[2]==1);
        vkDestroyPipeline(device,selected,NULL);
        size_t null_at=0, types_at=0;
        for (size_t i=5;i<expr_count;i+=expr[i]>>16) {
            unsigned op=expr[i]&0xffff;
            if (op==46) null_at=i;
            if (!types_at && op>=19 && op<=39) types_at=i;
        }
        assert(null_at && types_at);
        /* A null is not a specialization constant. Reject its SpecId even
         * when the caller supplies no specialization map. */
        uint32_t *decorated=malloc((expr_count+4)*4);assert(decorated);
        memcpy(decorated,expr,types_at*4);
        const uint32_t annotation[]={4u<<16|71u,expr[null_at+2],1,17};
        memcpy(decorated+types_at,annotation,16);
        memcpy(decorated+types_at+4,expr+types_at,(expr_count-types_at)*4);
        VkPipeline refused=NULL;
        assert(build(device,layout,decorated,(expr_count+4)*4,&refused)!=VK_SUCCESS && !refused);
        free(decorated);
        /* Extra operands must not turn a malformed ConstantNull into zero. */
        uint32_t *malformed=malloc((expr_count+1)*4);assert(malformed);
        memcpy(malformed,expr,(null_at+3)*4);malformed[null_at]=4u<<16|46u;
        malformed[null_at+3]=0;
        memcpy(malformed+null_at+4,expr+null_at+3,(expr_count-null_at-3)*4);
        assert(build(device,layout,malformed,(expr_count+1)*4,&refused)!=VK_SUCCESS && !refused);
        free(malformed);
        if (!(form&1)) {
            /* Direct scalar zero, including extraction from a null vector,
             * remains an invalid launch dimension. */
            for (size_t i=5;i<expr_count;i+=expr[i]>>16)
                if ((expr[i]&0xffff)==331 && expr[i+2]==38) expr[i+3]=expr[3]-2;
            assert(build(device,layout,expr,expr_count*4,&refused)!=VK_SUCCESS && !refused);
        }
        free(expr);
    }

    assert(build(device, layout, by_id, bytes, &pipeline) == VK_SUCCESS);
    assert(!memcmp(pipeline->program.local_size, reference->program.local_size,
                   sizeof(reference->program.local_size)));
    assert(pipeline->program.code_words == reference->program.code_words &&
           !memcmp(pipeline->program.code, reference->program.code,
                   reference->program.code_words * 4));
    VkPipeline refused;
    assert(build(device, layout, spec, bytes, &refused) != VK_SUCCESS && !refused);

    uint32_t *specialized = specialize_dimension(by_id, count, id64, 7);
    VkPipeline default_size, size32, size16, warm32;
    assert(build(device, layout, specialized, bytes + 16, &default_size) == VK_SUCCESS);
    assert(default_size->program.local_size[0] == 64);
    uint32_t x = 32;
    VkSpecializationMapEntry map = {.constantID = 7, .offset = 0, .size = sizeof(x)};
    VkSpecializationInfo specialization = {.mapEntryCount = 1, .pMapEntries = &map,
        .dataSize = sizeof(x), .pData = &x};
    assert(build_specialized(device, layout, specialized, bytes + 16, &specialization,
                             &size32) == VK_SUCCESS);
    assert(size32->program.local_size[0] == 32 && size32->program.local_size[1] == 1 &&
           size32->program.local_size[2] == 1);
    x = 16;
    assert(build_specialized(device, layout, specialized, bytes + 16, &specialization,
                             &size16) == VK_SUCCESS);
    assert(size16->program.local_size[0] == 16);
    x = 32;
    assert(build_specialized(device, layout, specialized, bytes + 16, &specialization,
                             &warm32) == VK_SUCCESS);
    assert(warm32->program.local_size[0] == 32);
    /* SPIR-V 1.0 glslang uses BuiltIn WorkgroupSize instead of LocalSizeId.
     * Its specialized dimensions override the literal execution mode and do
     * not require maintenance4. This is the route used by original CTS. */
    uint32_t *legacy = specialize_dimension(literal, count, id64, 7);
    device->enabled_features_t09 &= ~PS5VK_T09_FEATURE_MAINTENANCE4;
    for (unsigned without_mode = 0; without_mode < 2; ++without_mode) {
        for (unsigned repeat = 0; repeat < 3; ++repeat) {
            VkPipeline selected;
            x = repeat == 1 ? 16 : 32;
            assert(build_specialized(device, layout, legacy, bytes + 16, &specialization,
                                     &selected) == VK_SUCCESS);
            assert(selected->program.local_size[0] == x && selected->program.local_size[1] == 1 &&
                   selected->program.local_size[2] == 1);
            vkDestroyPipeline(device, selected, NULL);
        }
        for (size_t i = 5; i < count + 4;) {
            unsigned length = legacy[i] >> 16;
            if ((legacy[i] & 0xffff) == 16 && length == 6 && legacy[i + 2] == 17)
                for (unsigned j = 0; j < length; ++j) legacy[i + j] = 1u << 16;
            i += length;
        }
    }
    x = 0;
    assert(build_specialized(device, layout, legacy, bytes + 16, &specialization,
                             &refused) != VK_SUCCESS && !refused);
    for (size_t i = 5; i < count + 4; i += legacy[i] >> 16)
        if ((legacy[i] & 0xffff) == 71 && (legacy[i] >> 16) == 4 && legacy[i + 2] == 11 && legacy[i + 3] == 25)
            legacy[i + 1] = id64; /* Scalar BuiltIn is malformed. */
    x = 32;
    assert(build_specialized(device, layout, legacy, bytes + 16, &specialization,
                             &refused) != VK_SUCCESS && !refused);
    free(legacy);
    device->enabled_features_t09 |= PS5VK_T09_FEATURE_MAINTENANCE4;
    const unsigned specialized_lane[3][4] = {{1, 0, 1, 0}, {0, 0, 1, 0}, {1, 0, 0, 1}};
    for (unsigned source = 0; source < 3; ++source) {
        for (unsigned lane = 0; lane < 4; ++lane) {
            uint32_t *vector = vector_dimension(specialized, count + 4, id64, id1, source, lane);
            VkPipeline selected;
            assert(build(device, layout, vector, bytes + 152, &selected) == VK_SUCCESS);
            assert(selected->program.local_size[0] == (specialized_lane[source][lane] ? 64 : 1));
            vkDestroyPipeline(device, selected, NULL);
            for (unsigned repeat = 0; repeat < 3; ++repeat) {
                x = repeat == 1 ? 16 : 32;
                assert(build_specialized(device, layout, vector, bytes + 152, &specialization,
                                         &selected) == VK_SUCCESS);
                assert(selected->program.local_size[0] == (specialized_lane[source][lane] ? x : 1));
                vkDestroyPipeline(device, selected, NULL);
            }
            device->enabled_features_t09 &= ~PS5VK_T09_FEATURE_MAINTENANCE4;
            assert(build(device, layout, vector, bytes + 152, &refused) != VK_SUCCESS && !refused);
            device->enabled_features_t09 |= PS5VK_T09_FEATURE_MAINTENANCE4;
            free(vector);
        }
    }
    /* Reject malformed indices, unsupported undefined shuffle lanes and a cycle in an
     * unselected component before the real compiler sees the malformed DAG. */
    for (unsigned invalid = 0; invalid < 5; ++invalid) {
        uint32_t *vector = vector_dimension(specialized, count + 4, id64, id1, 2, 0);
        for (size_t i = 5; i < count + 38; i += vector[i] >> 16) {
            unsigned op = vector[i] & 0xffff;
            if (op == 52 && vector[i + 3] == 81 && invalid == 0) vector[i + 5] = 4;
            if (op == 52 && vector[i + 3] == 82 && invalid == 1) vector[i + 6] = 4;
            if (op == 52 && vector[i + 3] == 79 && (invalid == 2 || invalid == 3))
                vector[i + 9] = invalid == 2 ? 8 : UINT32_MAX;
            if (op == 51 && vector[i + 2] == specialized[3] + 1 && invalid == 4)
                vector[i + 6] = specialized[3] + 4;
        }
        assert(build(device, layout, vector, bytes + 152, &refused) != VK_SUCCESS && !refused);
        free(vector);
    }
    x = 32;
    const unsigned ops[] = {128, 130, 132, 134, 135, 137, 138, 139, 194, 195, 196, 197, 198, 199};
    const uint32_t expected[] = {33, 31, 32, 32, 32, 0, 0, 0, 16, 16, 64, 33, 33, 0};
    for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) {
        uint32_t *expression = expression_dimension(specialized, count + 4, id64, id1, ops[i]);
        VkPipeline expr_pipeline;
        VkResult result = build_specialized(device, layout, expression, bytes + 40,
                                            &specialization, &expr_pipeline);
        if (expected[i]) {
            if (result != VK_SUCCESS) fprintf(stderr, "expression op=%u rc=%d\n", ops[i], result);
            assert(result == VK_SUCCESS);
            assert(expr_pipeline->program.local_size[0] == expected[i]);
            vkDestroyPipeline(device, expr_pipeline, NULL);
        } else assert(result != VK_SUCCESS && !expr_pipeline);
        free(expression);
    }
    const unsigned condition_ops[] = {164, 165, 166, 167, 168, 170, 171, 172,
                                      173, 174, 175, 176, 177, 178, 179};
    const uint32_t positive[] = {1, 32, 32, 1, 1, 1, 32, 32, 32, 32, 32, 1, 1, 1, 1};
    const uint32_t negative[] = {1, 32, 32, 1, 1, 1, 32, 32, 1, 32, 1, 1, 32, 1, 32};
    for (unsigned i = 0; i < sizeof(condition_ops) / sizeof(condition_ops[0]); ++i) {
        uint32_t *conditional = conditional_dimension(specialized, count + 4, id64, id1,
                                                      condition_ops[i]);
        VkPipeline selected;
        assert(build(device, layout, conditional, bytes + 116, &selected) == VK_SUCCESS);
        assert(selected->program.local_size[0] == positive[i]);
        vkDestroyPipeline(device, selected, NULL);
        x = UINT32_MAX;
        assert(build_specialized(device, layout, conditional, bytes + 116, &specialization,
                                 &selected) == VK_SUCCESS);
        assert(selected->program.local_size[0] == negative[i]);
        vkDestroyPipeline(device, selected, NULL);
        free(conditional);
    }
    uint32_t *boolean_input = conditional_dimension(specialized, count + 4, id64, id1, 166);
    uint32_t *boolean_spec = specialize_dimension(boolean_input, count + 29, specialized[3] + 2, 9);
    VkBool32 truth = VK_FALSE;
    VkSpecializationMapEntry truth_map = {.constantID = 9, .size = sizeof(truth)};
    VkSpecializationInfo truth_info = {.mapEntryCount = 1, .pMapEntries = &truth_map,
        .dataSize = sizeof(truth), .pData = &truth};
    for (unsigned i = 0; i < 2; ++i) {
        VkPipeline selected;
        truth = i ? 2 : VK_FALSE; /* Every nonzero VkBool32 represents true. */
        assert(build_specialized(device, layout, boolean_spec, bytes + 132, &truth_info,
                                 &selected) == VK_SUCCESS);
        assert(selected->program.local_size[0] == (i ? 32 : 1));
        vkDestroyPipeline(device, selected, NULL);
    }
    for (unsigned source = 0; source < 3; ++source) {
        for (unsigned lane = 0; lane < 4; ++lane) {
            uint32_t *vector = boolean_vector_dimension(boolean_spec, count + 33,
                specialized[3] + 2, specialized[3] + 3, source, lane);
            for (unsigned input = 0; input < 2; ++input) {
                VkPipeline selected;
                truth = input ? 2 : VK_FALSE;
                assert(build_specialized(device, layout, vector, bytes + 296, &truth_info,
                                         &selected) == VK_SUCCESS);
                assert(selected->program.local_size[0] ==
                       (input && specialized_lane[source][lane] ? 32 : 1));
                vkDestroyPipeline(device, selected, NULL);
            }
            free(vector);
        }
    }
    /* A select condition must be boolean, and a workgroup dimension must
     * remain integer even though its expression may consume booleans. */
    for (size_t i = 5; i < count + 29; i += boolean_input[i] >> 16)
        if ((boolean_input[i] & 0xffff) == 52 && boolean_input[i + 3] == 169)
            boolean_input[i + 4] = id1;
    assert(build(device, layout, boolean_input, bytes + 116, &refused) != VK_SUCCESS && !refused);
    for (size_t i = 5; i < count + 33; i += boolean_spec[i] >> 16)
        if ((boolean_spec[i] & 0xffff) == 331 && boolean_spec[i + 2] == 38)
            boolean_spec[i + 3] = specialized[3] + 2;
    assert(build(device, layout, boolean_spec, bytes + 132, &refused) != VK_SUCCESS && !refused);
    free(boolean_input); free(boolean_spec);
    x = 32;
    const unsigned unary_ops[] = {126, 200};
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t *first = expression_dimension(specialized, count + 4, id64, 0, unary_ops[i]);
        uint32_t *second = expression_dimension(first, count + 10, first[3] - 1, 0, unary_ops[i]);
        VkPipeline nested;
        assert(build_specialized(device, layout, second, bytes + 64, &specialization,
                                 &nested) == VK_SUCCESS);
        assert(nested->program.local_size[0] == 32);
        vkDestroyPipeline(device, nested, NULL);
        free(first); free(second);
    }
    /* Invalid divisions and oversized shift counts are rejected without
     * asking the compiler to evaluate undefined SPIR-V results. */
    const unsigned invalid_ops[] = {134, 135, 137, 138, 139, 194, 195, 196};
    const uint32_t id0 = constant_id(literal, count, 0);
    assert(id0);
    for (unsigned i = 0; i < sizeof(invalid_ops) / sizeof(invalid_ops[0]); ++i) {
        uint32_t *invalid = expression_dimension(specialized, count + 4, id64,
                                                i < 5 ? id0 : id64, invalid_ops[i]);
        assert(build_specialized(device, layout, invalid, bytes + 40, &specialization,
                                 &refused) != VK_SUCCESS && !refused);
        free(invalid);
    }
    uint32_t *expression = expression_dimension(specialized, count + 4, id64, id1, 128);
    VkPipeline expr_default, expr_override;
    assert(build(device, layout, expression, bytes + 40, &expr_default) == VK_SUCCESS);
    assert(expr_default->program.local_size[0] == 65);
    x = 16;
    assert(build_specialized(device, layout, expression, bytes + 40, &specialization,
                             &expr_override) == VK_SUCCESS);
    assert(expr_override->program.local_size[0] == 17);
    vkDestroyPipeline(device, expr_default, NULL);
    vkDestroyPipeline(device, expr_override, NULL);
    /* A cyclic result ID must terminate before entering the compiler. */
    for (size_t i = 5; i < count + 10; i += expression[i] >> 16)
        if ((expression[i] & 0xffff) == 52) expression[i + 4] = expression[i + 2];
    assert(build(device, layout, expression, bytes + 40, &refused) != VK_SUCCESS && !refused);
    free(expression);
    x = 32;
    map.size = 2;
    assert(build_specialized(device, layout, specialized, bytes + 16, &specialization,
                             &refused) != VK_SUCCESS && !refused);
    map.size = sizeof(x); map.offset = sizeof(x);
    assert(build_specialized(device, layout, specialized, bytes + 16, &specialization,
                             &refused) != VK_SUCCESS && !refused);
    map.offset = 0;
    VkSpecializationMapEntry duplicates[2] = {map, map};
    specialization.mapEntryCount = 2; specialization.pMapEntries = duplicates;
    assert(build_specialized(device, layout, specialized, bytes + 16, &specialization,
                             &refused) != VK_SUCCESS && !refused);
    specialization.mapEntryCount = 1; specialization.pMapEntries = NULL;
    assert(build_specialized(device, layout, specialized, bytes + 16, &specialization,
                             &refused) != VK_SUCCESS && !refused);
    vkDestroyPipeline(device, warm32, NULL);
    vkDestroyPipeline(device, size16, NULL);
    vkDestroyPipeline(device, size32, NULL);
    vkDestroyPipeline(device, default_size, NULL);
    free(specialized);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipeline(device, reference, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDescriptorSetLayout(device, sl, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    free(literal); free(by_id); free(spec);
    puts("LocalSizeId: scalar and vector specialization expressions under maintenance4 "
         "(host compiler, no GPU evidence)");
    return 0;
}
