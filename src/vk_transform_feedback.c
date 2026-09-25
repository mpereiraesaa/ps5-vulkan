#include "vk_transform_feedback.h"
#include <stdlib.h>
#include <string.h>

/* SPIR-V enumerants (third_party/psbc-reference src/compiler/spirv/spirv.h). */
enum { OP_ENTRY_POINT=15, OP_EXECUTION_MODE=16, OP_TYPE_INT=21, OP_TYPE_FLOAT=22,
       OP_TYPE_VECTOR=23, OP_TYPE_MATRIX=24, OP_TYPE_ARRAY=28, OP_TYPE_STRUCT=30,
       OP_TYPE_POINTER=32, OP_CONSTANT=43, OP_VARIABLE=59, OP_DECORATE=71,
       OP_MEMBER_DECORATE=72, OP_EMIT_VERTEX=218, OP_END_PRIMITIVE=219,
       OP_EMIT_STREAM_VERTEX=220, OP_END_STREAM_PRIMITIVE=221 };
enum { MODE_XFB=11, MODE_OUTPUT_POINTS=27 };
enum { DEC_STREAM=29, DEC_OFFSET=35, DEC_XFB_BUFFER=36, DEC_XFB_STRIDE=37 };
enum { STORAGE_OUTPUT=3 };
enum { MODEL_VERTEX=0, MODEL_TESS_EVAL=2, MODEL_GEOMETRY=3 };
/* A capture module with a million ids is outside anything this profile runs;
 * the bound keeps the per-id table allocation proportionate. */
enum { ID_LIMIT=1u<<20, TYPE_DEPTH_LIMIT=8 };
#define UNSET 0xffffffffu

struct xfb_decorations { uint32_t stream, offset, buffer, stride; };
struct id_entry {
    uint32_t op;
    uint32_t a, b;   /* operands: width, component type/count, pointee, value */
    uint32_t at;     /* word index of the defining instruction */
    struct xfb_decorations dec;
};
struct member_decoration { uint32_t structure, member; struct xfb_decorations dec; };
struct reflection {
    const uint32_t *words;
    size_t count;
    uint32_t bound;
    struct id_entry *ids;
    struct member_decoration *members;
    size_t member_count, member_capacity;
    const struct ps5vk_xfb_limits *limits;
    struct ps5vk_xfb_interface *out;
};

static void clear_decorations(struct xfb_decorations *d)
{
    d->stream=d->offset=d->buffer=d->stride=UNSET;
}

/* One decoration of interest. A second value for the same slot is refused, so
 * the capture cannot depend on which of two decorations is read first. */
static int set_decoration(struct xfb_decorations *d, uint32_t decoration, uint32_t value)
{
    uint32_t *slot=decoration==DEC_STREAM?&d->stream:decoration==DEC_OFFSET?&d->offset:
        decoration==DEC_XFB_BUFFER?&d->buffer:decoration==DEC_XFB_STRIDE?&d->stride:NULL;
    if(!slot)return 0;
    if(*slot!=UNSET || value==UNSET)return PS5VK_XFB_MALFORMED;
    *slot=value;
    return 0;
}

static struct member_decoration *member_entry(struct reflection *r, uint32_t structure,
                                              uint32_t member, int create)
{
    for(size_t i=0;i<r->member_count;++i)
        if(r->members[i].structure==structure && r->members[i].member==member)
            return &r->members[i];
    if(!create)return NULL;
    if(r->member_count==r->member_capacity) {
        size_t capacity=r->member_capacity?r->member_capacity*2u:16u;
        struct member_decoration *grown=realloc(r->members,capacity*sizeof(*grown));
        if(!grown)return NULL;
        r->members=grown;r->member_capacity=capacity;
    }
    struct member_decoration *m=&r->members[r->member_count++];
    m->structure=structure;m->member=member;clear_decorations(&m->dec);
    return m;
}

/* The captured size of a type in bytes. Vulkan captures only 32- and 64-bit
 * components; *wide records a 64-bit one, which doubles the alignment rule. */
