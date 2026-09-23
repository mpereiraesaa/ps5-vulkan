#include "spirv_ubo_layout.h"
#include <limits.h>
#include <stdlib.h>

enum { OP_TYPE_BOOL = 20, OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22,
       OP_TYPE_VECTOR = 23, OP_TYPE_MATRIX = 24, OP_TYPE_ARRAY = 28,
       OP_TYPE_RUNTIME_ARRAY = 29, OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32,
       OP_CONSTANT = 43, OP_VARIABLE = 59, OP_DECORATE = 71,
       OP_MEMBER_DECORATE = 72 };
enum { DEC_BLOCK = 2, DEC_BUFFER_BLOCK = 3, DEC_ROW_MAJOR = 4,
       DEC_COL_MAJOR = 5, DEC_ARRAY_STRIDE = 6, DEC_MATRIX_STRIDE = 7,
       DEC_OFFSET = 35, STORAGE_UNIFORM = 2 };

struct member {
    uint32_t type, offset, matrix_stride;
    unsigned has_offset : 1, has_matrix_stride : 1, row_major : 1, col_major : 1;
};
struct type {
    uint32_t kind, a, b, stride, literal;
    struct member *members;
    uint32_t member_count;
    unsigned has_stride : 1, block : 1, buffer_block : 1, has_literal : 1;
};
struct layout { uint64_t align, size; };
struct state { struct type *types; uint32_t bound; int standard; };
struct matrix_order { uint32_t stride; unsigned present : 1, row : 1; };

static int round_up(uint64_t x, uint64_t align, uint64_t *out)
{
    if (!align || align > UINT32_MAX || x > UINT32_MAX ||
        x > UINT32_MAX - (align - 1)) return 0;
    *out = (x + align - 1) / align * align;
    return *out <= UINT32_MAX;
}
static int valid_id(const struct state *s, uint32_t id)
{ return id && id < s->bound; }

static int type_layout(const struct state *s, uint32_t id,
                       struct matrix_order order, unsigned depth,
                       struct layout *out)
{
    if (!valid_id(s, id) || depth > 64) return 0;
    const struct type *t = &s->types[id];
    struct layout elem;
    switch (t->kind) {
    case OP_TYPE_BOOL:
        if (order.present) return 0;
        *out = (struct layout){4, 4}; return 1;
    case OP_TYPE_INT: case OP_TYPE_FLOAT:
        if (order.present || (t->a != 8 && t->a != 16 &&
                              t->a != 32 && t->a != 64)) return 0;
        *out = (struct layout){t->a / 8, t->a / 8}; return 1;
    case OP_TYPE_VECTOR:
        if (order.present || (t->b < 2 || t->b > 4) ||
            !type_layout(s, t->a, (struct matrix_order){0}, depth + 1, &elem) ||
            elem.size != elem.align) return 0;
        *out = (struct layout){elem.align * (t->b == 2 ? 2 : 4), elem.size * t->b};
        return 1;
    case OP_TYPE_MATRIX: {
        if (!order.present || !valid_id(s, t->a)) return 0;
        const struct type *column = &s->types[t->a];
        if (column->kind != OP_TYPE_VECTOR || t->b < 2 || t->b > 4 ||
            column->b < 2 || column->b > 4 ||
            !type_layout(s, column->a, (struct matrix_order){0}, depth + 1, &elem)) return 0;
        uint64_t elements = order.row ? t->b : column->b;
        uint64_t vectors = order.row ? column->b : t->b;
        uint64_t align = elem.align * (elements == 2 ? 2 : 4);
        if (!s->standard && !round_up(align, 16, &align)) return 0;
        uint64_t bytes = elem.size * elements;
        if (!order.stride || order.stride % align || order.stride < bytes ||
            (vectors - 1) * order.stride > UINT32_MAX - bytes) return 0;
        *out = (struct layout){align, (vectors - 1) * order.stride + bytes};
        return 1;
    }
    case OP_TYPE_ARRAY: {
        if (!t->has_stride || !valid_id(s, t->b) ||
            !s->types[t->b].has_literal || !s->types[t->b].literal ||
            !type_layout(s, t->a, order, depth + 1, &elem)) return 0;
        uint64_t align = elem.align, occupied;
        if (!s->standard && !round_up(align, 16, &align)) return 0;
        if (!round_up(elem.size, elem.align, &occupied) ||
            !t->stride || t->stride % align || t->stride < occupied) return 0;
        uint64_t n = s->types[t->b].literal;
        if (n - 1 > (UINT32_MAX - elem.size) / t->stride) return 0;
        *out = (struct layout){align, (n - 1) * t->stride + elem.size};
        return 1;
    }
    case OP_TYPE_STRUCT: {
        if (order.present || !t->members || !t->member_count) return 0;
        uint64_t align = 1, max_end = 0;
        for (uint32_t i = 0; i < t->member_count; ++i) {
            const struct member *m = &t->members[i];
            struct matrix_order child = {m->matrix_stride, m->has_matrix_stride,
                                         m->row_major};
            if (!m->has_offset || (m->row_major && m->col_major) ||
                ((m->row_major || m->col_major) && !m->has_matrix_stride) ||
                !type_layout(s, m->type, child, depth + 1, &elem) ||
                m->offset % elem.align || m->offset > UINT32_MAX - elem.size) return 0;
            uint64_t end = m->offset + elem.size;
            if (elem.align > align) align = elem.align;
            if (end > max_end) max_end = end;
            for (uint32_t j = 0; j < i; ++j) {
                const struct member *prev = &t->members[j];
                struct matrix_order prev_order = {prev->matrix_stride,
                    prev->has_matrix_stride, prev->row_major};
                struct layout prev_layout;
                if (!type_layout(s, prev->type, prev_order, depth + 1, &prev_layout) ||
                    (m->offset < (uint64_t)prev->offset + prev_layout.size &&
                     prev->offset < end)) return 0;
            }
        }
        if (!s->standard && !round_up(align, 16, &align)) return 0;
        uint64_t size;
        if (!round_up(max_end, align, &size)) return 0;
        *out = (struct layout){align, size}; return 1;
    }
    default: return 0;
    }
}

