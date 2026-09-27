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

/* Compare integer vectors, select integer vectors, and extract a dimension. */
static uint32_t *comparison_vector_dimension(const uint32_t *words, size_t count,
    uint32_t x, uint32_t one, unsigned comparison, int scalar_condition, size_t *out_count)
{
    size_t functions = 5;
    uint32_t integer = 0, boolean = 0, iv = 0, bv = 0, next = words[3];
    for (; functions < count; functions += words[functions] >> 16) {
        const uint32_t *w = words + functions;
        if ((w[0] & 0xffff) == 50 && w[2] == x) integer = w[1];
        if ((w[0] & 0xffff) == 20) boolean = w[1];
        if ((w[0] & 0xffff) == 54) break;
    }
    assert(integer && functions < count);
    for (size_t i = 5; i < functions; i += words[i] >> 16) {
        const uint32_t *w = words + i;
        if ((w[0] & 0xffff) == 23 && w[3] == 2) {
            if (w[2] == integer) iv = w[1];
            if (boolean && w[2] == boolean) bv = w[1];
        }
    }
    uint32_t extra[96]; unsigned n = 0;
#define WORD(v) do { assert(n < 96); extra[n++] = (v); } while (0)
    if (!boolean) { boolean = next++; WORD(2u << 16 | 20u); WORD(boolean); }
    if (!iv) { iv = next++; WORD(4u << 16 | 23u); WORD(iv); WORD(integer); WORD(2); }
    if (!bv) { bv = next++; WORD(4u << 16 | 23u); WORD(bv); WORD(boolean); WORD(2); }
    uint32_t thirty_two = constant_id(words, count, 32);
    if (!thirty_two) { thirty_two = next++; WORD(4u << 16 | 43u); WORD(integer); WORD(thirty_two); WORD(32); }
    uint32_t a = next++, b = next++, c = next++, cond = next++, selected = next++, result = next++;
    WORD(5u << 16 | 51u); WORD(iv); WORD(a); WORD(x); WORD(one);
    WORD(5u << 16 | 51u); WORD(iv); WORD(b); WORD(one); WORD(one);
    WORD(5u << 16 | 51u); WORD(iv); WORD(c); WORD(thirty_two); WORD(one);
    WORD(6u << 16 | 52u); WORD(bv); WORD(cond); WORD(comparison); WORD(a); WORD(b);
    if (scalar_condition) {
        uint32_t scalar = next++;
        WORD(6u << 16 | 52u); WORD(boolean); WORD(scalar); WORD(81); WORD(cond); WORD(0);
        cond = scalar;
    }
    WORD(7u << 16 | 52u); WORD(iv); WORD(selected); WORD(169); WORD(cond); WORD(c); WORD(b);
    WORD(6u << 16 | 52u); WORD(integer); WORD(result); WORD(81); WORD(selected); WORD(0);
#undef WORD
    *out_count = count + n;
    uint32_t *out = malloc(*out_count * sizeof(*out)); assert(out);
    memcpy(out, words, functions * 4); memcpy(out + functions, extra, n * 4);
    memcpy(out + functions + n, words + functions, (count - functions) * 4);
    out[3] = next;
    if (scalar_condition) out[1] = 0x00010400u;
    for (size_t i = 5; i < functions;) {
        unsigned length = out[i] >> 16;
        if ((out[i] & 0xffff) == 331 && out[i + 2] == 38) out[i + 3] = result;
        if ((out[i] & 0xffff) == 71 && length == 4 && out[i + 2] == 11 && out[i + 3] == 25)
            for (unsigned j = 0; j < length; ++j) out[i + j] = 1u << 16;
        i += length;
    }
    if (scalar_condition) {
        /* SPIR-V 1.4 requires all used global variables in the entry interface. */
        uint32_t globals[16]; unsigned global_count = 0;
        size_t entry = 0;
        for (size_t i = 5; i < *out_count; i += out[i] >> 16) {
            unsigned op = out[i] & 0xffff;
            if (op == 15) entry = i;
            if (op == 54) break;
            if (op == 59 && out[i + 3] != 1) {
                assert(global_count < 16); globals[global_count++] = out[i + 2];
            }
        }
        assert(entry && global_count);
        size_t end = entry + (out[entry] >> 16);
        out = realloc(out, (*out_count + global_count) * sizeof(*out)); assert(out);
        memmove(out + end + global_count, out + end, (*out_count - end) * sizeof(*out));
        memcpy(out + end, globals, global_count * sizeof(*out));
        out[entry] += global_count << 16;
        *out_count += global_count;
    }
    return out;
}

static uint32_t *aggregate_dimension(const uint32_t *words, size_t count,
    uint32_t x, uint32_t one, unsigned form, size_t *out_count)
{
    size_t functions = 5; uint32_t integer = 0, next = words[3];
    for (; functions < count; functions += words[functions] >> 16) {
        const uint32_t *w = words + functions;
        if ((w[0] & 0xffff) == 50 && w[2] == x) integer = w[1];
        if ((w[0] & 0xffff) == 54) break;
    }
    assert(integer && functions < count);
    uint32_t extra[64]; unsigned n = 0;
#define WORD(v) do { assert(n < 64); extra[n++] = (v); } while (0)
    uint32_t type = next++, aggregate = next++, result = next++;
    if (form == 0) {
        WORD(4u << 16 | 30u); WORD(type); WORD(integer); WORD(integer);
        WORD(5u << 16 | 51u); WORD(type); WORD(aggregate); WORD(x); WORD(one);
    } else {
        WORD(4u << 16 | 28u); WORD(type); WORD(integer); WORD(one);
        WORD(4u << 16 | 51u); WORD(type); WORD(aggregate); WORD(x);
        if (form == 2) {
            uint32_t structure = next++, wrapped = next++;
            WORD(3u << 16 | 30u); WORD(structure); WORD(type);
            WORD(4u << 16 | 51u); WORD(structure); WORD(wrapped); WORD(aggregate);
            aggregate = wrapped;
        }
    }
    WORD((form == 2 ? 7u : 6u) << 16 | 52u); WORD(integer); WORD(result); WORD(81); WORD(aggregate); WORD(0);
    if (form == 2) WORD(0);
#undef WORD
    *out_count = count + n;
    uint32_t *out = malloc(*out_count * 4); assert(out);
    memcpy(out, words, functions * 4); memcpy(out + functions, extra, n * 4);
    memcpy(out + functions + n, words + functions, (count - functions) * 4); out[3] = next;
    for (size_t i = 5; i < functions;) {
        unsigned length = out[i] >> 16;
        if ((out[i] & 0xffff) == 331 && out[i + 2] == 38) out[i + 3] = result;
        if ((out[i] & 0xffff) == 71 && length == 4 && out[i + 2] == 11 && out[i + 3] == 25)
            for (unsigned j = 0; j < length; ++j) out[i + j] = 1u << 16;
        i += length;
    }
    return out;
}

