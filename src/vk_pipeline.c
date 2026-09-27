#include "vk_pipeline.h"
#include "compilation_cache.h"
#include "vk_pipeline_cache.h"
#include "spirv_ubo_layout.h"
#include "descriptor_table_layout.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define INVALID VK_ERROR_UNKNOWN
#if defined(PS5VK_TARGET_PS5) && PS5VK_TARGET_PS5
#include "ps5log.h"
#define COMPUTE_MARK(...) ps5log_printf(PS5LOG_MARK, __VA_ARGS__)
#else
#define COMPUTE_MARK(...) ((void)0)
#endif
static int module_valid(const uint32_t *words, size_t count)
{
    if (!words || count < 5 || words[0] != 0x07230203 || !words[3] || words[4]) return 0;
    for (size_t i = 5; i < count;) {
        size_t n = words[i] >> 16;
        if (!n || n > count - i) return 0;
        i += n;
    }
    return 1;
}
/* The shipping Vulkan 1.0 profile reports no subgroup stages or operations.
 * Private compute-only builds admit only independently selected Broadcast and
 * IAdd diagnostics. Each operation needs its own SPIR-V capability; no public
 * subgroup operation or feature follows from either internal switch. */
static int subgroup_module_unsupported(const uint32_t *words, size_t count,
                                       uint32_t platform_features,
                                       uint32_t platform_features_t09)
{
    /* A private build may also admit BASIC alone: OpGroupNonUniformElect,
     * subgroup-scope barriers and the subgroup built-ins, compute only. */
    const int basic_compute =
        !!(platform_features_t09 & PS5VK_T09_FEATURE_SUBGROUP_BASIC_COMPUTE);
    const int broadcast_compute =
        !!(platform_features & PS5VK_FEATURE_SUBGROUP_BROADCAST_COMPUTE);
    const int iadd_compute =
        !!(platform_features & PS5VK_FEATURE_SUBGROUP_IADD_COMPUTE);
    int basic = 0, ballot = 0, arithmetic = 0, broadcast = 0, iadd = 0;
    int compute_entry = 0;
    int other_entry = 0, subgroup = 0;
    for (size_t at = 5; at < count; at += words[at] >> 16) {
        uint32_t opcode = words[at] & 0xffffu;
        uint32_t length = words[at] >> 16;
        if (opcode == 17u && length == 2) {
            uint32_t capability = words[at + 1];
            if ((capability >= 61u && capability <= 68u) ||
                capability == 4423u || capability == 4431u ||
                capability == 5297u || capability == 6026u) {
                subgroup = 1;
                if ((capability != 61u && capability != 63u && capability != 64u) ||
                    (capability == 63u && !iadd_compute) ||
                    (capability == 64u && !broadcast_compute)) return 1;
                if (capability == 61u) basic = 1;
                if (capability == 63u) arithmetic = 1;
                if (capability == 64u) ballot = 1;
            }
        }
        if (opcode == 15u && length >= 4u) {
            if (words[at + 1] == 5u) compute_entry = 1;
            else other_entry = 1;
        }
        if ((opcode >= 333u && opcode <= 366u) ||
            opcode == 4431u || opcode == 5110u || opcode == 5111u ||
            opcode == 5296u) {
            subgroup = 1;
            if (opcode == 333u && basic_compute) continue;
            if (opcode == 337u && broadcast_compute) broadcast = 1;
            else if (opcode == 349u && iadd_compute) iadd = 1;
            else return 1;
        }
    }
    return subgroup && (!basic || !compute_entry || other_entry ||
                        (!broadcast && !iadd && !(basic_compute && !ballot && !arithmetic)) ||
                        (broadcast != ballot) ||
                        (iadd != arithmetic));
}
static int declares_int16_capability(const uint32_t *words, size_t count)
{
    for (size_t at = 5; at < count; at += words[at] >> 16)
        if ((words[at] & 0xffffu) == 17u &&
            (words[at] >> 16) == 2u && words[at + 1] == 22u)
            return 1;
    return 0;
}
VkBool32 ps5vk_shader_entry(VkShaderModule module, VkShaderStageFlagBits stage,
                            const char *name, uint32_t *out)
{
    if (!out) return VK_FALSE;
    *out = 0;
    if (!module || !name || !module_valid(module->words, module->word_count)) return VK_FALSE;
    uint32_t model;
    switch (stage) {
    case VK_SHADER_STAGE_VERTEX_BIT: model = 0; break;
    case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: model = 1; break;
    case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: model = 2; break;
    case VK_SHADER_STAGE_GEOMETRY_BIT: model = 3; break;
    case VK_SHADER_STAGE_FRAGMENT_BIT: model = 4; break;
    case VK_SHADER_STAGE_COMPUTE_BIT: model = 5; break;
    default: return VK_FALSE;
    }
    uint32_t id = 0; unsigned found = 0;
    for (size_t i = 5; i < module->word_count; i += module->words[i] >> 16) {
        const uint32_t *w = module->words + i;
        size_t n = w[0] >> 16;
        if ((w[0] & 0xffff) != 15) continue;
        if (n < 4 || !w[2] || w[2] >= module->words[3]) return VK_FALSE;
        const char *entry = (const char *)(w + 3);
        const char *end = memchr(entry, 0, (n - 3) * 4);
        if (!end) return VK_FALSE;
        if (w[1] == model && (size_t)(end - entry) == strlen(name) && !memcmp(entry, name, end - entry)) {
            id = w[2]; ++found;
        }
    }
    if (found != 1) return VK_FALSE;
    *out = id; return VK_TRUE;
}
/* Constant expressions used for launch dimensions share one recursion/visit
 * budget, including vector operands. Never let an unused lane hide a cycle. */
static int constant_value(VkShaderModule module, uint32_t id, uint32_t *value,
                          const VkSpecializationInfo *specialization,
                          unsigned depth, unsigned *budget, unsigned *kind);
static const uint32_t *constant_definition(VkShaderModule module, uint32_t id)
{
    if (!id || id >= module->words[3]) return NULL;
    const uint32_t *definition = NULL;
    for (size_t i = 5; i < module->word_count; i += module->words[i] >> 16) {
        const uint32_t *w = module->words + i;
        unsigned op = w[0] & 0xffff, length = w[0] >> 16;
        if (((op >= 41 && op <= 46) || (op >= 48 && op <= 52)) &&
            length >= 3 && w[2] == id) {
            if (definition) return NULL;
            definition = w;
        }
    }
    return definition;
}
static unsigned constant_scalar_kind(VkShaderModule module, uint32_t type)
{
    unsigned count = 0, kind = 0;
    for (size_t i = 5; i < module->word_count; i += module->words[i] >> 16) {
        const uint32_t *w = module->words + i;
        unsigned op = w[0] & 0xffff, length = w[0] >> 16;
        if (op == 21 && length == 4 && w[1] == type && w[2] == 32) {
            ++count; kind = 1;
        }
        if (op == 20 && length == 2 && w[1] == type) {
            ++count; kind = 2;
        }
    }
    return count == 1 ? kind : 0;
}
/* Shared scalar/component rules use modular unsigned arithmetic and wider
 * signed intermediates, avoiding undefined host overflow, division or shifts. */