static int type_size(struct reflection *r, uint32_t type, unsigned depth,
                     uint64_t *bytes, int *wide)
{
    if(!type || type>=r->bound || depth>TYPE_DEPTH_LIMIT)return PS5VK_XFB_MALFORMED;
    const struct id_entry *t=&r->ids[type];
    uint64_t element=0;
    int result;
    switch(t->op) {
    case OP_TYPE_INT:
    case OP_TYPE_FLOAT:
        if(t->a!=32u && t->a!=64u)return PS5VK_XFB_MALFORMED;
        if(t->a==64u)*wide=1;
        *bytes=t->a/8u;
        return 0;
    case OP_TYPE_VECTOR:
    case OP_TYPE_MATRIX:
    case OP_TYPE_ARRAY: {
        uint32_t count=t->b;
        if(t->op==OP_TYPE_ARRAY) {
            if(!count || count>=r->bound || r->ids[count].op!=OP_CONSTANT)return PS5VK_XFB_MALFORMED;
            count=r->ids[count].a;
        }
        if(!count)return PS5VK_XFB_MALFORMED;
        if((result=type_size(r,t->a,depth+1u,&element,wide)))return result;
        *bytes=element*count;
        return *bytes>UINT32_MAX?PS5VK_XFB_EXCEEDS:0;
    }
    case OP_TYPE_STRUCT: {
        const uint32_t *w=r->words+t->at;
        const uint32_t members=(w[0]>>16)-2u;
        uint64_t sequential=0,extent=0;
        int any_offset=0;
        for(uint32_t m=0;m<members;++m) {
            if((result=type_size(r,w[2u+m],depth+1u,&element,wide)))return result;
            const struct member_decoration *d=member_entry(r,type,m,0);
            if(d && d->dec.offset!=UNSET) {
                any_offset=1;
                if(d->dec.offset+element>extent)extent=d->dec.offset+element;
            }
            sequential+=element;
        }
        *bytes=any_offset?extent:sequential;
        return *bytes>UINT32_MAX?PS5VK_XFB_EXCEEDS:0;
    }
    default:
        /* Booleans, pointers, images and anything else cannot be captured. */
        return PS5VK_XFB_MALFORMED;
    }
}

/* Record one captured range and apply the per-output rules. */
static int capture(struct reflection *r, const struct xfb_decorations *dec,
                   uint64_t size, int wide)
{
    const struct ps5vk_xfb_limits *l=r->limits;
    struct ps5vk_xfb_interface *o=r->out;
    const uint32_t buffer=dec->buffer, stride=dec->stride, offset=dec->offset;
    const uint32_t stream=dec->stream==UNSET?0u:dec->stream;
    const uint32_t alignment=wide?8u:4u;
    if(buffer==UNSET || stride==UNSET || offset==UNSET || !size)return PS5VK_XFB_MALFORMED;
    if((offset%alignment) || (stride%alignment))return PS5VK_XFB_MALFORMED;
    if(buffer>=PS5VK_XFB_ABI_BUFFERS || buffer>=l->max_buffers ||
       stream>=PS5VK_XFB_ABI_STREAMS || stream>=l->max_streams ||
       (stream && !l->geometry_streams))return PS5VK_XFB_EXCEEDS;
    if(o->buffers_mask&(1u<<buffer)) {
        /* One buffer has one stride and belongs to one stream. */
        if(o->strides[buffer]!=stride || o->buffer_stream[buffer]!=stream)
            return PS5VK_XFB_MALFORMED;
    }
    const uint64_t end=(uint64_t)offset+size;
    if(stride>l->max_buffer_data_stride || end>l->max_buffer_data_size ||
       /* The compiler lays out one vertex per stride: data that crosses into
        * the next vertex's range is not something it describes. */
       end>stride)return PS5VK_XFB_EXCEEDS;
    o->buffers_mask|=1u<<buffer;
    o->captured_streams_mask|=1u<<stream;
    o->strides[buffer]=stride;
    o->buffer_stream[buffer]=stream;
    if(end>o->buffer_data_size[buffer])o->buffer_data_size[buffer]=(uint32_t)end;
    ++o->captures;
    return 0;
}

/* The captures of one Output variable. The variable's own decorations are what
 * its members inherit; a member with its own Offset is a separate capture. */
