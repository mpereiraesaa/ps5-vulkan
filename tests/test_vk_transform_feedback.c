#include "vk_transform_feedback.h"
#include <assert.h>
#include <string.h>

/* Capture-interface reflection for VK_EXT_transform_feedback. The modules are
 * assembled word by word so each rule has a minimal counterexample. The first
 * one has the exact shape of the pinned DXVK stream-output geometry shader:
 * Xfb on a geometry entry point, point output, and one output variable per
 * captured element carrying Stream, XfbBuffer, XfbStride and Offset
 * (dxbc_compiler.cpp emitXfbOutputDeclarations, spirv_module.cpp decorateXfb). */

#define UNSET 0xffffffffu
enum { MODEL_VERTEX=0, MODEL_TESS_CTRL=1, MODEL_GEOMETRY=3, MODEL_FRAGMENT=4 };
enum { MAX_WORDS=1024 };

struct module { uint32_t w[MAX_WORDS]; size_t n; };
struct cap { uint32_t buffer, stride, offset, stream; unsigned comps, width; };

static void put(struct module *m, uint32_t op, unsigned count, const uint32_t *operands)
{
    assert(m->n+1u+count<=MAX_WORDS);
    m->w[m->n++]=((1u+count)<<16)|op;
    for(unsigned i=0;i<count;++i)m->w[m->n++]=operands[i];
}
#define I(m,op,...) do { const uint32_t o_[]={__VA_ARGS__}; \
    put((m),(op),(unsigned)(sizeof(o_)/sizeof(o_[0])),o_); } while(0)

/* Ids: 1 main, 2 void, 3 function type, 4 uint, 5 f32, 6 f64, captures at
 * 10+3i (type), 11+3i (pointer), 12+3i (variable), stream constants 40+k,
 * label 60. */
static size_t build(struct module *m, uint32_t model, int xfb, int output_points,
                    const struct cap *caps, unsigned count,
                    const uint32_t *emits, unsigned emit_count)
{
    memset(m,0,sizeof(*m));
    m->w[0]=0x07230203u;m->w[1]=0x10300u;m->w[2]=0;m->w[3]=64;m->w[4]=0;m->n=5;
    I(m,17,1);   /* Shader */
    I(m,17,2);   /* Geometry */
    I(m,17,53);  /* TransformFeedback */
    I(m,17,54);  /* GeometryStreams */
    I(m,14,0,1); /* Logical GLSL450 */
    {
        uint32_t entry[3+2+4]={model,1,0x6e69616du,0};
        unsigned n=4;
        for(unsigned i=0;i<count;++i)entry[n++]=12u+3u*i;
        put(m,15,n,entry);
    }
    if(xfb)I(m,16,1,11);
    if(model==MODEL_GEOMETRY) {
        I(m,16,1,19);                       /* InputPoints */
        I(m,16,1,output_points?27u:28u);    /* OutputPoints / OutputLineStrip */
        I(m,16,1,26,1);                     /* OutputVertices 1 */
    }
    for(unsigned i=0;i<count;++i) {
        const uint32_t var=12u+3u*i;
        I(m,71,var,30,i);                   /* Location */
        if(caps[i].stream!=UNSET)I(m,71,var,29,caps[i].stream);
        if(caps[i].buffer!=UNSET)I(m,71,var,36,caps[i].buffer);
        if(caps[i].stride!=UNSET)I(m,71,var,37,caps[i].stride);
        if(caps[i].offset!=UNSET)I(m,71,var,35,caps[i].offset);
    }
    I(m,19,2);
    I(m,33,3,2);
    I(m,21,4,32,0);
    I(m,22,5,32);
    I(m,22,6,64);
    for(unsigned i=0;i<count;++i) {
        const uint32_t scalar=caps[i].width==64u?6u:5u;
        const uint32_t type=10u+3u*i;
        assert(caps[i].comps>=2 && caps[i].comps<=4);
        I(m,23,type,scalar,caps[i].comps);
        I(m,32,11u+3u*i,3,type);
        I(m,59,11u+3u*i,12u+3u*i,3);
    }
    for(unsigned k=0;k<emit_count;++k)I(m,43,4,40u+k,emits[k]);
    I(m,54,2,1,0,3);
    I(m,248,60);
    if(model==MODEL_GEOMETRY) {
        if(!emit_count)I(m,218);
        for(unsigned k=0;k<emit_count;++k)I(m,220,40u+k);
    }
    I(m,253);
    I(m,56);
    return m->n;
}

static const struct ps5vk_xfb_limits limits={
    .max_streams=4,.max_buffers=4,.max_buffer_data_size=2048,
    .max_buffer_data_stride=2048,.max_stream_data_size=2048,
    .geometry_streams=VK_TRUE,.streams_lines_triangles=VK_FALSE};