static int constant_operation(unsigned op, unsigned result_kind,
                              uint32_t a, uint32_t b, uint32_t c,
                              unsigned ak, unsigned bk, unsigned ck, uint32_t *value)
{
    int unary = op == 126 || op == 168 || op == 200;
    int select = op == 169, logical = op >= 164 && op <= 168;
    int comparison = op >= 170 && op <= 179;
    if (select) {
        if (ak != 2 || bk != result_kind || ck != result_kind) return 0;
        *value = a ? b : c;
        return 1;
    }
    unsigned operand_kind = logical ? 2u : 1u;
    if (result_kind != (logical || comparison ? 2u : 1u) ||
        ak != operand_kind || (!unary && bk != operand_kind)) return 0;
    int64_t sa = a <= INT32_MAX ? (int64_t)a : (int64_t)a - 0x100000000LL;
    int64_t sb = b <= INT32_MAX ? (int64_t)b : (int64_t)b - 0x100000000LL;
    switch (op) {
    case 126: *value = 0u - a; break;
    case 128: *value = a + b; break;
    case 130: *value = a - b; break;
    case 132: *value = a * b; break;
    case 134: if (!b) return 0; *value = a / b; break;
    case 135: if (!b || (sa == INT32_MIN && sb == -1)) return 0;
              *value = (uint32_t)(sa / sb); break;
    case 137: if (!b) return 0; *value = a % b; break;
    case 138: if (!b) return 0; *value = (uint32_t)(sa % sb); break;
    case 139: {
        if (!b) return 0;
        int64_t r = sa % sb;
        if (r && ((r < 0) != (sb < 0))) r += sb;
        *value = (uint32_t)r; break;
    }
    case 164: *value = !!a == !!b; break;
    case 165: *value = !!a != !!b; break;
    case 166: *value = a || b; break;
    case 167: *value = a && b; break;
    case 168: *value = !a; break;
    case 170: *value = a == b; break;
    case 171: *value = a != b; break;
    case 172: *value = a > b; break;
    case 173: *value = sa > sb; break;
    case 174: *value = a >= b; break;
    case 175: *value = sa >= sb; break;
    case 176: *value = a < b; break;
    case 177: *value = sa < sb; break;
    case 178: *value = a <= b; break;
    case 179: *value = sa <= sb; break;
    case 194: if (b >= 32) return 0; *value = a >> b; break;
    case 195: if (b >= 32) return 0;
              *value = a >> b;
              if (b && (a & 0x80000000u)) *value |= UINT32_MAX << (32 - b);
              break;
    case 196: if (b >= 32) return 0; *value = a << b; break;
    case 197: *value = a | b; break;
    case 198: *value = a ^ b; break;
    case 199: *value = a & b; break;
    case 200: *value = ~a; break;
    default: return 0;
    }
    return 1;
}
static int constant_extract(VkShaderModule module, uint32_t id,
    const uint32_t *indices, unsigned index_count, uint32_t expected, uint32_t *values, unsigned *components,
    const VkSpecializationInfo *specialization, unsigned depth, unsigned *budget);
static int constant_vector(VkShaderModule module, uint32_t id, uint32_t values[4],
                           unsigned *components, uint32_t *element_type,
                           const VkSpecializationInfo *specialization,
                           unsigned depth, unsigned *budget)
{
    if (depth >= 64 || !*budget) return 0;
    --*budget;
    const uint32_t *definition = constant_definition(module, id);
    if (!definition) return 0;
    unsigned types = 0;
    for (size_t i = 5; i < module->word_count; i += module->words[i] >> 16) {
        const uint32_t *w = module->words + i;
        unsigned op = w[0] & 0xffff, length = w[0] >> 16;
        if (op == 71 && length >= 3 && w[1] == id && w[2] == 1) return 0;
        if (op == 23 && length == 4 && w[1] == definition[1]) {
            *element_type = w[2]; *components = w[3]; ++types;
        }
    }
    if (types != 1 || *components < 2 || *components > 4) return 0;
    unsigned op = definition[0] & 0xffff, length = definition[0] >> 16;
    if (op == 46) { /* ConstantNull: integer/boolean vectors only. */
        if (length != 3) return 0;
        unsigned scalar_types = 0;
        for (size_t i = 5; i < module->word_count; i += module->words[i] >> 16) {
            const uint32_t *w = module->words + i;
            unsigned opcode = w[0] & 0xffff, size = w[0] >> 16;
            if ((opcode == 20 && size == 2 && w[1] == *element_type) ||
                (opcode == 21 && size == 4 && w[1] == *element_type && w[2] == 32))
                ++scalar_types;
        }
        if (scalar_types != 1) return 0;
        memset(values, 0, *components * sizeof(*values));
        return 1;
    }
    if (op == 44 || op == 51) {
        if (length != 3 + *components) return 0;
        for (unsigned i = 0; i < *components; ++i) {
            const uint32_t *child = constant_definition(module, definition[3 + i]);
            unsigned kind;
            if (!child || child[1] != *element_type ||
                !constant_value(module, definition[3 + i], &values[i], specialization,
                                depth + 1, budget, &kind)) return 0;
        }
        return 1;
    }
    if (op != 52 || length < 5) return 0;
    if (definition[3] == 81) {
        unsigned extracted;
        return length >= 6 && constant_extract(module, definition[4], definition + 5, length - 5,
            definition[1], values, &extracted, specialization, depth + 1, budget) && extracted == *components;
    }
    uint32_t a[4], b[4], at = 0, bt = 0;
    unsigned an = 0, bn = 0;
    if (definition[3] == 79) { /* VectorShuffle: lane selectors are literals. */
        if (length != 6 + *components ||
            !constant_vector(module, definition[4], a, &an, &at, specialization, depth + 1, budget) ||
            !constant_vector(module, definition[5], b, &bn, &bt, specialization, depth + 1, budget) ||
            at != *element_type || bt != *element_type) return 0;
        for (unsigned i = 0; i < *components; ++i) {
            uint32_t lane = definition[6 + i];
            if (lane >= an + bn) return 0; /* Includes undefined components. */
            values[i] = lane < an ? a[lane] : b[lane - an];
        }
        return 1;
    }
    if (definition[3] == 82) { /* CompositeInsert into a vector. */
        const uint32_t *object = constant_definition(module, definition[4]);
        const uint32_t *vector = length == 7 ? constant_definition(module, definition[5]) : NULL;
        unsigned kind;
        uint32_t value;
        if (!object || !vector || object[1] != *element_type || vector[1] != definition[1] ||
            definition[6] >= *components ||
            !constant_value(module, definition[4], &value, specialization, depth + 1, budget, &kind) ||
            !constant_vector(module, definition[5], values, &an, &at, specialization, depth + 1, budget))
            return 0;
        values[definition[6]] = value;
        return 1;
    }
    /* Apply the same bounded scalar operation to every component. */
    unsigned operation = definition[3];
    unsigned result_kind = constant_scalar_kind(module, *element_type);
    int unary = operation == 126 || operation == 168 || operation == 200;
    int select = operation == 169;
    if (!result_kind || length != (select ? 7u : unary ? 5u : 6u)) return 0;
    uint32_t c[4] = {0}, ct = 0;
    unsigned cn = 0, ak = 0, bk = 0, ck = 0;
    if (!constant_vector(module, definition[4], a, &an, &at, specialization, depth + 1, budget)) {
        /* SPIR-V 1.4 vector Select also permits a scalar boolean condition. */
        if (!select || !constant_value(module, definition[4], &a[0], specialization,
                                       depth + 1, budget, &ak) || ak != 2) return 0;
        for (unsigned i = 1; i < *components; ++i) a[i] = a[0];
        an = *components;
    } else ak = constant_scalar_kind(module, at);
    if (an != *components) return 0;
    if (!unary) {
        if (!constant_vector(module, definition[5], b, &bn, &bt, specialization,
                             depth + 1, budget) || bn != *components) return 0;
        bk = constant_scalar_kind(module, bt);
    } else memset(b, 0, sizeof(b));
    if (select) {
        if (!constant_vector(module, definition[6], c, &cn, &ct, specialization,
                             depth + 1, budget) || cn != *components) return 0;
        ck = constant_scalar_kind(module, ct);
    }
    for (unsigned i = 0; i < *components; ++i)
        if (!constant_operation(operation, result_kind, a[i], b[i], c[i], ak, bk, ck, &values[i]))
            return 0;
    return 1;
}
/* Aggregate expressions retain their declared types through extraction and
 * insertion. Validate every consumed member before selecting a scalar/vector,
 * using the same depth/visit budget as ordinary specialization operations. */