static int parse_types(const uint32_t *words, size_t count, struct state *s)
{
    for (size_t at = 5; at < count;) {
        uint32_t n = words[at] >> 16, op = words[at] & 0xffffu;
        if (!n || n > count - at) return 0;
        const uint32_t *w = words + at;
        uint32_t id = n > 1 ? w[1] : 0;
        if (op >= OP_TYPE_BOOL && op <= OP_TYPE_POINTER && op != 25 &&
            op != 26 && op != 27 && op != 31) {
            if (!valid_id(s, id) || s->types[id].kind) return 0;
            struct type *t = &s->types[id]; t->kind = op;
            if ((op == OP_TYPE_BOOL && n != 2) ||
                (op == OP_TYPE_INT && n != 4) ||
                (op == OP_TYPE_FLOAT && n != 3) ||
                ((op == OP_TYPE_VECTOR || op == OP_TYPE_MATRIX ||
                  op == OP_TYPE_ARRAY || op == OP_TYPE_POINTER) && n != 4) ||
                (op == OP_TYPE_RUNTIME_ARRAY && n != 3) ||
                (op == OP_TYPE_STRUCT && n < 3)) return 0;
            if (op == OP_TYPE_INT || op == OP_TYPE_FLOAT) t->a = w[2];
            if (op == OP_TYPE_VECTOR || op == OP_TYPE_MATRIX || op == OP_TYPE_ARRAY) {
                t->a = w[2]; t->b = w[3];
            }
            if (op == OP_TYPE_POINTER) { t->a = w[2]; t->b = w[3]; }
            if (op == OP_TYPE_RUNTIME_ARRAY) t->a = w[2];
            if (op == OP_TYPE_STRUCT) {
                t->member_count = n - 2;
                t->members = calloc(t->member_count, sizeof(*t->members));
                if (!t->members) return 0;
                for (uint32_t i = 0; i < t->member_count; ++i)
                    t->members[i].type = w[i + 2];
            }
        } else if (op == OP_CONSTANT && n == 4) {
            id = w[2];
            if (!valid_id(s, id) || !valid_id(s, w[1])) return 0;
            if (s->types[w[1]].kind == OP_TYPE_INT && s->types[w[1]].a == 32) {
                if (s->types[id].has_literal) return 0;
                s->types[id].has_literal = 1; s->types[id].literal = w[3];
            }
        }
        at += n;
    }
    return 1;
}