static int capture_variable(struct reflection *r, uint32_t variable)
{
    const struct id_entry *v=&r->ids[variable];
    if(!v->a || v->a>=r->bound || r->ids[v->a].op!=OP_TYPE_POINTER)return PS5VK_XFB_MALFORMED;
    const uint32_t type=r->ids[v->a].b;
    if(!type || type>=r->bound)return PS5VK_XFB_MALFORMED;
    const struct id_entry *t=&r->ids[type];
    int result, wide=0;
    uint64_t size=0;
    if(t->op==OP_TYPE_STRUCT) {
        const uint32_t *w=r->words+t->at;
        const uint32_t members=(w[0]>>16)-2u;
        int member_captures=0;
        for(uint32_t m=0;m<members;++m) {
            const struct member_decoration *d=member_entry(r,type,m,0);
            if(!d || d->dec.offset==UNSET)continue;
            struct xfb_decorations dec=d->dec;
            if(dec.buffer==UNSET)dec.buffer=v->dec.buffer;
            if(dec.stride==UNSET)dec.stride=v->dec.stride;
            if(dec.stream==UNSET)dec.stream=v->dec.stream;
            wide=0;
            if((result=type_size(r,w[2u+m],1u,&size,&wide)) ||
               (result=capture(r,&dec,size,wide)))return result;
            member_captures=1;
        }
        if(member_captures) {
            if(v->dec.offset!=UNSET)return PS5VK_XFB_MALFORMED;
            return 0;
        }
    }
    if(v->dec.offset==UNSET) {
        /* An output with no Offset is not captured. A buffer or stride with no
         * offset to go with it describes nothing and is refused. */
        return v->dec.buffer!=UNSET && v->dec.stride==UNSET?PS5VK_XFB_MALFORMED:0;
    }
    if((result=type_size(r,type,0u,&size,&wide)))return result;
    return capture(r,&v->dec,size,wide);
}

static int emit_stream(struct reflection *r, uint32_t constant)
{
    if(!constant || constant>=r->bound || r->ids[constant].op!=OP_CONSTANT)
        return PS5VK_XFB_MALFORMED;
    const uint32_t stream=r->ids[constant].a;
    if(stream>=PS5VK_XFB_ABI_STREAMS || stream>=r->limits->max_streams ||
       (stream && !r->limits->geometry_streams))return PS5VK_XFB_EXCEEDS;
    r->out->emitted_streams_mask|=1u<<stream;
    return 0;
}

static int parse(struct reflection *r)
{
    const uint32_t *words=r->words;
    int result;
    for(size_t at=5;at<r->count;at+=words[at]>>16) {
        const uint32_t *w=words+at;
        const uint32_t n=w[0]>>16, op=w[0]&0xffffu;
        uint32_t id=0;
        switch(op) {
        case OP_TYPE_INT: case OP_TYPE_FLOAT:
            if(n<3)return PS5VK_XFB_MALFORMED;
            id=w[1];
            break;
        case OP_TYPE_VECTOR: case OP_TYPE_MATRIX: case OP_TYPE_ARRAY:
            if(n!=4)return PS5VK_XFB_MALFORMED;
            id=w[1];
            break;
        case OP_TYPE_STRUCT:
            if(n<2)return PS5VK_XFB_MALFORMED;
            id=w[1];
            break;
        case OP_TYPE_POINTER:
            if(n!=4)return PS5VK_XFB_MALFORMED;
            id=w[1];
            break;
        case OP_CONSTANT:
            if(n<4)return PS5VK_XFB_MALFORMED;
            id=w[2];
            break;
        case OP_VARIABLE:
            if(n<4)return PS5VK_XFB_MALFORMED;
            id=w[2];
            break;
        case OP_DECORATE:
            if(n<3 || !w[1] || w[1]>=r->bound)return PS5VK_XFB_MALFORMED;
            if(w[2]==DEC_STREAM || w[2]==DEC_OFFSET || w[2]==DEC_XFB_BUFFER ||
               w[2]==DEC_XFB_STRIDE) {
                if(n!=4)return PS5VK_XFB_MALFORMED;
                if((result=set_decoration(&r->ids[w[1]].dec,w[2],w[3])))return result;
            }
            continue;
        case OP_MEMBER_DECORATE:
            if(n<4 || !w[1] || w[1]>=r->bound)return PS5VK_XFB_MALFORMED;
            if(w[3]==DEC_STREAM || w[3]==DEC_OFFSET || w[3]==DEC_XFB_BUFFER ||
               w[3]==DEC_XFB_STRIDE) {
                if(n!=5)return PS5VK_XFB_MALFORMED;
                struct member_decoration *m=member_entry(r,w[1],w[2],1);
                if(!m)return PS5VK_XFB_EXCEEDS;
                if((result=set_decoration(&m->dec,w[3],w[4])))return result;
            }
            continue;
        case OP_EMIT_VERTEX: case OP_END_PRIMITIVE:
            r->out->emitted_streams_mask|=1u;
            continue;
        case OP_EMIT_STREAM_VERTEX: case OP_END_STREAM_PRIMITIVE:
            if(n!=2)return PS5VK_XFB_MALFORMED;
            if((result=emit_stream(r,w[1])))return result;
            continue;
        default:
            continue;
        }
        if(!id || id>=r->bound || r->ids[id].op)return PS5VK_XFB_MALFORMED;
        struct id_entry *e=&r->ids[id];
        e->op=op;e->at=(uint32_t)at;
        if(op==OP_TYPE_INT || op==OP_TYPE_FLOAT)e->a=w[2];
        else if(op==OP_TYPE_VECTOR || op==OP_TYPE_MATRIX || op==OP_TYPE_ARRAY) {
            e->a=w[2];e->b=w[3];
        } else if(op==OP_TYPE_POINTER) {
            e->a=w[2];e->b=w[3];
        } else if(op==OP_CONSTANT) {
            e->a=w[3];
        } else if(op==OP_VARIABLE) {
            e->a=w[1];e->b=w[3];
        }
    }
    return 0;
}