static const uint32_t *constant_type(VkShaderModule module, uint32_t id)
{
    const uint32_t *type = NULL;
    for (size_t i = 5; i < module->word_count; i += module->words[i] >> 16) {
        const uint32_t *w = module->words + i;
        unsigned op = w[0] & 0xffff, length = w[0] >> 16;
        if (op >= 19 && op <= 33 && length >= 2 && w[1] == id) {
            if (type) return NULL;
            type = w;
        }
    }
    return type;
}
static int constant_members(VkShaderModule module, const uint32_t *type,
    uint32_t *count, const VkSpecializationInfo *specialization, unsigned depth, unsigned *budget)
{
    if (!type) return 0;
    unsigned op = type[0] & 0xffff, length = type[0] >> 16;
    if (op == 23 && length == 4 && type[3] >= 2 && type[3] <= 4) { *count = type[3]; return 1; }
    if (op == 30 && length >= 2) { *count = length - 2; return 1; }
    if (op == 28 && length == 4) {
        unsigned kind;
        return constant_value(module, type[3], count, specialization, depth + 1, budget, &kind) &&
               kind == 1 && *count > 0 && *count <= 4096;
    }
    return 0;
}
static int constant_null_type_valid(VkShaderModule module, uint32_t type_id,
    const VkSpecializationInfo *specialization, unsigned depth, unsigned *budget)
{
    if (depth >= 64 || !*budget) return 0;
    --*budget;
    const uint32_t *type = constant_type(module, type_id);
    if (!type) return 0;
    unsigned op = type[0] & 0xffff, length = type[0] >> 16;
    if (op == 20) return length == 2;
    if (op == 21) return length == 4 && (type[2] == 8 || type[2] == 16 || type[2] == 32 || type[2] == 64);
    if (op == 22) return length == 3 && (type[2] == 16 || type[2] == 32 || type[2] == 64);
    if (op == 23) return length == 4 && type[3] >= 2 && type[3] <= 4 &&
        constant_null_type_valid(module, type[2], specialization, depth + 1, budget);
    uint32_t members;
    if (!constant_members(module, type, &members, specialization, depth + 1, budget)) return 0;
    for (uint32_t i = 0; i < members; ++i) {
        uint32_t child = op == 30 ? type[2 + i] : type[2];
        if (!constant_null_type_valid(module, child, specialization, depth + 1, budget)) return 0;
    }
    return 1;
}
static int constant_path_type(VkShaderModule module, uint32_t type_id,
    const uint32_t *indices, unsigned index_count, uint32_t *selected,
    const VkSpecializationInfo *specialization, unsigned depth, unsigned *budget)
{
    if (depth >= 64 || !*budget) return 0;
    --*budget;
    if (!index_count) { *selected = type_id; return constant_type(module, type_id) != NULL; }
    const uint32_t *type = constant_type(module, type_id);
    if (!type) return 0;
    unsigned op = type[0] & 0xffff;
    uint32_t members;
    if (op == 23 && type[0] >> 16 == 4) members = type[3];
    else if (!constant_members(module, type, &members, specialization, depth + 1, budget)) return 0;
    if (indices[0] >= members) return 0;
    uint32_t child = op == 30 ? type[2 + indices[0]] : type[2];
    return constant_path_type(module, child, indices + 1, index_count - 1, selected,
        specialization, depth + 1, budget);
}

/* A mixed aggregate may contain numeric leaves not used as launch dimensions.
 * Validate their literal shape and specialization storage without narrowing them. */
static int constant_unused_literal(VkShaderModule module, const uint32_t *definition,
    const uint32_t *type, const VkSpecializationInfo *specialization)
{
    unsigned type_op = type[0] & 0xffff, type_length = type[0] >> 16;
    if (!((type_op == 21 && type_length == 4) || (type_op == 22 && type_length == 3))) return 0;
    uint32_t width = type[2];
    if (width != 8 && width != 16 && width != 32 && width != 64) return 0;
    if (type_op == 22 && width == 8) return 0;
    unsigned op = definition[0] & 0xffff, length = definition[0] >> 16;
    if (op == 46 ? length != 3 : ((op != 43 && op != 50) || length != 3 + (width + 31) / 32)) return 0;
    unsigned decorated = 0; uint32_t spec_id = 0;
    for (size_t i = 5; i < module->word_count; i += module->words[i] >> 16) {
        const uint32_t *w = module->words + i;
        if ((w[0] & 0xffff) == 71 && w[0] >> 16 >= 3 && w[1] == definition[2] && w[2] == 1) {
            if (w[0] >> 16 != 4 || op != 50 || ++decorated > 1) return 0;
            spec_id = w[3];
        }
    }
    if (!decorated || !specialization) return 1;
    if (specialization->mapEntryCount > PS5VK_MAX_SPECIALIZATION_CONSTANTS ||
        (specialization->mapEntryCount && !specialization->pMapEntries)) return 0;
    unsigned matches = 0;
    for (uint32_t i = 0; i < specialization->mapEntryCount; ++i) {
        const VkSpecializationMapEntry *entry = specialization->pMapEntries + i;
        if (entry->constantID != spec_id) continue;
        if (++matches > 1 || entry->size != width / 8 || !specialization->pData ||
            entry->offset > specialization->dataSize || entry->size > specialization->dataSize - entry->offset)
            return 0;
    }
    return 1;
}