static int parse_decorations(const uint32_t *words, size_t count, struct state *s)
{
    for (size_t at = 5; at < count;) {
        uint32_t n = words[at] >> 16, op = words[at] & 0xffffu;
        const uint32_t *w = words + at;
        if (op == OP_DECORATE && n >= 3) {
            if (!valid_id(s, w[1])) return 0;
            struct type *t = &s->types[w[1]];
            switch (w[2]) {
            case DEC_BLOCK:
                if (n != 3 || t->block) return 0;
                t->block = 1; break;
            case DEC_BUFFER_BLOCK:
                if (n != 3 || t->buffer_block) return 0;
                t->buffer_block = 1; break;
            case DEC_ARRAY_STRIDE:
                if (n != 4 || t->has_stride) return 0;
                t->has_stride = 1; t->stride = w[3]; break;
            default: break;
            }
        } else if (op == OP_MEMBER_DECORATE && n >= 4) {
            if (!valid_id(s, w[1]) || s->types[w[1]].kind != OP_TYPE_STRUCT ||
                w[2] >= s->types[w[1]].member_count) return 0;
            struct member *m = &s->types[w[1]].members[w[2]];
            switch (w[3]) {
            case DEC_OFFSET:
                if (n != 5 || m->has_offset) return 0;
                m->has_offset = 1; m->offset = w[4]; break;
            case DEC_MATRIX_STRIDE:
                if (n != 5 || m->has_matrix_stride) return 0;
                m->has_matrix_stride = 1; m->matrix_stride = w[4]; break;
            case DEC_ROW_MAJOR:
                if (n != 4 || m->row_major) return 0;
                m->row_major = 1; break;
            case DEC_COL_MAJOR:
                if (n != 4 || m->col_major) return 0;
                m->col_major = 1; break;
            default: break;
            }
        }
        at += n;
    }
    return 1;
}

int ps5vk_spirv_validate_ubo_layout(const uint32_t *words, size_t count,
                                    int standard_layout)
{
    if (!words || count < 5 || words[0] != 0x07230203u ||
        !words[3] || words[3] > 262144 || words[4]) return 0;
    int block = 0, uniform = 0;
    for (size_t at = 5; at < count;) {
        uint32_t n = words[at] >> 16, op = words[at] & 0xffffu;
        if (!n || n > count - at) return 0;
        if (op == OP_DECORATE && n == 3 && words[at + 2] == DEC_BLOCK) block = 1;
        if (op == OP_VARIABLE && n >= 4 && words[at + 3] == STORAGE_UNIFORM) uniform = 1;
        at += n;
    }
    if (!block || !uniform) return 1;
    struct state s = {.types = calloc(words[3], sizeof(struct type)),
                      .bound = words[3], .standard = !!standard_layout};
    if (!s.types) return 0;
    int ok = parse_types(words, count, &s) && parse_decorations(words, count, &s);
    if (ok) for (size_t at = 5; at < count;) {
        uint32_t n = words[at] >> 16, op = words[at] & 0xffffu;
        const uint32_t *w = words + at;
        if (op == OP_VARIABLE && n >= 4 && w[3] == STORAGE_UNIFORM) {
            if (!valid_id(&s, w[1])) { ok = 0; break; }
            const struct type *ptr = &s.types[w[1]];
            if (ptr->kind != OP_TYPE_POINTER || ptr->a != STORAGE_UNIFORM ||
                !valid_id(&s, ptr->b)) { ok = 0; break; }
            const struct type *block = &s.types[ptr->b];
            if (block->block && !block->buffer_block) {
                struct layout result;
                if (!type_layout(&s, ptr->b, (struct matrix_order){0}, 0, &result)) {
                    ok = 0; break;
                }
            }
        }
        at += n;
    }
    for (uint32_t id = 1; id < s.bound; ++id) free(s.types[id].members);
    free(s.types);
    return ok;
}