int ps5vk_xfb_reflect(const uint32_t *words, size_t count, uint32_t entry,
                      const struct ps5vk_xfb_limits *limits,
                      struct ps5vk_xfb_interface *out)
{
    if(!out)return PS5VK_XFB_MALFORMED;
    memset(out,0,sizeof(*out));
    if(!words || count<5 || words[0]!=0x07230203u || !limits || !entry ||
       entry>=words[3])return PS5VK_XFB_MALFORMED;
    /* The entry point, its interface and its execution modes come first. A
     * module without Xfb on this entry point costs one walk and no memory. */
    size_t entry_at=0, interface_first=0;
    int xfb=0, output_points=0;
    for(size_t at=5;at<count;) {
        const uint32_t *w=words+at;
        const uint32_t n=w[0]>>16, op=w[0]&0xffffu;
        if(!n || n>count-at)return PS5VK_XFB_MALFORMED;
        if(op==OP_ENTRY_POINT && n>=4 && w[2]==entry) {
            if(entry_at)return PS5VK_XFB_MALFORMED;
            const char *name=(const char *)(w+3);
            const char *end=memchr(name,0,(size_t)(n-3u)*4u);
            if(!end)return PS5VK_XFB_MALFORMED;
            entry_at=at;
            interface_first=3u+(size_t)(end-name)/4u+1u;
        } else if(op==OP_EXECUTION_MODE && n>=3 && w[1]==entry) {
            if(w[2]==MODE_XFB) {
                if(n!=3 || xfb)return PS5VK_XFB_MALFORMED;
                xfb=1;
            } else if(w[2]==MODE_OUTPUT_POINTS) output_points=1;
        }
        at+=n;
    }
    if(!entry_at)return PS5VK_XFB_MALFORMED;
    if(!xfb)return PS5VK_XFB_NONE;
    const uint32_t model=words[entry_at+1];
    /* Only the last pre-rasterization stage can capture. */
    if(model!=MODEL_VERTEX && model!=MODEL_TESS_EVAL && model!=MODEL_GEOMETRY)
        return PS5VK_XFB_MALFORMED;
    if(words[3]>ID_LIMIT)return PS5VK_XFB_EXCEEDS;
    struct reflection r={.words=words,.count=count,.bound=words[3],.limits=limits,.out=out};
    r.ids=calloc(r.bound,sizeof(*r.ids));
    if(!r.ids)return PS5VK_XFB_EXCEEDS;
    for(uint32_t i=0;i<r.bound;++i)clear_decorations(&r.ids[i].dec);
    int result=parse(&r);
    const uint32_t *e=words+entry_at;
    const size_t n=e[0]>>16;
    for(size_t i=interface_first;!result && i<n;++i) {
        const uint32_t id=e[i];
        if(!id || id>=r.bound){result=PS5VK_XFB_MALFORMED;break;}
        if(r.ids[id].op!=OP_VARIABLE || r.ids[id].b!=STORAGE_OUTPUT)continue;
        result=capture_variable(&r,id);
    }
    if(!result) {
        /* The vertex and evaluation stages write stream zero implicitly. */
        if(model!=MODEL_GEOMETRY)out->emitted_streams_mask|=1u;
        for(uint32_t b=0;b<PS5VK_XFB_ABI_BUFFERS;++b)
            if(out->buffers_mask&(1u<<b)) {
                const uint32_t s=out->buffer_stream[b];
                const uint64_t sum=(uint64_t)out->stream_bytes[s]+out->buffer_data_size[b];
                if(sum>limits->max_stream_data_size){result=PS5VK_XFB_EXCEEDS;break;}
                out->stream_bytes[s]=(uint32_t)sum;
            }
    }
    if(!result && (out->emitted_streams_mask&(out->emitted_streams_mask-1u)) &&
       !limits->streams_lines_triangles && !output_points)
        result=PS5VK_XFB_EXCEEDS;
    free(r.ids);
    free(r.members);
    if(result) {
        memset(out,0,sizeof(*out));
        return result;
    }
    return PS5VK_XFB_CAPTURES;
}