static int constant_tree_valid(VkShaderModule module, uint32_t id, uint32_t expected,
    const VkSpecializationInfo *specialization, unsigned depth, unsigned *budget)
{
    if (depth >= 64 || !*budget) return 0;
    --*budget;
    const uint32_t *definition = constant_definition(module, id);
    if (!definition || definition[1] != expected) return 0;
    unsigned kind = constant_scalar_kind(module, expected);
    if (kind) {
        uint32_t value;
        return constant_value(module, id, &value, specialization, depth + 1, budget, &kind);
    }
    const uint32_t *type = constant_type(module, expected);
    if (!type) return 0;
    /* Quantized float members are checked structurally here; the compiler
     * evaluates them. They cannot themselves become integer launch sizes. */
    if ((definition[0] & 0xffff) == 52 && definition[0] >> 16 == 5 && definition[3] == 116) {
        const uint32_t *element = type;
        if ((type[0] & 0xffff) == 23) {
            if (type[0] >> 16 != 4 || type[3] < 2 || type[3] > 4) return 0;
            element = constant_type(module, type[2]);
        }
        if (!element || (element[0] & 0xffff) != 22 || element[0] >> 16 != 3 || element[2] != 32) return 0;
        for (size_t i = 5; i < module->word_count; i += module->words[i] >> 16) {
            const uint32_t *w = module->words + i;
            if ((w[0] & 0xffff) == 71 && w[0] >> 16 >= 3 && w[1] == id && w[2] == 1) return 0;
        }
        return constant_tree_valid(module, definition[4], expected, specialization, depth + 1, budget);
    }
    if ((type[0] & 0xffff) == 21 || (type[0] & 0xffff) == 22)
        return constant_unused_literal(module, definition, type, specialization);
    if ((type[0] & 0xffff) == 23 && type[0] >> 16 == 4 && constant_scalar_kind(module, type[2])) {
        uint32_t values[4], element; unsigned components;
        return constant_vector(module, id, values, &components, &element, specialization, depth + 1, budget);
    }
    uint32_t members;
    if (!constant_members(module, type, &members, specialization, depth + 1, budget)) return 0;
    unsigned op = definition[0] & 0xffff, length = definition[0] >> 16;
    int insert = op == 52 && length >= 7 && definition[3] == 82;
    int extract = op == 52 && length >= 6 && definition[3] == 81;
    if (!insert && !extract && op != 46 && ((op != 44 && op != 51) || length != 3 + members)) return 0;
    for (size_t i = 5; i < module->word_count; i += module->words[i] >> 16) {
        const uint32_t *w = module->words + i;
        if ((w[0] & 0xffff) == 71 && w[0] >> 16 >= 3 && w[1] == id && w[2] == 1) return 0;
    }
    if (extract) {
        const uint32_t *source = constant_definition(module, definition[4]);
        uint32_t selected;
        return source && constant_path_type(module, source[1], definition + 5, length - 5, &selected,
                   specialization, depth + 1, budget) && selected == expected &&
               constant_tree_valid(module, definition[4], source[1], specialization, depth + 1, budget);
    }
    if (insert) {
        uint32_t selected;
        return constant_path_type(module, expected, definition + 6, length - 6, &selected,
                   specialization, depth + 1, budget) &&
               constant_tree_valid(module, definition[5], expected, specialization, depth + 1, budget) &&
               constant_tree_valid(module, definition[4], selected, specialization, depth + 1, budget);
    }
    if (op == 46) return length == 3 &&
        constant_null_type_valid(module, expected, specialization, depth + 1, budget);
    for (uint32_t i = 0; i < members; ++i) {
        uint32_t child_type = (type[0] & 0xffff) == 30 ? type[2 + i] : type[2];
        if (!constant_tree_valid(module, definition[3 + i], child_type, specialization, depth + 1, budget)) return 0;
    }
    return 1;
}
static int constant_extract(VkShaderModule module, uint32_t id,
    const uint32_t *indices, unsigned index_count, uint32_t expected, uint32_t *values, unsigned *components,
    const VkSpecializationInfo *specialization, unsigned depth, unsigned *budget)
{
    if (depth >= 64 || !*budget) return 0;
    --*budget;
    const uint32_t *definition = constant_definition(module, id);
    if (!definition) return 0;
    if (!index_count) {
        if (definition[1] != expected) return 0;
        unsigned kind = constant_scalar_kind(module, expected);
        if (kind) {
            *components = 1;
            return constant_value(module, id, values, specialization, depth + 1, budget, &kind);
        }
        uint32_t element;
        return constant_vector(module, id, values, components, &element, specialization, depth + 1, budget);
    }
    const uint32_t *type = constant_type(module, definition[1]);
    if (!type) return 0;
    if ((definition[0] & 0xffff) == 52 && (definition[0] >> 16) >= 6 && definition[3] == 81) {
        unsigned prefix = (definition[0] >> 16) - 5;
        uint32_t path[64];
        if (prefix > 64 || index_count > 64 - prefix ||
            !constant_tree_valid(module, id, definition[1], specialization, depth + 1, budget)) return 0;
        memcpy(path, definition + 5, prefix * sizeof(*path));
        memcpy(path + prefix, indices, index_count * sizeof(*path));
        return constant_extract(module, definition[4], path, prefix + index_count, expected,
            values, components, specialization, depth + 1, budget);
    }
    if ((type[0] & 0xffff) == 23) {
        uint32_t lanes[4], element; unsigned count;
        if (index_count != 1 || !constant_vector(module, id, lanes, &count, &element,
            specialization, depth + 1, budget) || element != expected || indices[0] >= count) return 0;
        values[0] = lanes[indices[0]]; *components = 1; return 1;
    }
    if ((definition[0] & 0xffff) == 46) {
        uint32_t selected;
        if (!constant_tree_valid(module, id, definition[1], specialization, depth + 1, budget) ||
            !constant_path_type(module, definition[1], indices, index_count, &selected,
                specialization, depth + 1, budget) || selected != expected) return 0;
        const uint32_t *selected_type = constant_type(module, selected);
        if (constant_scalar_kind(module, selected)) *components = 1;
        else if (selected_type && (selected_type[0] & 0xffff) == 23 && selected_type[0] >> 16 == 4 &&
                 selected_type[3] >= 2 && selected_type[3] <= 4 && constant_scalar_kind(module, selected_type[2]))
            *components = selected_type[3];
        else return 0;
        memset(values, 0, *components * sizeof(*values)); return 1;
    }
    uint32_t members;
    if (!constant_members(module, type, &members, specialization, depth + 1, budget) ||
        indices[0] >= members ||
        !constant_tree_valid(module, id, definition[1], specialization, depth + 1, budget)) return 0;
    if ((definition[0] & 0xffff) == 52 && definition[3] == 82) {
        unsigned inserted_indices = (definition[0] >> 16) - 6;
        unsigned common = index_count < inserted_indices ? index_count : inserted_indices;
        int matching = 1;
        for (unsigned i = 0; i < common; ++i)
            if (indices[i] != definition[6 + i]) matching = 0;
        if (!matching) return constant_extract(module, definition[5], indices, index_count,
            expected, values, components, specialization, depth + 1, budget);
        if (index_count < inserted_indices) {
            /* Extracting a whole vector after insertion into one of its lanes. */
            const uint32_t *selected_type = constant_type(module, expected);
            if (inserted_indices != index_count + 1 || !selected_type ||
                (selected_type[0] & 0xffff) != 23 || selected_type[0] >> 16 != 4 ||
                !constant_extract(module, definition[5], indices, index_count, expected,
                    values, components, specialization, depth + 1, budget)) return 0;
            uint32_t lane = definition[6 + index_count], value;
            unsigned kind;
            const uint32_t *object = constant_definition(module, definition[4]);
            if (lane >= *components || !object || object[1] != selected_type[2] ||
                !constant_value(module, definition[4], &value, specialization, depth + 1, budget, &kind)) return 0;
            values[lane] = value; return 1;
        }
        return constant_extract(module, definition[4], indices + inserted_indices,
            index_count - inserted_indices, expected, values, components, specialization, depth + 1, budget);
    }
    return constant_extract(module, definition[3 + indices[0]], indices + 1, index_count - 1,
        expected, values, components, specialization, depth + 1, budget);
}

/* Resolve a scalar 32-bit integer or boolean, including a specialized workgroup
 * dimension. Keep this in agreement with the compiler's specialization input:
 * defaults apply only when the application did not supply that SpecId. */