static int reflect(const struct module *m, const struct ps5vk_xfb_limits *l,
                   struct ps5vk_xfb_interface *out)
{
    return ps5vk_xfb_reflect(m->w,m->n,1,l,out);
}

static void dxvk_shape(void)
{
    /* Two vec4 elements of one D3D11 stream-output entry list, both stream 0,
     * buffer 0, stride 32, at offsets 0 and 16. */
    struct module m;
    struct ps5vk_xfb_interface x;
    const struct cap caps[]={{0,32,0,0,4,32},{0,32,16,0,4,32}};
    build(&m,MODEL_GEOMETRY,1,1,caps,2,NULL,0);
    assert(reflect(&m,&limits,&x)==PS5VK_XFB_CAPTURES);
    assert(x.captures==2 && x.buffers_mask==1u && x.captured_streams_mask==1u);
    assert(x.emitted_streams_mask==1u);
    assert(x.strides[0]==32 && x.buffer_stream[0]==0 && x.buffer_data_size[0]==32);
    assert(x.stream_bytes[0]==32 && !x.stream_bytes[1]);

    /* The same interface split over two buffers keeps one stride per buffer
     * and sums both buffer data sizes into the stream. */
    const struct cap split[]={{0,16,0,0,4,32},{1,12,0,0,3,32}};
    build(&m,MODEL_GEOMETRY,1,1,split,2,NULL,0);
    assert(reflect(&m,&limits,&x)==PS5VK_XFB_CAPTURES);
    assert(x.buffers_mask==3u && x.strides[0]==16 && x.strides[1]==12);
    assert(x.buffer_data_size[1]==12 && x.stream_bytes[0]==28);
}

static void not_capturing(void)
{
    struct module m;
    struct ps5vk_xfb_interface x;
    const struct cap caps[]={{0,32,0,0,4,32}};
    /* Decorations without the Xfb execution mode capture nothing. */
    build(&m,MODEL_GEOMETRY,0,1,caps,1,NULL,0);
    assert(reflect(&m,&limits,&x)==PS5VK_XFB_NONE && !x.captures && !x.buffers_mask);
    /* A module that is not SPIR-V, or an entry id that names no entry point. */
    assert(ps5vk_xfb_reflect(m.w,4,1,&limits,&x)==PS5VK_XFB_MALFORMED);
    assert(ps5vk_xfb_reflect(m.w,m.n,2,&limits,&x)==PS5VK_XFB_MALFORMED);
    assert(ps5vk_xfb_reflect(m.w,m.n,1,NULL,&x)==PS5VK_XFB_MALFORMED);
    /* Xfb with an output that has no Offset: nothing is captured, and that is
     * a valid (empty) capture interface. */
    const struct cap plain[]={{UNSET,UNSET,UNSET,UNSET,4,32}};
    build(&m,MODEL_VERTEX,1,0,plain,1,NULL,0);
    assert(reflect(&m,&limits,&x)==PS5VK_XFB_CAPTURES && !x.captures);
    assert(x.emitted_streams_mask==1u);
    /* Only the last pre-rasterization stages capture. */
    build(&m,MODEL_FRAGMENT,1,0,caps,1,NULL,0);
    assert(reflect(&m,&limits,&x)==PS5VK_XFB_MALFORMED);
    build(&m,MODEL_TESS_CTRL,1,0,caps,1,NULL,0);
    assert(reflect(&m,&limits,&x)==PS5VK_XFB_MALFORMED);
}