static uint32_t *aggregate_vector_dimension(const uint32_t *words, size_t count,
    uint32_t x, uint32_t one, unsigned form, unsigned mode, size_t *out_count)
{
    size_t functions = 5; uint32_t integer = 0, vector = 0, next = words[3];
    for (; functions < count; functions += words[functions] >> 16) {
        const uint32_t *w = words + functions;
        if ((w[0] & 0xffff) == 50 && w[2] == x) integer = w[1];
        if ((w[0] & 0xffff) == 54) break;
    }
    assert(integer && functions < count);
    for (size_t i = 5; i < functions; i += words[i] >> 16)
        if ((words[i] & 0xffff) == 23 && words[i + 2] == integer && words[i + 3] == 2) vector = words[i + 1];
    uint32_t extra[96]; unsigned n = 0;
#define WORD(v) do { assert(n < 96); extra[n++] = (v); } while (0)
    if (!vector) { vector = next++; WORD(4u << 16 | 23u); WORD(vector); WORD(integer); WORD(2); }
    uint32_t type = next++, a = next++, b = next++, aggregate = next++;
    WORD((form ? 4u : 3u) << 16 | (form ? 28u : 30u)); WORD(type); WORD(vector); if (form) WORD(one);
    WORD(5u << 16 | 51u); WORD(vector); WORD(a); WORD(x); WORD(one);
    WORD(5u << 16 | 51u); WORD(vector); WORD(b); WORD(one); WORD(x);
    if (mode == 1) { WORD(3u << 16 | 46u); WORD(type); WORD(aggregate); }
    else { WORD(4u << 16 | 51u); WORD(type); WORD(aggregate); WORD(a); }
    if (mode >= 2) {
        uint32_t inserted = next++;
        WORD((mode == 2 ? 7u : 8u) << 16 | 52u); WORD(type); WORD(inserted); WORD(82);
        WORD(mode == 2 ? b : one); WORD(aggregate); WORD(0);
        if (mode > 2) WORD(mode == 4 ? 1 : 0);
        aggregate = inserted;
    }
    uint32_t extracted_vector = next++, scalar = next++, result = next++;
    WORD(6u << 16 | 52u); WORD(vector); WORD(extracted_vector); WORD(81); WORD(aggregate); WORD(0);
    WORD(6u << 16 | 52u); WORD(integer); WORD(scalar); WORD(81); WORD(extracted_vector); WORD(0);
    WORD(6u << 16 | 52u); WORD(integer); WORD(result); WORD(128); WORD(scalar); WORD(one);
#undef WORD
    *out_count = count + n;
    uint32_t *out = malloc(*out_count * 4); assert(out);
    memcpy(out, words, functions * 4); memcpy(out + functions, extra, n * 4);
    memcpy(out + functions + n, words + functions, (count - functions) * 4); out[3] = next;
    for (size_t i = 5; i < functions;) {
        unsigned length = out[i] >> 16;
        if ((out[i] & 0xffff) == 331 && out[i + 2] == 38) out[i + 3] = result;
        if ((out[i] & 0xffff) == 71 && length == 4 && out[i + 2] == 11 && out[i + 3] == 25)
            for (unsigned j = 0; j < length; ++j) out[i + j] = 1u << 16;
        i += length;
    }
    return out;
}

/* Round-trip a specialized 32-bit integer through Int8/Int16, either as a scalar
 * or every lane of a vector. The sign probe turns the high bit into a bounded
 * workgroup size, distinguishing sign extension from zero extension.
 * Forms: 0 direct, 1 sign probe, 2 narrow addition with wraparound,
 * 3 aggregate extraction, 4 cross-width chain, 5 cross-width sign probe. */