static int constant_value(VkShaderModule module, uint32_t id, uint32_t *value,
                          const VkSpecializationInfo *specialization,
                          unsigned depth, unsigned *budget, unsigned *kind)
{
    if (!id || id >= module->words[3] || depth >= 64 || !*budget) return 0;
    --*budget;
    const uint32_t *expression = NULL;
    uint32_t type = 0, spec_id = 0, literal_kind = 0;
    int is_spec = 0, is_null = 0, found = 0, decorated = 0;
    for (size_t i = 5; i < module->word_count; i += module->words[i] >> 16) {
        const uint32_t *w = module->words + i;
        uint32_t op = w[0] & 0xffff;
        if ((op == 43 || op == 50) && w[0] >> 16 == 4 && w[2] == id) {
            type = w[1]; *value = w[3]; is_spec = op == 50; literal_kind = 1; ++found;
        }
        if ((op == 41 || op == 42 || op == 48 || op == 49) &&
            (w[0] >> 16) == 3 && w[2] == id) {
            type = w[1]; *value = op == 41 || op == 48;
            is_spec = op == 48 || op == 49; literal_kind = 2; ++found;
        }
        if (op == 46 && (w[0] >> 16) == 3 && w[2] == id) {
            type = w[1]; *value = 0; is_null = 1; ++found;
        }
        if (op == 52 && (w[0] >> 16) >= 5 && w[2] == id) {
            type = w[1]; expression = w; ++found;
        }
        if (op == 71 && w[0] >> 16 == 4 && w[1] == id && w[2] == 1) {
            spec_id = w[3]; ++decorated;
        }
    }
    if (found != 1 || decorated > 1 || (is_null && decorated)) return 0;
    int scalar = 0;
    *kind = 0;
    for (size_t i = 5; i < module->word_count; i += module->words[i] >> 16) {
        const uint32_t *w = module->words + i;
        if ((w[0] & 0xffff) == 21 && w[0] >> 16 == 4 && w[1] == type && w[2] == 32) {
            ++scalar; *kind = 1;
        }
        if ((w[0] & 0xffff) == 20 && w[0] >> 16 == 2 && w[1] == type) {
            ++scalar; *kind = 2;
        }
    }
    if (scalar != 1 || (literal_kind && literal_kind != *kind)) return 0;
    if (expression) {
        /* Scalar integer and boolean specialization expressions. Use unsigned arithmetic
         * for modular SPIR-V results and wider signed intermediates so host
         * overflow, division and shifts never invoke undefined C behavior.
         * Bound recursion and total visits, including cyclic malformed IDs. */
        unsigned op = expression[3], length = expression[0] >> 16;
        int unary = op == 126 || op == 168 || op == 200;
        int select = op == 169;
        uint32_t a, b = 0, c = 0;
        unsigned ak = 0, bk = 0, ck = 0;
        if (op == 81) {
            unsigned components;
            return !decorated && length >= 6 && constant_extract(module, expression[4],
                expression + 5, length - 5, type, value, &components, specialization, depth + 1, budget) && components == 1;
        }
        if (decorated || length != (select ? 7u : unary ? 5u : 6u) ||
            !constant_value(module, expression[4], &a, specialization, depth + 1, budget, &ak) ||
            (!unary && !constant_value(module, expression[5], &b, specialization, depth + 1, budget, &bk)) ||
            (select && !constant_value(module, expression[6], &c, specialization, depth + 1, budget, &ck)))
            return 0;
        return constant_operation(op, *kind, a, b, c, ak, bk, ck, value);
    }
    if (!is_spec || !decorated || !specialization) return 1;
    if (specialization->mapEntryCount > PS5VK_MAX_SPECIALIZATION_CONSTANTS ||
        (specialization->mapEntryCount && !specialization->pMapEntries)) return 0;
    unsigned matches = 0;
    for (uint32_t i = 0; i < specialization->mapEntryCount; ++i) {
        const VkSpecializationMapEntry *entry = &specialization->pMapEntries[i];
        if (entry->constantID != spec_id) continue;
        if (++matches > 1 || entry->size != sizeof(*value) || !specialization->pData ||
            entry->offset > specialization->dataSize ||
            entry->size > specialization->dataSize - entry->offset) return 0;
        memcpy(value, (const uint8_t *)specialization->pData + entry->offset, sizeof(*value));
    }
    if (*kind == 2) *value = !!*value;
    return 1;
}
/* LocalSize (OpExecutionMode, mode 17) or, when the device enabled
 * maintenance4, LocalSizeId (OpExecutionModeId 331, mode 38) whose three
 * operands are scalar 32-bit constants or specialization expressions, including
 * scalar/vector extraction from typed constant aggregates.
 * A BuiltIn WorkgroupSize constant takes precedence over the execution mode,
 * including its specialization, and is also legal without an execution mode.
 * Unsupported expression types remain rejected rather than guessed. */
static int local_size(VkShaderModule module, const char *name, uint32_t dims[3],
                      VkBool32 local_size_id, const VkSpecializationInfo *specialization)
{
    uint32_t id;
    if (!ps5vk_shader_entry(module, VK_SHADER_STAGE_COMPUTE_BIT, name, &id)) return 0;
    unsigned found = 0, builtins = 0;
    uint32_t workgroup_id = 0;
    for (size_t i = 5; i < module->word_count; i += module->words[i] >> 16) {
        const uint32_t *w = module->words + i;
        if ((w[0] & 0xffff) == 71 && (w[0] >> 16) == 4 && w[2] == 11 && w[3] == 25) {
            workgroup_id = w[1]; ++builtins;
        }
        if ((w[0] & 0xffff) == 16 && w[0] >> 16 == 6 && w[1] == id && w[2] == 17) {
            memcpy(dims, w + 3, 3 * sizeof(*dims)); ++found;
        } else if ((w[0] & 0xffff) == 331 && w[0] >> 16 == 6 && w[1] == id && w[2] == 38) {
            if (!local_size_id) return 0;
            unsigned budget = 4096;
            for (unsigned n = 0; n < 3; ++n) {
                unsigned kind;
                if (!constant_value(module, w[3 + n], &dims[n], specialization, 0, &budget, &kind) ||
                    kind != 1) return 0;
            }
            ++found;
        }
    }
    if (found > 1 || builtins > 1) return 0;
    if (builtins) {
        uint32_t values[4], element_type = 0;
        unsigned components = 0, budget = 4096, integer_types = 0;
        if (!constant_vector(module, workgroup_id, values, &components, &element_type,
                             specialization, 0, &budget) || components != 3) return 0;
        for (size_t i = 5; i < module->word_count; i += module->words[i] >> 16) {
            const uint32_t *w = module->words + i;
            if ((w[0] & 0xffff) == 21 && (w[0] >> 16) == 4 &&
                w[1] == element_type && w[2] == 32) ++integer_types;
        }
        if (integer_types != 1) return 0;
        memcpy(dims, values, 3 * sizeof(*dims));
        return 1;
    }
    return found == 1;
}