static void malformed(void)
{
    struct module m;
    struct ps5vk_xfb_interface x;
    /* One buffer, two strides. */
    const struct cap strides[]={{0,32,0,0,4,32},{0,48,16,0,4,32}};
    build(&m,MODEL_GEOMETRY,1,1,strides,2,NULL,0);
    assert(reflect(&m,&limits,&x)==PS5VK_XFB_MALFORMED && !x.captures);
    /* One buffer, two streams. */
    const struct cap streams[]={{0,32,0,0,4,32},{0,32,16,1,4,32}};
    build(&m,MODEL_GEOMETRY,1,1,streams,2,NULL,0);
    assert(reflect(&m,&limits,&x)==PS5VK_XFB_MALFORMED);
    /* Offset and stride are dword aligned; 64-bit data is qword aligned. */
    const struct cap offset[]={{0,32,2,0,4,32}};
    build(&m,MODEL_VERTEX,1,0,offset,1,NULL,0);
    assert(reflect(&m,&limits,&x)==PS5VK_XFB_MALFORMED);
    const struct cap stride[]={{0,30,0,0,4,32}};
    build(&m,MODEL_VERTEX,1,0,stride,1,NULL,0);
    assert(reflect(&m,&limits,&x)==PS5VK_XFB_MALFORMED);
    const struct cap wide[]={{0,48,4,0,2,64}};
    build(&m,MODEL_VERTEX,1,0,wide,1,NULL,0);
    assert(reflect(&m,&limits,&x)==PS5VK_XFB_MALFORMED);
    const struct cap wide_ok[]={{0,48,8,0,2,64}};
    build(&m,MODEL_VERTEX,1,0,wide_ok,1,NULL,0);
    assert(reflect(&m,&limits,&x)==PS5VK_XFB_CAPTURES && x.buffer_data_size[0]==24);
    /* Offset without a buffer, or a buffer without a stride. */
    const struct cap no_buffer[]={{UNSET,32,0,0,4,32}};
    build(&m,MODEL_VERTEX,1,0,no_buffer,1,NULL,0);
    assert(reflect(&m,&limits,&x)==PS5VK_XFB_MALFORMED);
    const struct cap no_stride[]={{0,UNSET,UNSET,0,4,32}};
    build(&m,MODEL_VERTEX,1,0,no_stride,1,NULL,0);
    assert(reflect(&m,&limits,&x)==PS5VK_XFB_MALFORMED);
    /* A duplicated decoration is refused rather than resolved by order. */
    const struct cap one[]={{0,32,0,0,4,32}};
    build(&m,MODEL_VERTEX,1,0,one,1,NULL,0);
    struct module twice=m;
    I(&twice,71,12,35,16);
    assert(reflect(&twice,&limits,&x)==PS5VK_XFB_MALFORMED);
    /* Two Xfb execution modes on one entry point. */
    twice=m;
    I(&twice,16,1,11);
    assert(reflect(&twice,&limits,&x)==PS5VK_XFB_MALFORMED);
    /* A stream operand that is not a constant. */
    const uint32_t emit[]={0};
    build(&m,MODEL_GEOMETRY,1,1,one,1,emit,1);
    for(size_t i=5;i<m.n;i+=m.w[i]>>16)
        if((m.w[i]&0xffffu)==220)m.w[i+1]=12;
    assert(reflect(&m,&limits,&x)==PS5VK_XFB_MALFORMED);
}