static uint32_t *converted_dimension(const uint32_t *words, size_t count,
    uint32_t input, uint32_t one, unsigned op, unsigned lanes, unsigned bits, int conversion_form,
    size_t *out_count)
{
    size_t functions = 5; uint32_t integer = 0;
    for (; functions < count; functions += words[functions] >> 16) {
        if ((words[functions] & 0xffff) == 50 && words[functions + 2] == input)
            integer = words[functions + 1];
        if ((words[functions] & 0xffff) == 54) break;
    }
    assert(integer && functions < count && lanes >= 1 && lanes <= 4);
    uint32_t extra[96], next = words[3]; unsigned n = 0;
#define WORD(v) do { assert(n < 96); extra[n++] = (v); } while (0)
    uint32_t short_type = next++, narrow_type = short_type, wide_type = integer;
    WORD(4u << 16 | 21u); WORD(short_type); WORD(bits); WORD(0);
    if (lanes > 1) {
        narrow_type = next++; wide_type = 0;
        WORD(4u << 16 | 23u); WORD(narrow_type); WORD(short_type); WORD(lanes);
        for (size_t at = 5; at < functions; at += words[at] >> 16)
            if ((words[at] & 0xffff) == 23 && words[at + 2] == integer && words[at + 3] == lanes)
                wide_type = words[at + 1];
        if (!wide_type) {
            wide_type = next++;
            WORD(4u << 16 | 23u); WORD(wide_type); WORD(integer); WORD(lanes);
        }
        uint32_t vector = next++;
        WORD((3u + lanes) << 16 | 51u); WORD(wide_type); WORD(vector);
        for (unsigned lane = 0; lane < lanes; ++lane) WORD(input);
        input = vector;
    }
    uint32_t narrow = next++, wide = next++;
    WORD(5u << 16 | 52u); WORD(narrow_type); WORD(narrow); WORD(op); WORD(input);
    if (conversion_form == 2) {
        uint32_t two = next++, adjusted = next++;
        WORD(4u << 16 | 43u); WORD(short_type); WORD(two); WORD(2);
        if (lanes > 1) {
            uint32_t vector = next++;
            WORD((3u + lanes) << 16 | 51u); WORD(narrow_type); WORD(vector);
            for (unsigned lane = 0; lane < lanes; ++lane) WORD(two);
            two = vector;
        }
        WORD(6u << 16 | 52u); WORD(narrow_type); WORD(adjusted); WORD(128); WORD(narrow); WORD(two);
        narrow = adjusted;
    }
    if (conversion_form == 3) {
        uint32_t structure = next++, aggregate = next++, extracted = next++;
        WORD(3u << 16 | 30u); WORD(structure); WORD(narrow_type);
        WORD(4u << 16 | 51u); WORD(structure); WORD(aggregate); WORD(narrow);
        WORD(6u << 16 | 52u); WORD(narrow_type); WORD(extracted); WORD(81); WORD(aggregate); WORD(0);
        narrow = extracted;
    }
    if (conversion_form >= 4) {
        uint32_t other_scalar = next++, other_type = other_scalar, converted = next++;
        WORD(4u << 16 | 21u); WORD(other_scalar); WORD(bits == 8 ? 16 : 8); WORD(0);
        if (lanes > 1) {
            other_type = next++;
            WORD(4u << 16 | 23u); WORD(other_type); WORD(other_scalar); WORD(lanes);
        }
        WORD(5u << 16 | 52u); WORD(other_type); WORD(converted); WORD(op); WORD(narrow);
        narrow = converted;
    }
    WORD(5u << 16 | 52u); WORD(wide_type); WORD(wide); WORD(op); WORD(narrow);
    uint32_t result = wide;
    if (lanes > 1) {
        result = next++;
        WORD(6u << 16 | 52u); WORD(integer); WORD(result); WORD(81); WORD(wide); WORD(lanes - 1);
    }
    if (conversion_form == 1 || conversion_form == 5) {
        uint32_t shift = next++, high = next++, bounded = next++;
        WORD(4u << 16 | 43u); WORD(integer); WORD(shift); WORD(31);
        WORD(6u << 16 | 52u); WORD(integer); WORD(high); WORD(194); WORD(result); WORD(shift);
        WORD(6u << 16 | 52u); WORD(integer); WORD(bounded); WORD(128); WORD(high); WORD(one);
        result = bounded;
    }
#undef WORD
    *out_count = count + n + 2;
    uint32_t *out = malloc(*out_count * 4); assert(out);
    memcpy(out, words, 20); out[1] = 0x00010400u; out[3] = next;
    out[5] = 2u << 16 | 17u; out[6] = bits == 16 ? 22 : 39; /* Int16 or Int8 */
    memcpy(out + 7, words + 5, (functions - 5) * 4);
    memcpy(out + functions + 2, extra, n * 4);
    memcpy(out + functions + 2 + n, words + functions, (count - functions) * 4);
    for (size_t i = 7; i < functions + 2;) {
        unsigned length = out[i] >> 16;
        if ((out[i] & 0xffff) == 331 && out[i + 2] == 38) out[i + 3] = result;
        if ((out[i] & 0xffff) == 71 && length == 4 && out[i + 2] == 11 && out[i + 3] == 25)
            for (unsigned j = 0; j < length; ++j) out[i + j] = 1u << 16;
        i += length;
    }
    /* The seed uses only SSBO Uniform pointers with BufferBlock. Their
     * SPIR-V 1.4 form is StorageBuffer plus Block. */
    for (size_t i = 7; i < functions + 2; i += out[i] >> 16) {
        unsigned opcode = out[i] & 0xffff;
        if (opcode == 71 && out[i] >> 16 == 3 && out[i + 2] == 3) out[i + 2] = 2;
        if (opcode == 32 && out[i + 2] == 2) out[i + 2] = 12;
        if (opcode == 59 && out[i + 3] == 2) out[i + 3] = 12;
    }
    /* SPIR-V 1.4 includes descriptor globals in the entry-point interface. */
    uint32_t globals[64]; unsigned global_count = 0; size_t entry_at = 0;
    for (size_t i = 5; i < *out_count; i += out[i] >> 16) {
        unsigned opcode = out[i] & 0xffff;
        if (opcode == 54) break;
        if (opcode == 15) entry_at = i;
        if (opcode == 59) { assert(global_count < 64); globals[global_count++] = out[i + 2]; }
    }
    assert(entry_at);
    unsigned entry_length = out[entry_at] >> 16;
    size_t name_words = (strlen((char *)(out + entry_at + 3)) + 4) / 4;
    uint32_t missing[64]; unsigned missing_count = 0;
    for (unsigned g = 0; g < global_count; ++g) {
        int present = 0;
        for (size_t i = 3 + name_words; i < entry_length; ++i)
            if (out[entry_at + i] == globals[g]) present = 1;
        if (!present) missing[missing_count++] = globals[g];
    }
    out = realloc(out, (*out_count + missing_count) * 4); assert(out);
    size_t after_entry = entry_at + entry_length;
    memmove(out + after_entry + missing_count, out + after_entry, (*out_count - after_entry) * 4);
    memcpy(out + after_entry, missing, missing_count * 4);
    out[entry_at] = (entry_length + missing_count) << 16 | 15u;
    *out_count += missing_count;
    /* Earlier fixture helpers use Nop padding. It is not valid in the
     * declaration section, so compact it out of these standalone modules. */
    size_t compact = 5;
    for (size_t at = 5; at < *out_count;) {
        unsigned length = out[at] >> 16;
        if ((out[at] & 0xffff) != 0) {
            memmove(out + compact, out + at, length * 4); compact += length;
        }
        at += length;
    }
    *out_count = compact;
    if (conversion_form >= 4) {
        out = realloc(out, (*out_count + 2) * 4); assert(out);
        memmove(out + 7, out + 5, (*out_count - 5) * 4);
        out[5] = 2u << 16 | 17u; out[6] = bits == 8 ? 22 : 39;
        *out_count += 2;
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
    device->enabled_features |= PS5VK_FEATURE_SHADER_INT16;
    device->platform_features |= PS5VK_FEATURE_SHADER_INT8_COMPUTE;
    for (unsigned bits = 8; bits <= 16; bits += 8)
    for (unsigned op = 113; op <= 114; ++op)
    for (unsigned lanes = 1; lanes <= 4; ++lanes)
    for (unsigned conversion_form = 0; conversion_form < 6; ++conversion_form) {
        uint32_t *seed = specialize_dimension(by_id, count, id64, 7);
        size_t converted_count;
        uint32_t *converted = converted_dimension(seed, count + 4, id64, id1,
            op, lanes, bits, conversion_form, &converted_count);
        for (unsigned specialized = 0; specialized < 2; ++specialized) {
            uint32_t value = conversion_form == 1 || conversion_form == 2 || conversion_form == 5 ? (1u << bits) - 1 : (1u << bits) + 3;
            uint32_t expected = conversion_form == 2 ? (specialized ? 1 : 66) :
                (conversion_form == 1 || conversion_form == 5) ? (specialized && op == 114 ? 2 : 1) : (specialized ? 3 : 64);
            VkSpecializationMapEntry entry = {7, 0, 4};
            VkSpecializationInfo info = {1, &entry, 4, &value};
            const VkSpecializationInfo *map = specialized ? &info : NULL;
            struct ps5vk_compiled_program program = {0}; uint32_t *code = NULL;
            assert(ps5vk_runtime_compile_compute_features(converted, converted_count,
                "main", layout, map, PS5VK_FEATURE_SHADER_INT16 | PS5VK_FEATURE_SHADER_INT8_COMPUTE, &program, &code) == VK_SUCCESS);
            assert(code && program.local_size[0] == expected); free(code);
            VkPipeline candidate = NULL;
            VkResult result = build_specialized(device, layout, converted, converted_count * 4, map, &candidate);
            if (result != VK_SUCCESS) fprintf(stderr, "conversion op=%u lanes=%u form=%u spec=%u result=%d\n",
                op, lanes, conversion_form, specialized, result);
            assert(result == VK_SUCCESS && candidate && candidate->program.local_size[0] == expected);
            vkDestroyPipeline(device, candidate, NULL);
        }
        VkPipeline refused = NULL;
        size_t short_type_at = 0, first_conversion = 0;
        for (size_t at = 5; at < converted_count; at += converted[at] >> 16) {
            if ((converted[at] & 0xffff) == 21 && converted[at + 2] == bits) short_type_at = at;
            if (!first_conversion && (converted[at] & 0xffff) == 52 && converted[at + 3] == op)
                first_conversion = at;
        }
        assert(short_type_at && first_conversion);
        /* A conversion must change width and must consume a valid noncyclic
         * integer definition. These fail before reaching the compiler. */
        converted[short_type_at + 2] = 32;
        assert(build(device, layout, converted, converted_count * 4, &refused) != VK_SUCCESS && !refused);
        converted[short_type_at + 2] = bits;
        uint32_t operand = converted[first_conversion + 4];
        const uint32_t invalid_operands[] = {0, converted[3], converted[first_conversion + 2]};
        for (unsigned invalid = 0; invalid < 3; ++invalid) {
            converted[first_conversion + 4] = invalid_operands[invalid];
            assert(build(device, layout, converted, converted_count * 4, &refused) != VK_SUCCESS && !refused);
        }
        converted[first_conversion + 4] = operand;
        if (op == 113) {
            converted[short_type_at + 3] = 1; /* UConvert requires an unsigned destination. */
            assert(build(device, layout, converted, converted_count * 4, &refused) != VK_SUCCESS && !refused);
            converted[short_type_at + 3] = 0;
        }
        if (lanes == 1) {
            for (size_t at = 5; at < converted_count; at += converted[at] >> 16) {
                if ((converted[at] & 0xffff) == 331 && converted[at + 2] == 38) {
                    uint32_t old = converted[at + 3];
                    converted[at + 3] = converted[first_conversion + 2];
                    assert(build(device, layout, converted, converted_count * 4, &refused) != VK_SUCCESS && !refused);
                    converted[at + 3] = old;
                }
            }
            /* Specialization storage follows the narrowed scalar's byte size. */
            converted[first_conversion] = 4u << 16 | 43u;
            converted[first_conversion + 3] = 3;
            memmove(converted + first_conversion + 4, converted + first_conversion + 5,
                (converted_count - first_conversion - 5) * 4);
            --converted_count;
            uint32_t *narrow_spec = specialize_dimension(converted, converted_count,
                converted[first_conversion + 2], 8);
            uint16_t value = conversion_form == 1 || conversion_form == 2 || conversion_form == 5 ? (1u << bits) - 1 : 5;
            VkSpecializationMapEntry entry = {8, 0, bits / 8};
            VkSpecializationInfo info = {1, &entry, bits / 8, &value};
            struct ps5vk_compiled_program program = {0}; uint32_t *code = NULL;
            uint32_t expected = conversion_form == 2 ? 1 : (conversion_form == 1 || conversion_form == 5) ? (op == 114 ? 2 : 1) : 5;
            assert(ps5vk_runtime_compile_compute_features(narrow_spec, converted_count + 4,
                "main", layout, &info, PS5VK_FEATURE_SHADER_INT16 | PS5VK_FEATURE_SHADER_INT8_COMPUTE, &program, &code) == VK_SUCCESS);
            assert(code && program.local_size[0] == expected); free(code);
            VkPipeline candidate = NULL;
            assert(build_specialized(device, layout, narrow_spec, (converted_count + 4) * 4,
                &info, &candidate) == VK_SUCCESS && candidate && candidate->program.local_size[0] == expected);
            vkDestroyPipeline(device, candidate, NULL);
            entry.size = 4;
            assert(build_specialized(device, layout, narrow_spec, (converted_count + 4) * 4,
                &info, &refused) != VK_SUCCESS && !refused);
            free(narrow_spec);
        }
        free(converted); free(seed);
    }
    device->enabled_features &= ~PS5VK_FEATURE_SHADER_INT16;
    device->platform_features &= ~PS5VK_FEATURE_SHADER_INT8_COMPUTE;
    for (unsigned form = 0; form < 3; ++form) {
        uint32_t *seed = specialize_dimension(by_id, count, id64, 7);
        size_t expression_count;
        uint32_t *expr = aggregate_dimension(seed, count + 4, id64, id1, form, &expression_count);
        for (unsigned specialized = 0; specialized < 2; ++specialized) {
            uint32_t value = 32;
            VkSpecializationMapEntry entry = {7, 0, 4};
            VkSpecializationInfo info = {1, &entry, 4, &value};
            const VkSpecializationInfo *map = specialized ? &info : NULL;
            struct ps5vk_compiled_program program = {0}; uint32_t *code = NULL;
            VkResult compiler = ps5vk_runtime_compile_compute(expr, expression_count, "main", layout, map, &program, &code);
            assert(compiler == VK_SUCCESS && code && program.local_size[0] == (specialized ? 32 : 64)); free(code);
            VkPipeline candidate = NULL;
            VkResult front = build_specialized(device, layout, expr, expression_count * 4, map, &candidate);
            assert(front == VK_SUCCESS && candidate && candidate->program.local_size[0] == (specialized ? 32u : 64u)); vkDestroyPipeline(device, candidate, NULL);
        }
        for (size_t i = 5; i < expression_count; i += expr[i] >> 16) {
            if ((expr[i] & 0xffff) == 52 && expr[i + 3] == 81) {
                unsigned length = expr[i] >> 16;
                for (unsigned index = 5; index < length; ++index) {
                    uint32_t old = expr[i + index]; expr[i + index] = UINT32_MAX;
                    VkPipeline invalid = NULL;
                    assert(build(device, layout, expr, expression_count * 4, &invalid) != VK_SUCCESS && !invalid);
                    expr[i + index] = old;
                }
            }
            /* The unselected second structure member must be well formed. */
            if (form == 0 && (expr[i] & 0xffff) == 51 && expr[i] >> 16 == 5) {
                uint32_t old = expr[i + 4]; expr[i + 4] = expr[3];
                VkPipeline invalid = NULL;
                assert(build(device, layout, expr, expression_count * 4, &invalid) != VK_SUCCESS && !invalid);
                expr[i + 4] = old;
            }
            if ((expr[i] & 0xffff) == 28) {
                uint32_t old = expr[i + 3]; expr[i + 3] = id64;
                VkPipeline invalid = NULL;
                assert(build(device, layout, expr, expression_count * 4, &invalid) != VK_SUCCESS && !invalid);
                expr[i + 3] = old;
            }
        }
        free(expr); free(seed);
    }
    for (unsigned form = 0; form < 3; ++form) {
        uint32_t *seed = specialize_dimension(by_id, count, id64, 7);
        size_t expression_count;
        uint32_t *expr = aggregate_dimension(seed, count + 4, id64, id1, form, &expression_count);
        uint32_t aggregate = 0, extracted = 0;
        for (size_t i = 5; i < expression_count; i += expr[i] >> 16)
            if ((expr[i] & 0xffff) == 52 && expr[i + 3] == 81) {
                aggregate = expr[i + 4]; extracted = expr[i + 2];
            }
        assert(aggregate && extracted);
        for (size_t i = 5; i < expression_count;) {
            unsigned length = expr[i] >> 16;
            if ((expr[i] & 0xffff) == 51 && expr[i + 2] == aggregate) {
                expr[i] = 3u << 16 | 46u;
                for (unsigned j = 3; j < length; ++j) expr[i + j] = 1u << 16;
            }
            i += length;
        }
        uint32_t *wrapped = expression_dimension(expr, expression_count, extracted, id64, 128);
        for (unsigned specialized = 0; specialized < 2; ++specialized) {
            uint32_t value = 32;
            VkSpecializationMapEntry entry = {7, 0, 4};
            VkSpecializationInfo info = {1, &entry, 4, &value};
            const VkSpecializationInfo *map = specialized ? &info : NULL;
            struct ps5vk_compiled_program program = {0}; uint32_t *code = NULL;
            assert(ps5vk_runtime_compile_compute(wrapped, expression_count + 6, "main", layout, map, &program, &code) == VK_SUCCESS);
            assert(code && program.local_size[0] == (specialized ? 32 : 64)); free(code);
            VkPipeline candidate = NULL;
            VkResult front = build_specialized(device, layout, wrapped, (expression_count + 6) * 4, map, &candidate);
            assert(front == VK_SUCCESS && candidate && candidate->program.local_size[0] == (specialized ? 32u : 64u));
            vkDestroyPipeline(device, candidate, NULL);
        }
        free(wrapped); free(expr); free(seed);
    }
    for (unsigned form = 0; form < 3; ++form) {
        for (unsigned untouched = 0; untouched < (form == 0 ? 2u : 1u); ++untouched) {
            uint32_t *seed = specialize_dimension(by_id, count, id64, 7);
            size_t expression_count;
            uint32_t *expr = aggregate_dimension(seed, count + 4, id64, id1, form, &expression_count);
            size_t extract_at = 0; uint32_t root = 0, root_type = 0;
            for (size_t i = 5; i < expression_count; i += expr[i] >> 16)
                if ((expr[i] & 0xffff) == 52 && expr[i + 3] == 81) { extract_at = i; root = expr[i + 4]; }
            for (size_t i = 5; i < expression_count; i += expr[i] >> 16)
                if ((expr[i] & 0xffff) == 51 && expr[i + 2] == root) root_type = expr[i + 1];
            assert(extract_at && root && root_type);
            unsigned length = form == 2 ? 8 : 7;
            uint32_t inserted = expr[3];
            uint32_t op[] = {length << 16 | 52u, root_type, inserted, 82, id1, root, untouched, 0};
            expr = realloc(expr, (expression_count + length) * 4); assert(expr);
            memmove(expr + extract_at + length, expr + extract_at, (expression_count - extract_at) * 4);
            memcpy(expr + extract_at, op, length * 4);
            expr[extract_at + length + 4] = inserted; expr[3]++;
            expression_count += length;
            for (unsigned specialized = 0; specialized < 2; ++specialized) {
                uint32_t value = 32, expected = untouched ? (specialized ? 32 : 64) : 1;
                VkSpecializationMapEntry entry = {7, 0, 4};
                VkSpecializationInfo info = {1, &entry, 4, &value};
                const VkSpecializationInfo *map = specialized ? &info : NULL;
                struct ps5vk_compiled_program program = {0}; uint32_t *code = NULL;
                assert(ps5vk_runtime_compile_compute(expr, expression_count, "main", layout, map, &program, &code) == VK_SUCCESS);
                assert(code && program.local_size[0] == expected); free(code);
                VkPipeline candidate = NULL;
                VkResult front = build_specialized(device, layout, expr, expression_count * 4, map, &candidate);
                assert(front == VK_SUCCESS && candidate && candidate->program.local_size[0] == expected);
                vkDestroyPipeline(device, candidate, NULL);
            }
            free(expr); free(seed);
        }
    }
    for (unsigned form = 0; form < 2; ++form) {
        for (unsigned mode = 0; mode < 5; ++mode) {
            uint32_t *seed = specialize_dimension(by_id, count, id64, 7);
            size_t expression_count;
            uint32_t *expr = aggregate_vector_dimension(seed, count + 4, id64, id1, form, mode, &expression_count);
            for (unsigned specialized = 0; specialized < 2; ++specialized) {
                uint32_t value = 32, expected = mode == 1 ? 1 : (mode == 2 || mode == 3) ? 2 : (specialized ? 33 : 65);
                VkSpecializationMapEntry entry = {7, 0, 4};
                VkSpecializationInfo info = {1, &entry, 4, &value};
                const VkSpecializationInfo *map = specialized ? &info : NULL;
                struct ps5vk_compiled_program program = {0}; uint32_t *code = NULL;
                assert(ps5vk_runtime_compile_compute(expr, expression_count, "main", layout, map, &program, &code) == VK_SUCCESS);
                assert(code && program.local_size[0] == expected); free(code);
                VkPipeline candidate = NULL;
                VkResult front = build_specialized(device, layout, expr, expression_count * 4, map, &candidate);
                assert(front == VK_SUCCESS && candidate && candidate->program.local_size[0] == expected);
                vkDestroyPipeline(device, candidate, NULL);
            }
            for (size_t i = 5; i < expression_count; i += expr[i] >> 16) {
                unsigned op = expr[i] & 0xffff, length = expr[i] >> 16;
                if (op == 52 && (expr[i + 3] == 81 || expr[i + 3] == 82)) {
                    unsigned first_index = expr[i + 3] == 81 ? 5 : 6;
                    for (unsigned index = first_index; index < length; ++index) {
                        uint32_t old = expr[i + index]; expr[i + index] = UINT32_MAX;
                        VkPipeline invalid = NULL;
                        assert(build(device, layout, expr, expression_count * 4, &invalid) != VK_SUCCESS && !invalid);
                        expr[i + index] = old;
                    }
                    unsigned operand = expr[i + 3] == 81 ? 4 : 5;
                    uint32_t old = expr[i + operand]; expr[i + operand] = expr[i + 2];
                    VkPipeline invalid = NULL;
                    assert(build(device, layout, expr, expression_count * 4, &invalid) != VK_SUCCESS && !invalid);
                    expr[i + operand] = old;
                }
                if (op == 46 || op == 51 || (op == 52 && expr[i + 3] == 82)) {
                    /* Exercise the consumed aggregate, not standalone constants
                     * that are unreachable from the launch expression. */
                    unsigned aggregate_type = 0;
                    for (size_t t = 5; t < expression_count; t += expr[t] >> 16)
                        if (((expr[t] & 0xffff) == 28 || (expr[t] & 0xffff) == 30) && expr[t + 1] == expr[i + 1])
                            aggregate_type = 1;
                    if (!aggregate_type) continue;
                    size_t types = 5;
                    while (types < expression_count && (expr[types] & 0xffff) != 19) types += expr[types] >> 16;
                    assert(types < expression_count);
                    uint32_t *decorated = malloc((expression_count + 4) * 4); assert(decorated);
                    memcpy(decorated, expr, types * 4);
                    uint32_t decoration[] = {4u << 16 | 71u, expr[i + 2], 1, 23};
                    memcpy(decorated + types, decoration, sizeof(decoration));
                    memcpy(decorated + types + 4, expr + types, (expression_count - types) * 4);
                    VkPipeline invalid = NULL;
                    assert(build(device, layout, decorated, (expression_count + 4) * 4, &invalid) != VK_SUCCESS && !invalid);
                    free(decorated);
                }
            }
            free(expr); free(seed);
        }
    }
    for (unsigned quantize_mode = 0; quantize_mode < 2; ++quantize_mode)
    for (unsigned specialized_float = 0; specialized_float < (quantize_mode ? 6u : 4u); ++specialized_float) {
        uint32_t *seed = specialize_dimension(by_id, count, id64, 7);
        size_t expression_count;
        uint32_t *expr = aggregate_dimension(seed, count + 4, id64, id1, 0, &expression_count);
        size_t type_at = 0; uint32_t aggregate_type = 0, float_type = expr[3], float_value = expr[3] + 1;
        for (size_t i = 5; i < expression_count; i += expr[i] >> 16)
            if ((expr[i] & 0xffff) == 30 && expr[i] >> 16 == 4) { type_at = i; aggregate_type = expr[i + 1]; }
        assert(type_at && aggregate_type);
        unsigned vector_field = specialized_float >= 2;
        uint32_t vector_type = expr[3] + 2, vector_value = expr[3] + 3;
        expr[type_at + 3] = vector_field ? vector_type : float_type;
        for (size_t i = 5; i < expression_count; i += expr[i] >> 16)
            if ((expr[i] & 0xffff) == 51 && expr[i + 1] == aggregate_type) expr[i + 4] = vector_field ? vector_value : float_value;
        uint32_t declarations[] = {3u << 16 | 22u, float_type, 32,
            4u << 16 | 43u, float_type, float_value, 0x3f800000u,
            4u << 16 | 23u, vector_type, float_type, 2,
            5u << 16 | 51u, vector_type, vector_value, float_value, float_value};
        unsigned added = vector_field ? 16 : 7;
        expr = realloc(expr, (expression_count + added) * 4); assert(expr);
        memmove(expr + type_at + added, expr + type_at, (expression_count - type_at) * 4);
        memcpy(expr + type_at, declarations, added * 4); expr[3] += vector_field ? 4 : 2; expression_count += added;
        if (quantize_mode) {
            uint32_t quantized = expr[3]++;
            unsigned quantized_vector = specialized_float >= 4;
            size_t quantize_at = type_at + (quantized_vector ? added : 7);
            expr = realloc(expr, (expression_count + 5) * 4); assert(expr);
            memmove(expr + quantize_at + 5, expr + quantize_at, (expression_count - quantize_at) * 4);
            uint32_t quantize[] = {5u << 16 | 52u, quantized_vector ? vector_type : float_type, quantized, 116, quantized_vector ? vector_value : float_value};
            memcpy(expr + quantize_at, quantize, sizeof(quantize)); expression_count += 5;
            for (size_t i = 5; i < expression_count; i += expr[i] >> 16)
                if ((expr[i] & 0xffff) == 51)
                    for (unsigned child = 3; child < expr[i] >> 16; ++child)
                        if (expr[i + child] == (quantized_vector ? vector_value : float_value)) expr[i + child] = quantized;
        }
        if (specialized_float & 1) {
            uint32_t *with_spec = specialize_dimension(expr, expression_count, float_value, 9);
            free(expr); expr = with_spec; expression_count += 4;
        }
        for (unsigned specialized = 0; specialized < 2; ++specialized) {
            uint32_t data[] = {32, 0x40000000u};
            VkSpecializationMapEntry entries[] = {{7, 0, 4}, {9, 4, 4}};
            VkSpecializationInfo info = {2, entries, sizeof(data), data};
            const VkSpecializationInfo *map = specialized ? &info : NULL;
            struct ps5vk_compiled_program program = {0}; uint32_t *code = NULL;
            VkResult compiler = ps5vk_runtime_compile_compute(expr, expression_count, "main", layout, map, &program, &code);
            assert(compiler == VK_SUCCESS && code && program.local_size[0] == (specialized ? 32u : 64u)); free(code);
            VkPipeline candidate = NULL;
            VkResult front = build_specialized(device, layout, expr, expression_count * 4, map, &candidate);
            assert(front == VK_SUCCESS && candidate && candidate->program.local_size[0] == (specialized ? 32u : 64u));
            vkDestroyPipeline(device, candidate, NULL);
        }
        for (size_t i = 5; i < expression_count; i += expr[i] >> 16) {
            if ((expr[i] & 0xffff) != 52 || expr[i + 3] != 116) continue;
            uint32_t old = expr[i + 4];
            const uint32_t bad_operands[] = {id1, expr[i + 2], expr[3]};
            for (unsigned bad = 0; bad < 3; ++bad) {
                expr[i + 4] = bad_operands[bad];
                VkPipeline invalid = NULL;
                assert(build(device, layout, expr, expression_count * 4, &invalid) != VK_SUCCESS && !invalid);
            }
            expr[i + 4] = old;
            uint32_t instruction = expr[i];
            expr[i] = 4u << 16 | 52u; expr[i + 4] = 1u << 16;
            VkPipeline invalid = NULL;
            assert(build(device, layout, expr, expression_count * 4, &invalid) != VK_SUCCESS && !invalid);
            expr[i] = instruction; expr[i + 4] = old;
            size_t types = 5;
            while (types < expression_count && (expr[types] & 0xffff) != 19) types += expr[types] >> 16;
            assert(types < expression_count);
            uint32_t *decorated = malloc((expression_count + 4) * 4); assert(decorated);
            memcpy(decorated, expr, types * 4);
            uint32_t decoration[] = {4u << 16 | 71u, expr[i + 2], 1, 23};
            memcpy(decorated + types, decoration, sizeof(decoration));
            memcpy(decorated + types + 4, expr + types, (expression_count - types) * 4);
            assert(build(device, layout, decorated, (expression_count + 4) * 4, &invalid) != VK_SUCCESS && !invalid);
            free(decorated);
        }
        free(expr); free(seed);
    }
    for (unsigned mode = 0; mode < 4; ++mode) {
        uint32_t *seed = specialize_dimension(by_id, count, id64, 7);
        size_t expression_count;
        uint32_t *expr = aggregate_dimension(seed, count + 4, id64, id1, 2, &expression_count);
        size_t extract_at = 0; uint32_t root = 0, array_type = 0, root_type = 0, scalar = 0;
        for (size_t i = 5; i < expression_count; i += expr[i] >> 16) {
            if ((expr[i] & 0xffff) == 28) array_type = expr[i + 1];
            if ((expr[i] & 0xffff) == 52 && expr[i + 3] == 81) {
                extract_at = i; root = expr[i + 4]; scalar = expr[i + 2];
            }
        }
        for (size_t i = 5; i < expression_count;) {
            unsigned length = expr[i] >> 16;
            if ((expr[i] & 0xffff) == 51 && expr[i + 2] == root) {
                root_type = expr[i + 1];
                if (mode == 3) {
                    expr[i] = 3u << 16 | 46u;
                    for (unsigned j = 3; j < length; ++j) expr[i + j] = 1u << 16;
                }
            }
            i += length;
        }
        assert(extract_at && root && array_type && root_type && scalar);
        uint32_t instructions[24], next = expr[3]; unsigned n = 0;
#define WORD(v) do { assert(n < 24); instructions[n++] = (v); } while (0)
        if (mode == 2) {
            uint32_t inserted = next++;
            WORD(8u << 16 | 52u); WORD(root_type); WORD(inserted); WORD(82); WORD(id1); WORD(root); WORD(0); WORD(0);
            root = inserted;
        }
        uint32_t intermediate = next++;
        WORD(6u << 16 | 52u); WORD(array_type); WORD(intermediate); WORD(81); WORD(root); WORD(0);
        if (mode == 1) {
            uint32_t inserted = next++;
            WORD(7u << 16 | 52u); WORD(array_type); WORD(inserted); WORD(82); WORD(id1); WORD(intermediate); WORD(0);
            intermediate = inserted;
        }
#undef WORD
        expr = realloc(expr, (expression_count + n) * 4); assert(expr);
        memmove(expr + extract_at + n, expr + extract_at, (expression_count - extract_at) * 4);
        memcpy(expr + extract_at, instructions, n * 4);
        expr[extract_at + n] = 6u << 16 | 52u;
        expr[extract_at + n + 4] = intermediate; expr[extract_at + n + 6] = 1u << 16;
        expr[3] = next; expression_count += n;
        uint32_t *wrapped = expression_dimension(expr, expression_count, scalar, id1, 128);
        for (unsigned specialized = 0; specialized < 2; ++specialized) {
            uint32_t value = 32, expected = mode == 0 ? (specialized ? 33 : 65) : mode == 3 ? 1 : 2;
            VkSpecializationMapEntry entry = {7, 0, 4};
            VkSpecializationInfo info = {1, &entry, 4, &value};
            const VkSpecializationInfo *map = specialized ? &info : NULL;
            struct ps5vk_compiled_program program = {0}; uint32_t *code = NULL;
            assert(ps5vk_runtime_compile_compute(wrapped, expression_count + 6, "main", layout, map, &program, &code) == VK_SUCCESS);
            assert(code && program.local_size[0] == expected); free(code);
            VkPipeline candidate = NULL;
            VkResult front = build_specialized(device, layout, wrapped, (expression_count + 6) * 4, map, &candidate);
            assert(front == VK_SUCCESS && candidate && candidate->program.local_size[0] == expected);
            vkDestroyPipeline(device, candidate, NULL);
        }
        for (size_t i = 5; i < expression_count + 6; i += wrapped[i] >> 16) {
            if ((wrapped[i] & 0xffff) != 52 || (wrapped[i + 3] != 81 && wrapped[i + 3] != 82)) continue;
            unsigned first = wrapped[i + 3] == 81 ? 5 : 6, length = wrapped[i] >> 16;
            for (unsigned index = first; index < length; ++index) {
                uint32_t old = wrapped[i + index]; wrapped[i + index] = UINT32_MAX;
                VkPipeline invalid = NULL;
                assert(build(device, layout, wrapped, (expression_count + 6) * 4, &invalid) != VK_SUCCESS && !invalid);
                wrapped[i + index] = old;
            }
            unsigned operand = wrapped[i + 3] == 81 ? 4 : 5;
            uint32_t old = wrapped[i + operand]; wrapped[i + operand] = wrapped[i + 2];
            VkPipeline invalid = NULL;
            assert(build(device, layout, wrapped, (expression_count + 6) * 4, &invalid) != VK_SUCCESS && !invalid);
            wrapped[i + operand] = old;
        }
        free(wrapped); free(expr); free(seed);
    }
    /* Every lane uses the scalar operation rules; specialization must reach
     * vector operands before the launch dimensions are resolved. */
    const struct { unsigned op; uint32_t normal, specialized; } vector_ops[] = {
        {128,65,33}, {130,63,31}, {132,64,32}, {134,64,32}, {135,64,32},
        {194,32,16}, {195,32,16}, {196,128,64}, {197,65,33}, {198,65,33},
    };
    for (unsigned op_index = 0; op_index < sizeof(vector_ops)/sizeof(vector_ops[0]); ++op_index) {
        uint32_t *seed = specialize_dimension(by_id, count, id64, 7);
        uint32_t *expr = vector_dimension(seed, count + 4, id64, id1, 2, 0);
        size_t operation = 0;
        for (size_t i = 5; i < count + 38;) {
            unsigned n = expr[i] >> 16;
            if ((expr[i] & 0xffff) == 52 && expr[i + 3] == 79) {
                operation = i;
                expr[i] = 6u << 16 | 52u; expr[i + 3] = vector_ops[op_index].op;
                for (unsigned k = 6; k < n; ++k) expr[i + k] = 1u << 16;
            }
            i += n;
        }
        assert(operation);
        /* Shift operands at lane 2 would otherwise contain 64 or 32, which
         * are undefined even though the launch extracts lane zero. */
        if (vector_ops[op_index].op >= 194 && vector_ops[op_index].op <= 196) {
            for (size_t i = 5; i < count + 38; i += expr[i] >> 16)
                if ((expr[i] & 0xffff) == 51 && expr[i] >> 16 == 7)
                    expr[i + 5] = id1;
        }
        for (unsigned specialized = 0; specialized < 2; ++specialized) {
            uint32_t value = 32;
            VkSpecializationMapEntry entry = {7, 0, 4};
            VkSpecializationInfo info = {1, &entry, 4, &value};
            const VkSpecializationInfo *map = specialized ? &info : NULL;
            uint32_t expected = specialized ? vector_ops[op_index].specialized : vector_ops[op_index].normal;
            VkPipeline candidate = NULL;
            assert(build_specialized(device, layout, expr, (count + 38) * 4, map, &candidate) == VK_SUCCESS);
            assert(candidate && candidate->program.local_size[0] == expected);
            vkDestroyPipeline(device, candidate, NULL);
        }
        /* A cyclic vector operand must stop at the shared recursion bound. */
        uint32_t original = expr[operation + 4];
        expr[operation + 4] = expr[operation + 2];
        VkPipeline invalid = NULL;
        assert(build(device, layout, expr, (count + 38) * 4, &invalid) != VK_SUCCESS && !invalid);
        expr[operation + 4] = original;
        free(expr); free(seed);
    }

    const unsigned vector_extra_ops[] = {126, 137, 138, 139, 199, 200};
    for (unsigned op_index = 0; op_index < 6; ++op_index) {
        unsigned op = vector_extra_ops[op_index];
        int unary = op == 126 || op == 200;
        uint32_t *seed = specialize_dimension(by_id, count, id64, 7);
        uint32_t *expr = vector_dimension(seed, count + 4, id64, id1, 2, 0);
        for (size_t i = 5; i < count + 38;) {
            unsigned n = expr[i] >> 16;
            if ((expr[i] & 0xffff) == 52 && expr[i + 3] == 79) {
                unsigned length = unary ? 5 : 6;
                expr[i] = length << 16 | 52u; expr[i + 3] = op;
                for (unsigned j = length; j < n; ++j) expr[i + j] = 1u << 16;
            }
            i += n;
        }
        /* Undo unary operations, or add one to a zero-valued remainder/AND,
         * so the resulting local dimension remains valid. */
        uint32_t *wrapped = expression_dimension(expr, count + 38, expr[3] - 1, id1, unary ? op : 128);
        for (unsigned input = 0; input < 2; ++input) {
            uint32_t value = input ? 32 : 64;
            VkSpecializationMapEntry entry = {7, 0, 4};
            VkSpecializationInfo info = {1, &entry, 4, &value};
            VkPipeline selected = NULL;
            assert(build_specialized(device, layout, wrapped, (count + 44) * 4, &info, &selected) == VK_SUCCESS);
            assert(selected && selected->program.local_size[0] == (unary ? value : 1));
            vkDestroyPipeline(device, selected, NULL);
        }
        free(wrapped); free(expr); free(seed);
    }
    /* Undefined arithmetic in a lane that is not extracted still invalidates
     * the vector expression, without sending malformed input to the compiler. */
    const unsigned vector_invalid_ops[] = {134, 135, 137, 138, 139, 194, 195, 196};
    uint32_t zero_id = constant_id(literal, count, 0); assert(zero_id);
    for (unsigned op_index = 0; op_index < 8; ++op_index) {
        unsigned op = vector_invalid_ops[op_index];
        uint32_t *seed = specialize_dimension(by_id, count, id64, 7);
        uint32_t *expr = vector_dimension(seed, count + 4, id64, id1, 2, 0);
        for (size_t i = 5; i < count + 38;) {
            unsigned n = expr[i] >> 16;
            if ((expr[i] & 0xffff) == 52 && expr[i + 3] == 79) {
                expr[i] = 6u << 16 | 52u; expr[i + 3] = op;
                for (unsigned j = 6; j < n; ++j) expr[i + j] = 1u << 16;
            }
            if (op < 194 && (expr[i] & 0xffff) == 51 && n == 7) expr[i + 4] = zero_id;
            i += n;
        }
        VkPipeline invalid = NULL;
        assert(build(device, layout, expr, (count + 38) * 4, &invalid) != VK_SUCCESS && !invalid);
        free(expr); free(seed);
    }
    for (unsigned comparison = 170; comparison <= 179; ++comparison) {
        for (unsigned scalar = 0; scalar < 2; ++scalar) {
            uint32_t *seed = specialize_dimension(by_id, count, id64, 7);
            size_t expression_count;
            uint32_t *expr = comparison_vector_dimension(seed, count + 4, id64, id1,
                comparison, scalar, &expression_count);
            const uint32_t inputs[] = {0, 1, 32, UINT32_MAX};
            for (unsigned input = 0; input < 4; ++input) {
                uint32_t value = inputs[input];
                int64_t signed_value = value <= INT32_MAX ? value : (int64_t)value - 0x100000000LL;
                const unsigned conditions[] = {value == 1, value != 1, value > 1, signed_value > 1,
                    value >= 1, signed_value >= 1, value < 1, signed_value < 1, value <= 1, signed_value <= 1};
                VkSpecializationMapEntry entry = {7, 0, 4};
                VkSpecializationInfo info = {1, &entry, 4, &value};
                VkPipeline selected = NULL;
                assert(build_specialized(device, layout, expr, expression_count * 4, &info, &selected) == VK_SUCCESS);
                assert(selected && selected->program.local_size[0] == (conditions[comparison - 170] ? 32 : 1));
                vkDestroyPipeline(device, selected, NULL);
            }
            /* Vector operations cannot consume a scalar integer operand. */
            for (size_t i = 5; i < expression_count; i += expr[i] >> 16) {
                if ((expr[i] & 0xffff) == 52 && expr[i + 3] == comparison) {
                    uint32_t operand = expr[i + 4]; expr[i + 4] = id1;
                    VkPipeline invalid = NULL;
                    assert(build(device, layout, expr, expression_count * 4, &invalid) != VK_SUCCESS && !invalid);
                    expr[i + 4] = operand;
                }
            }
            free(expr); free(seed);
        }
    }
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
    const unsigned boolean_vector_ops[] = {164, 165, 166, 167, 168};
    for (unsigned op_index = 0; op_index < 5; ++op_index) {
        unsigned op = boolean_vector_ops[op_index];
        uint32_t *vector = boolean_vector_dimension(boolean_spec, count + 33,
            specialized[3] + 2, specialized[3] + 3, 2, 0);
        for (size_t i = 5; i < count + 74;) {
            unsigned n = vector[i] >> 16;
            if ((vector[i] & 0xffff) == 52 && vector[i + 3] == 79) {
                unsigned length = op == 168 ? 5 : 6;
                vector[i] = length << 16 | 52u; vector[i + 3] = op;
                for (unsigned j = length; j < n; ++j) vector[i + j] = 1u << 16;
            }
            i += n;
        }
        for (unsigned input = 0; input < 2; ++input) {
            truth = input ? 2 : VK_FALSE;
            /* Second vector lane zero is false; first is specialized. */
            unsigned expected = op == 164 || op == 168 ? !input : op == 167 ? 0 : input;
            VkPipeline selected = NULL;
            assert(build_specialized(device, layout, vector, (count + 74) * 4,
                                     &truth_info, &selected) == VK_SUCCESS);
            assert(selected && selected->program.local_size[0] == (expected ? 32 : 1));
            vkDestroyPipeline(device, selected, NULL);
        }
        free(vector);
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
    puts("LocalSizeId: scalar, vector and aggregate specialization expressions under maintenance4 "
         "(host compiler, no GPU evidence)");
    return 0;
}