VkResult ps5vk_program_resolve(void *context, const uint32_t *words, size_t count,
                             const char *entry, const struct ps5vk_compiled_program **out)
{
    if (!out) return INVALID;
    *out = NULL;
    const struct ps5vk_program_library *library = context;
    if (!library || !entry || !words || count > SIZE_MAX / 4) return INVALID;
    for (size_t j = 0; j < library->count; ++j) {
        const struct ps5vk_compiled_program *p = &library->programs[j];
        if (p->spirv && p->entry && p->spirv_words == count && !strcmp(p->entry, entry) &&
            !memcmp(words, p->spirv, count * sizeof(*words))) { *out = p; return VK_SUCCESS; }
    }
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

/* Vulkan permits only a null initializer for Workgroup variables, and only
 * with explicit shaderZeroInitializeWorkgroupMemory opt-in. Check this before
 * calling the compiler, which otherwise assumes valid SPIR-V. */
static VkResult workgroup_initializers(const uint32_t *words, size_t count, VkBool32 enabled)
{
    for (size_t i = 5; i < count; i += words[i] >> 16) {
        const uint32_t *w = words + i;
        if ((w[0] & 0xffff) != 59 || (w[0] >> 16) < 5 || w[3] != 4) continue;
        if (!enabled) return VK_ERROR_FEATURE_NOT_PRESENT;
        if ((w[0] >> 16) != 5 || !w[1] || w[1] >= words[3] ||
            !w[4] || w[4] >= words[3]) return INVALID;
        uint32_t pointee = 0, initializer_type = 0;
        unsigned pointers = 0, initializers = 0;
        for (size_t j = 5; j < count; j += words[j] >> 16) {
            const uint32_t *v = words + j;
            if ((v[0] & 0xffff) == 32 && (v[0] >> 16) == 4 && v[1] == w[1] && v[2] == 4) {
                pointee = v[3]; ++pointers;
            }
            if ((v[0] & 0xffff) == 46 && (v[0] >> 16) == 3 && v[2] == w[4]) {
                initializer_type = v[1]; ++initializers;
            }
        }
        if (pointers != 1 || initializers != 1 || !pointee || pointee != initializer_type) return INVALID;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *info,
    const VkAllocationCallbacks *a, VkShaderModule *out)
{
    if (!out) return INVALID;
    *out = VK_NULL_HANDLE;
    if (!d || !info || info->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO ||
        !info->pCode || info->codeSize % 4) return INVALID;
    if (info->pNext || info->flags) return VK_ERROR_UNKNOWN;
    if (info->codeSize > 16 * 1024 * 1024 || info->codeSize > SIZE_MAX - sizeof(struct VkShaderModule_T))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!module_valid(info->pCode, info->codeSize / 4)) return INVALID;
    VkResult initialization = workgroup_initializers(info->pCode, info->codeSize / 4,
        !!(d->enabled_features_t09 & PS5VK_T09_FEATURE_ZERO_INITIALIZE_WORKGROUP_MEMORY));
    if (initialization != VK_SUCCESS) return initialization;
    if (subgroup_module_unsupported(info->pCode, info->codeSize / 4,
                                    d->platform_features,
                                    d->physical ?
                                    d->physical->platform.supported_features_t09 : 0u))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    /* Shader modules may be shared across graphics and compute entries, so
     * the Int16 capability must require the logical-device opt-in before
     * either pipeline frontend can accept it. */
    if (!(d->enabled_features & PS5VK_FEATURE_SHADER_INT16) &&
        declares_int16_capability(info->pCode, info->codeSize / 4))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (!ps5vk_spirv_validate_ubo_layout(info->pCode, info->codeSize / 4,
            !!(d->enabled_features & PS5VK_FEATURE_UNIFORM_BUFFER_STANDARD_LAYOUT)))
        return INVALID;
    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkShaderModule m = ps5vk_object_alloc(d->custom_allocator ? &d->allocator : NULL, a,
        sizeof(*m) + info->codeSize, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &saved, &custom);
    if (!m) return VK_ERROR_OUT_OF_HOST_MEMORY;
    m->device = d; m->allocator = saved; m->custom_allocator = custom; m->word_count = info->codeSize / 4;
    memcpy(m->words, info->pCode, info->codeSize); ++d->pipeline_objects; *out = m;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice d, VkShaderModule m, const VkAllocationCallbacks *a)
{
    (void)a;
    if (!d || !m || m->device != d) return;
    VkAllocationCallbacks saved = m->allocator; VkBool32 custom = m->custom_allocator;
    --d->pipeline_objects; ps5vk_object_free(m, &saved, custom);
}

/* OpCapability declarations that name negotiated device features. Int16 is
 * gated by shaderInt16; the Int8 compute probe is private and default off.
 * Broader narrow storage classes remain unsupported. */
static int spirv_narrow_requirements(const uint32_t *words, size_t count,
                                     uint32_t *required)
{
    if (!module_valid(words, count) || !required) return 0;
    *required = 0;
    for (size_t at = 5; at < count;) {
        uint32_t instruction = words[at];
        uint16_t length = instruction >> 16;
        uint16_t opcode = instruction & 0xffffu;
        if (opcode == 17u) { /* OpCapability */
            if (length != 2) return 0;
            switch (words[at + 1]) {
            case 4433u: *required |= PS5VK_FEATURE_STORAGE_BUFFER_16BIT; break;
            case 4448u: *required |= PS5VK_FEATURE_STORAGE_BUFFER_8BIT; break;
            case 5345u: /* VulkanMemoryModel */
                *required |= PS5VK_FEATURE_VULKAN_MEMORY_MODEL; break;
            case 5346u: /* VulkanMemoryModelDeviceScope */
                *required |= PS5VK_FEATURE_VULKAN_MEMORY_MODEL |
                             PS5VK_FEATURE_VULKAN_MEMORY_MODEL_DEVICE_SCOPE; break;
            case 5347u: /* PhysicalStorageBufferAddresses */
                *required |= PS5VK_FEATURE_BUFFER_DEVICE_ADDRESS; break;
            case 39u: /* Int8 */
                *required |= PS5VK_FEATURE_SHADER_INT8_COMPUTE; break;
            case 22u: /* Int16 */
                *required |= PS5VK_FEATURE_SHADER_INT16; break;
            case 4434u: /* UniformAndStorageBuffer16BitAccess */
            case 4449u: /* UniformAndStorageBuffer8BitAccess */
                return 0;
            default: break;
            }
        }
        at += length;
    }
    return 1;
}

static int program_valid(const struct ps5vk_compiled_program *p, VkShaderModule m,
                         VkPipelineLayout layout, const char *entry, const uint32_t dims[3])
{
    if (!p || !p->spirv || !p->code || !p->entry || p->gfx != 1013 || p->wave_size != 32 ||
        !p->code_words || p->code_words > 1024 * 1024 || p->spirv_words != m->word_count ||
        strcmp(p->entry, entry) || memcmp(p->spirv, m->words, m->word_count * 4) ||
        memcmp(p->local_size, dims, 3 * sizeof(*dims)) || !p->vgprs || p->vgprs > 256 ||
        !p->sgprs || p->sgprs > 106 || p->float_mode > 255 || p->ieee_mode > 1 ||
        p->mem_ordered > 1 || p->user_sgprs < 2 || p->user_sgprs > 10 || p->lds_size > 128 ||
        p->tg_size > 1 || p->tidig_components > 2 ||
        p->descriptor_count > PS5VK_MAX_DESCRIPTORS ||
        (p->descriptor_set_mask & ~((1u << PS5VK_MAX_SETS) - 1)) ||
        (!!p->descriptor_count != !!p->descriptor_set_mask)) return 0;
    /* The original amdllpc offline fixture has a separately validated ABI:
     * s0 is the PAL internal pointer and the sole descriptor table is s1.
     * PSBC's reusable ABI reserves s0..s1 and starts direct set pointers at
     * s2.  Keep the legacy shape deliberately narrow instead of relabelling
     * its compiler metadata as the newer ABI. */
    VkBool32 legacy = p->user_sgprs == 2 && p->descriptor_set_mask == 1 &&
        p->descriptor_set_sgpr[0] == 1 && !p->push_constant_size &&
        !p->push_constant_sgpr && !p->grid_size_sgpr;
    if (legacy) {
        for (uint32_t set = 1; set < PS5VK_MAX_SETS; ++set)
            if (p->descriptor_set_sgpr[set]) return 0;
    } else {
        uint32_t expected_user_sgprs = 2;
        for (uint32_t set = 0; set < PS5VK_MAX_SETS; ++set) {
            VkBool32 used = (p->descriptor_set_mask & (1u << set)) != 0;
            if (used) {
                if (set >= layout->set_count || p->descriptor_set_sgpr[set] != expected_user_sgprs++) return 0;
            } else if (p->descriptor_set_sgpr[set]) return 0;
        }
        if (p->push_constant_size) {
            if (p->push_constant_size > layout->push_constant_size ||
                p->push_constant_sgpr != expected_user_sgprs++) return 0;
            for (uint32_t j = 0; j < (p->push_constant_size + 3u) / 4u; ++j)
                if (!(layout->push_constant_stages[j] & VK_SHADER_STAGE_COMPUTE_BIT)) return 0;
        } else if (p->push_constant_sgpr) return 0;
        if (p->grid_size_sgpr) {
            if (p->grid_size_sgpr != expected_user_sgprs) return 0;
            expected_user_sgprs += 3;
        }
        if (p->user_sgprs != expected_user_sgprs) return 0;
    }
    uint64_t invocations = 1;
    for (unsigned j = 0; j < 3; ++j) {
        if (!dims[j] || dims[j] > 1024 || p->tgid[j] > 1) return 0;
        invocations *= dims[j];
    }
    if (invocations > 1024) return 0;
    for (uint32_t j = 0; j < p->descriptor_count; ++j) {
        const struct ps5vk_program_descriptor *b = &p->descriptors[j];
        /* Zero for every type the compute path cannot encode. */
        if (b->set >= layout->set_count || b->binding >= PS5VK_MAX_BINDINGS) return 0;
        const uint32_t dwords = ps5vk_descriptor_span_dwords(&layout->sets[b->set], b->binding, b->type);
        if (!(p->descriptor_set_mask & (1u << b->set)) ||
            b->table_dword % 4 || !dwords ||
            b->table_dword > PS5VK_MAX_TABLE_DWORDS - dwords) return 0;
        const struct ps5vk_binding *binding = &layout->sets[b->set].binding[b->binding];
        if (layout->sets[b->set].type[b->binding] != b->type ||
            binding->count <= b->element || !(binding->stages & VK_SHADER_STAGE_COMPUTE_BIT)) return 0;
        for (uint32_t k = 0; k < j; ++k)
            if (p->descriptors[k].set == b->set &&
                ((p->descriptors[k].table_dword < b->table_dword + dwords &&
                  b->table_dword < p->descriptors[k].table_dword +
                    ps5vk_descriptor_span_dwords(&layout->sets[b->set], p->descriptors[k].binding,
                        p->descriptors[k].type)) ||
                 (p->descriptors[k].binding == b->binding && p->descriptors[k].element == b->element))) return 0;
    }
    return 1;
}

static VkResult create_pipeline_inner(struct ps5vk_compiled_program *compiled_heap, VkDevice d, const VkComputePipelineCreateInfo *info,
                                const VkAllocationCallbacks *a, VkPipeline *out)
{
    if (info->sType != VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO || !info->layout ||
        info->layout->device != d || info->stage.sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO ||
        !info->stage.module || info->stage.module->device != d || !info->stage.pName) return INVALID;
    if (info->pNext ||
        (info->flags & ~(VK_PIPELINE_CREATE_DISPATCH_BASE_BIT_KHR |
                         VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT | VK_PIPELINE_CREATE_DERIVATIVE_BIT |
                         VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT |
                         VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT)) ||
        (info->stage.flags & ~(VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT |
                               VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT)) ||
        info->stage.stage != VK_SHADER_STAGE_COMPUTE_BIT)
        return VK_ERROR_UNKNOWN;
    /* The compute compiler and dispatch ABI are fixed at wave32. A varying
     * request may select that same size; it does not require multiple sizes.
     * Validate before cache lookup, since none of these accepted requests
     * changes the generated code or the existing wave32 dispatch. */
    const VkBool32 varying = !!(info->stage.flags & VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT);
    const VkBool32 full = !!(info->stage.flags & VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT);
    if ((varying && !d->subgroup_size_control_enabled) ||
        (full && !d->compute_full_subgroups_enabled)) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (info->stage.pNext) {
        const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *required = info->stage.pNext;
        if (required->sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO ||
            required->pNext || varying) return INVALID;
        if (!d->subgroup_size_control_enabled) return VK_ERROR_FEATURE_NOT_PRESENT;
        if (required->requiredSubgroupSize != 32) return INVALID;
    }
    const VkBool32 no_compile = !!(info->flags & VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT);
    if ((info->flags & (VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT |
                        VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT)) &&
        !(d->enabled_features_t09 & PS5VK_T09_FEATURE_PIPELINE_CREATION_CACHE_CONTROL))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if ((info->flags & VK_PIPELINE_CREATE_DISPATCH_BASE_BIT_KHR) &&
        !d->device_group_extension_enabled)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (!d->compiler.resolve && (!d->runtime_compiler_enabled || !d->compiler.compile)) return VK_ERROR_UNKNOWN;
    uint32_t required_features = 0;
    const uint32_t compile_features =
        (d->enabled_features & ~PS5VK_FEATURE_SHADER_INT8_COMPUTE) |
        (d->platform_features & PS5VK_FEATURE_SHADER_INT8_COMPUTE);
    if (!spirv_narrow_requirements(info->stage.module->words,
                                   info->stage.module->word_count,
                                   &required_features) ||
        (required_features & ~compile_features))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t dims[3];
    if (!local_size(info->stage.module, info->stage.pName, dims,
                    !!(d->enabled_features_t09 & PS5VK_T09_FEATURE_MAINTENANCE4),
                    info->stage.pSpecializationInfo))
        return VK_ERROR_UNKNOWN;

    /* Reject invalid specialized launch sizes before they reach the compiler. */
    uint64_t invocations = 1;
    for (unsigned j = 0; j < 3; ++j) {
        if (!dims[j] || dims[j] > 1024) return INVALID;
        invocations *= dims[j];
    }
    if (invocations > 1024) return INVALID;

    /* All three full-subgroup forms in the registry constrain X, not merely
     * the product X*Y*Z. With wave32 and at most 1024 invocations the required
     * size also bounds the workgroup to at most 32 subgroups. */
    if (full && dims[0] % 32) return INVALID;

    const struct ps5vk_compiled_program *program = NULL;
    struct ps5vk_cache_entry *entry = NULL;
#define compiled_storage (*compiled_heap)
    uint32_t *compiled_code = NULL;
    COMPUTE_MARK("PS5VK_COMPUTE_PIPELINE phase=begin words=%zu", info->stage.module->word_count);

    struct ps5vk_cache_key key;
    if (!ps5vk_cache_build_key(info->stage.module->words, info->stage.module->word_count,
                               info->stage.pName, info->layout,
                               info->stage.pSpecializationInfo, compile_features,
                               &key)) return INVALID;
    if (d->pipeline_cache) {
            entry = ps5vk_compilation_cache_lookup(d->pipeline_cache, &key, info->stage.module->words);
            if (entry) {
                program = &entry->program;
            } else if (!no_compile && d->runtime_compiler_enabled && d->compiler.compile) {
                VkResult cr = d->compiler.compile(d->compiler.context,
                    info->stage.module->words, info->stage.module->word_count,
                    info->stage.pName, info->layout, info->stage.pSpecializationInfo,
                    compile_features,
                    &compiled_storage, &compiled_code);
                COMPUTE_MARK("PS5VK_COMPUTE_PIPELINE phase=compiled rc=%d descriptors=%u code_words=%zu",
                             (int)cr, compiled_storage.descriptor_count, compiled_storage.code_words);
                if (cr == VK_SUCCESS) {
                    compiled_storage.code = compiled_code;
                    entry = ps5vk_compilation_cache_insert(d->pipeline_cache, &key,
                        info->stage.module->words, &compiled_storage, compiled_code);
                    if (entry) {
                        free(compiled_code);
                        compiled_code = NULL;
                        program = &entry->program;
                    } else {
                        program = &compiled_storage;
                    }
                } else if (cr != VK_ERROR_FEATURE_NOT_PRESENT) {
                    return cr;
                }
            }
    }

    if (!program && d->compiler.resolve) {
        if (info->stage.pSpecializationInfo &&
            info->stage.pSpecializationInfo->mapEntryCount) {
            if (no_compile) return VK_PIPELINE_COMPILE_REQUIRED;
            if (compiled_code) free(compiled_code);
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        VkResult result = d->compiler.resolve(d->compiler.context, info->stage.module->words,
            info->stage.module->word_count, info->stage.pName, &program);
        if (result != VK_SUCCESS) {
            if (compiled_code) free(compiled_code);
            return result == VK_ERROR_FEATURE_NOT_PRESENT ?
                (no_compile ? VK_PIPELINE_COMPILE_REQUIRED : VK_ERROR_UNKNOWN) : result;
        }
    }

    if (!program) {
        if (compiled_code) free(compiled_code);
        return no_compile ? VK_PIPELINE_COMPILE_REQUIRED : VK_ERROR_UNKNOWN;
    }
    COMPUTE_MARK("PS5VK_COMPUTE_PIPELINE phase=validate cached=%d", entry != NULL);
    if (!program_valid(program, info->stage.module, info->layout, info->stage.pName, dims)) {
        if (compiled_code) free(compiled_code);
        if (entry) ps5vk_cache_entry_release(d->pipeline_cache, entry);
        return INVALID;
    }

    VkAllocationCallbacks saved = {0}; VkBool32 custom = VK_FALSE;
    VkPipeline p = ps5vk_object_alloc(d->custom_allocator ? &d->allocator : NULL, a,
        sizeof(*p) + program->code_words * 4, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &saved, &custom);
    if (!p) {
        if (compiled_code) free(compiled_code);
        if (entry) ps5vk_cache_entry_release(d->pipeline_cache, entry);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    p->device = d; p->allocator = saved; p->custom_allocator = custom;
    p->dispatch_base_enabled =
        !!(info->flags & VK_PIPELINE_CREATE_DISPATCH_BASE_BIT_KHR);
    p->allow_derivatives = !!(info->flags & VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT);
    p->set_count = info->layout->set_count;
    memcpy(p->sets, info->layout->sets, sizeof(p->sets));
    p->push_constant_size = info->layout->push_constant_size;
    memcpy(p->push_constant_stages, info->layout->push_constant_stages,
           sizeof(p->push_constant_stages));
    p->program = *program;
    p->cache_entry = entry;
    memcpy(p->code, program->code, program->code_words * 4); p->program.code = p->code;
    /* Pipeline owns all execution metadata and code. Module/compiler source
     * pointers are deliberately removed; neither is needed at dispatch. */
    p->program.spirv = NULL; p->program.spirv_words = 0; p->program.entry = NULL;
    if (compiled_code) free(compiled_code);
    ++d->pipeline_objects; *out = p;
    return VK_SUCCESS;
}
#undef compiled_storage
static VkResult create_pipeline(VkDevice d, const VkComputePipelineCreateInfo *info,
                                const VkAllocationCallbacks *a, VkPipeline *out)
{
    /* Heap, not stack: the program carries PS5VK_MAX_DESCRIPTORS records
     * and the caller may be an application thread with a small stack. */
    struct ps5vk_compiled_program *compiled = calloc(1, sizeof(*compiled));
    if (!compiled) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult result = create_pipeline_inner(compiled, d, info, a, out);
    COMPUTE_MARK("PS5VK_COMPUTE_PIPELINE phase=end rc=%d", (int)result);
    free(compiled);
    return result;
}
VkBool32 ps5vk_pipeline_derivative_valid(VkDevice d, VkPipelineCreateFlags flags,
    VkPipeline base, int32_t base_index, uint32_t ordinal,
    VkPipelineCreateFlags indexed_flags, VkBool32 graphics)
{
    if (!(flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT)) return VK_TRUE;
    if (base_index == -1)
        return base && base->device == d && base->graphics == graphics && base->allow_derivatives;
    if (base || base_index < 0 || (uint32_t)base_index >= ordinal) return VK_FALSE;
    return !!(indexed_flags & VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice d, VkPipelineCache cache,
    uint32_t count, const VkComputePipelineCreateInfo *infos, const VkAllocationCallbacks *a, VkPipeline *out)
{
    if (!out) return INVALID;
    for (uint32_t j = 0; j < count; ++j) out[j] = VK_NULL_HANDLE;
    if (!d || !count || !infos) return INVALID;
    /* A live same-device cache is accepted; it carries no portable records yet,
     * so compilation still comes from the bounded internal cache. */
    if (cache && !ps5vk_pipeline_cache_usable(d, cache)) return VK_ERROR_UNKNOWN;
    VkResult result = VK_SUCCESS;
    for (uint32_t j = 0; j < count; ++j) {
        const int32_t index = infos[j].basePipelineIndex;
        const VkPipelineCreateFlags indexed_flags = index >= 0 && (uint32_t)index < j ? infos[index].flags : 0;
        VkResult r = ps5vk_pipeline_derivative_valid(d, infos[j].flags,
            infos[j].basePipelineHandle, index, j, indexed_flags, VK_FALSE) ?
            create_pipeline(d, &infos[j], a, &out[j]) : VK_ERROR_UNKNOWN;
        if (r != VK_SUCCESS && (result == VK_SUCCESS ||
            (result == VK_PIPELINE_COMPILE_REQUIRED && r < 0))) result = r;
        if (r != VK_SUCCESS && (infos[j].flags & VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT) &&
            (d->enabled_features_t09 & PS5VK_T09_FEATURE_PIPELINE_CREATION_CACHE_CONTROL)) break;
    }
    /* As Vulkan permits, successful siblings remain caller-owned on failure. */
    return result;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice d, VkPipeline p, const VkAllocationCallbacks *a)
{
    (void)a;
    if (!d || !p || p->device != d || p->variant_parent) return;
    /* A dynamic-topology variant is owned by this pipeline: pending work on
     * either keeps both alive, and destroying this one destroys both. */
    for (VkPipeline v = p; v; v = v->topology_variant)
        if (v->pending) { ++d->lifetime_errors; return; }
    if (d->invalidate && !d->invalidate(d, VK_OBJECT_TYPE_PIPELINE, p)) { ++d->lifetime_errors; return; }
    if (p->cache_entry) {
        ps5vk_cache_entry_release(d->pipeline_cache, p->cache_entry);
        p->cache_entry = NULL;
    }
    for (VkPipeline variant = p->topology_variant, next; variant; variant = next) {
        next = variant->topology_variant;
        VkAllocationCallbacks vsaved = variant->allocator; VkBool32 vcustom = variant->custom_allocator;
        if (variant->graphics_state) variant->graphics_release(d, variant->graphics_state);
        --d->pipeline_objects; ps5vk_object_free(variant, &vsaved, vcustom);
    }
    VkAllocationCallbacks saved = p->allocator; VkBool32 custom = p->custom_allocator;
    if (p->graphics && p->graphics_state) p->graphics_release(d, p->graphics_state);
    --d->pipeline_objects; ps5vk_object_free(p, &saved, custom);
}