static void limits_and_streams(void)
{
    struct module m;
    struct ps5vk_xfb_interface x;
    /* A buffer index past the reported buffer count. */
    const struct cap buffer4[]={{4,16,0,0,4,32}};
    build(&m,MODEL_VERTEX,1,0,buffer4,1,NULL,0);
    assert(reflect(&m,&limits,&x)==PS5VK_XFB_EXCEEDS);
    struct ps5vk_xfb_limits narrow=limits;
    narrow.max_buffers=1;
    const struct cap buffer1[]={{1,16,0,0,4,32}};
    build(&m,MODEL_VERTEX,1,0,buffer1,1,NULL,0);
    assert(reflect(&m,&narrow,&x)==PS5VK_XFB_EXCEEDS);
    assert(reflect(&m,&limits,&x)==PS5VK_XFB_CAPTURES);
    /* Stride and buffer data size limits, including their boundaries. */
    const struct cap at_stride[]={{0,2048,2032,0,4,32}};
    build(&m,MODEL_VERTEX,1,0,at_stride,1,NULL,0);
    assert(reflect(&m,&limits,&x)==PS5VK_XFB_CAPTURES && x.buffer_data_size[0]==2048);
    const struct cap over_stride[]={{0,2052,0,0,4,32}};
    build(&m,MODEL_VERTEX,1,0,over_stride,1,NULL,0);
    assert(reflect(&m,&limits,&x)==PS5VK_XFB_EXCEEDS);
    /* Data that runs past its own stride. */
    const struct cap past[]={{0,16,4,0,4,32}};
    build(&m,MODEL_VERTEX,1,0,past,1,NULL,0);
    assert(reflect(&m,&limits,&x)==PS5VK_XFB_EXCEEDS);
    /* Stream data size is the sum of the stream's buffer data sizes. */
    narrow=limits;narrow.max_stream_data_size=31;
    const struct cap two[]={{0,16,0,0,4,32},{1,16,0,0,4,32}};
    build(&m,MODEL_GEOMETRY,1,1,two,2,NULL,0);
    assert(reflect(&m,&narrow,&x)==PS5VK_XFB_EXCEEDS);
    narrow.max_stream_data_size=32;
    assert(reflect(&m,&narrow,&x)==PS5VK_XFB_CAPTURES && x.stream_bytes[0]==32);

    /* Streams: a non-zero stream needs geometryStreams and a stream index
     * inside maxTransformFeedbackStreams, both for captures and emits. */
    const struct cap stream1[]={{0,16,0,0,4,32},{1,16,0,1,4,32}};
    const uint32_t emits[]={0,1};
    build(&m,MODEL_GEOMETRY,1,1,stream1,2,emits,2);
    assert(reflect(&m,&limits,&x)==PS5VK_XFB_CAPTURES);
    assert(x.captured_streams_mask==3u && x.emitted_streams_mask==3u);
    assert(x.buffer_stream[1]==1 && x.stream_bytes[0]==16 && x.stream_bytes[1]==16);
    narrow=limits;narrow.geometry_streams=VK_FALSE;
    assert(reflect(&m,&narrow,&x)==PS5VK_XFB_EXCEEDS);
    narrow=limits;narrow.max_streams=1;
    assert(reflect(&m,&narrow,&x)==PS5VK_XFB_EXCEEDS);
    /* A stream-1 capture is refused on its own, even before anything emits
     * to that stream. */
    build(&m,MODEL_GEOMETRY,1,1,stream1,2,NULL,0);
    narrow=limits;narrow.geometry_streams=VK_FALSE;
    assert(reflect(&m,&narrow,&x)==PS5VK_XFB_EXCEEDS);
    assert(reflect(&m,&limits,&x)==PS5VK_XFB_CAPTURES && x.emitted_streams_mask==1u);
    const uint32_t emit4[]={4};
    const struct cap one[]={{0,16,0,0,4,32}};
    build(&m,MODEL_GEOMETRY,1,1,one,1,emit4,1);
    assert(reflect(&m,&limits,&x)==PS5VK_XFB_EXCEEDS);
    /* Several emitted streams need point output unless the device reports
     * transformFeedbackStreamsLinesTriangles. */
    build(&m,MODEL_GEOMETRY,1,0,stream1,2,emits,2);
    assert(reflect(&m,&limits,&x)==PS5VK_XFB_EXCEEDS);
    narrow=limits;narrow.streams_lines_triangles=VK_TRUE;
    assert(reflect(&m,&narrow,&x)==PS5VK_XFB_CAPTURES);
    /* The profile that captures nothing refuses every capture. */
    static const struct ps5vk_xfb_limits none;
    build(&m,MODEL_VERTEX,1,0,one,1,NULL,0);
    assert(reflect(&m,&none,&x)==PS5VK_XFB_EXCEEDS && !x.captures);
}

static void block_members(void)
{
    /* gl_PerVertex-style output block: member 0 (vec4) carries Offset 0 and is
     * captured into the variable's buffer 1 with stride 16; member 1 (float)
     * has no Offset and is not captured. */
    struct module m;
    memset(&m,0,sizeof(m));
    m.w[0]=0x07230203u;m.w[1]=0x10300u;m.w[3]=64;m.n=5;
    I(&m,17,1);I(&m,17,53);I(&m,14,0,1);
    I(&m,15,0,1,0x6e69616du,0,12);
    I(&m,16,1,11);
    I(&m,71,12,36,1);I(&m,71,12,37,16);
    I(&m,72,10,0,11,0);   /* member 0 BuiltIn Position (ignored here) */
    I(&m,72,10,0,35,0);   /* member 0 Offset 0 */
    I(&m,19,2);I(&m,33,3,2);I(&m,22,5,32);I(&m,23,7,5,4);
    I(&m,30,10,7,5);
    I(&m,32,11,3,10);
    I(&m,59,11,12,3);
    I(&m,54,2,1,0,3);I(&m,248,60);I(&m,253);I(&m,56);
    struct ps5vk_xfb_interface x;
    assert(reflect(&m,&limits,&x)==PS5VK_XFB_CAPTURES);
    assert(x.captures==1 && x.buffers_mask==2u && x.strides[1]==16);
    assert(x.buffer_data_size[1]==16 && x.stream_bytes[0]==16);
    /* A variable Offset beside member Offsets is ambiguous. */
    struct module both=m;
    I(&both,71,12,35,0);
    assert(reflect(&both,&limits,&x)==PS5VK_XFB_MALFORMED);
    /* A member stream overrides nothing it may not: its buffer inherits the
     * variable's, so a second member on another stream in the same buffer
     * is refused. */
    struct module mixed=m;
    I(&mixed,72,10,1,35,4);
    I(&mixed,72,10,1,29,1);
    assert(reflect(&mixed,&limits,&x)==PS5VK_XFB_MALFORMED);
}

int main(void)
{
    dxvk_shape();
    not_capturing();
    malformed();
    limits_and_streams();
    block_members();
    return 0;
}
