#include "spirv_graphics_interface.h"
#include "graphics_formats.h"
#include "color_attachment_contract.h"
#include <stdlib.h>
#include <string.h>

enum { ID_LIMIT=65536, LOCATIONS=32, BLOCK_MEMBERS=8 };
/* SPIR-V execution models this profile describes. */
enum { MODEL_VERTEX=0, MODEL_TESS_CTRL=1, MODEL_TESS_EVAL=2, MODEL_GEOMETRY=3,
       MODEL_FRAGMENT=4 };
/* Built-in decoration ids this profile knows by name. */
enum { BUILTIN_POSITION=0, BUILTIN_POINT_SIZE=1, BUILTIN_CLIP_DISTANCE=3,
       BUILTIN_CULL_DISTANCE=4, BUILTIN_VERTEX_INDEX=42, BUILTIN_INSTANCE_INDEX=43,
       BUILTIN_BASE_VERTEX=4424, BUILTIN_BASE_INSTANCE=4425, BUILTIN_DRAW_INDEX=4426,
       BUILTIN_VIEW_INDEX=4440, BUILTIN_VIEWPORT_INDEX=10, BUILTIN_FRAG_COORD=15,
       /* The sample index a per-sample shaded invocation was launched for. */
       BUILTIN_SAMPLE_ID=18 };
/* The tessellation built-ins the two stages exchange with the tessellator, and
 * the decorations/execution modes that describe a patch. Values are the pinned
 * SPIR-V enumerants (third_party/psbc-reference src/compiler/spirv/spirv.h). */
enum { BUILTIN_PRIMITIVE_ID=7, BUILTIN_INVOCATION_ID=8, BUILTIN_TESS_LEVEL_OUTER=11,
       BUILTIN_TESS_LEVEL_INNER=12, BUILTIN_TESS_COORD=13, BUILTIN_PATCH_VERTICES=14 };
enum { DECORATION_PATCH=15 };
enum { MODE_SPACING_EQUAL=1, MODE_SPACING_FRACTIONAL_EVEN=2,
       MODE_SPACING_FRACTIONAL_ODD=3, MODE_VERTEX_ORDER_CW=4,
       MODE_VERTEX_ORDER_CCW=5, MODE_POINT_MODE=10,
       MODE_INPUT_POINTS=19, MODE_INPUT_LINES=20, MODE_INPUT_LINES_ADJACENCY=21,
       MODE_TRIANGLES=22, MODE_INPUT_TRIANGLES_ADJACENCY=23,
       MODE_QUADS=24, MODE_ISOLINES=25,
       MODE_DOMAIN_TRIANGLES=22, MODE_DOMAIN_QUADS=24, MODE_DOMAIN_ISOLINES=25,
       MODE_OUTPUT_VERTICES=26, MODE_OUTPUT_POINTS=27, MODE_OUTPUT_LINE_STRIP=28,
       MODE_OUTPUT_TRIANGLE_STRIP=29 };
struct id_info {
    unsigned op, type, count, signedness, storage, location, builtin, index,
        index_set, forbidden, selected, flat, patch;
    /* OpTypeStruct member type ids, for the bounded built-in block below. */
    unsigned member_types[BLOCK_MEMBERS];
};
struct interface_slot { unsigned components, numeric; };
struct interface {
    /* The geometry stage's INPUT PRIMITIVE, from its execution mode: Triangles,
     * Quads, Isolines, InputPoints or InputLines. A stage that reads nothing
     * per-vertex declares no gl_in array at all, so the mode is what binds it to
     * the primitive the pipeline assembles. */
    unsigned input_primitive;
    struct interface_slot inputs[LOCATIONS], outputs[LOCATIONS];
    /* Fragment Location 0, Index 1 is the secondary source for MRT0 rather
     * than MRT1.  Keep it in a distinct namespace so ordinary location
     * collision checks remain strict and no other indexed output is widened. */
    struct interface_slot secondary_outputs[LOCATIONS];
    /* Per-patch interface: a variable or built-in that carries the Patch
     * decoration. It lives at the same locations as the per-vertex interface,
     * so it needs its own slots to be describable at all. SPIRV-Tools #5654
     * explicitly separates Patch and non-Patch location conflict checks;
     * GLSL frontend rejection alone must not narrow the SPIR-V contract. */
    struct interface_slot patch_inputs[LOCATIONS], patch_outputs[LOCATIONS];
    /* Declared gl_ClipDistance/gl_CullDistance array lengths, in components.
     * They start at zero and are set at most once per stage. */
    unsigned clip_distances, cull_distances;
    /* The geometry stage's gl_ViewportIndex export. It is not a varying: it is
     * a 32-bit integer scalar with no location that selects one of the viewport
     * banks the pipeline programs, and core Vulkan lets ONLY a geometry stage
     * write it. Set at most once per stage, like the distance arrays; whether
     * the pipeline may use it at all is the multiViewport negotiation, which
     * the adapter decides on the logical device's enabled mask. */
    unsigned viewport_index;
    /* The same two arrays as PIXEL INPUTS: a fragment stage reads what the
     * last pre-raster stage exported. The rasterizer delivers those components
     * exactly like a varying, from the same packed position registers, so a
     * read is budgeted against the same ceiling as an export - but it is not
     * an export, and a stage cannot be both. */
    unsigned clip_distance_reads, cull_distance_reads;
    /* A geometry stage's per-vertex input array length: the number of vertices
     * of the input primitive, which the pipeline topology must agree with. */
    unsigned input_vertices;
    /* Tessellation facts, all from the module's execution modes: the domain
     * (0 when none), the spacing and winding modes, the point mode, and the
     * control stage's declared output vertex count. patch_vertices records the
     * largest declared per-vertex array length, which a front end emits as
     * gl_MaxPatchVertices (32) for inputs; the count that binds a pipeline is
     * the control stage's OutputVertices plus the pipeline's patch control
     * points, not this bound. */
    unsigned domain, spacing, winding, point_mode, control_points, patch_vertices;
};

/* A declared array of a literal length: the form every per-vertex interface in
 * this profile uses, whether it is a distance array, a geometry input array or
 * a redeclared built-in block. */
static int declared_array(const struct id_info *ids,unsigned bound,unsigned type,
                          unsigned *length,unsigned *element)
{
    if(!type || type>=bound)return 0;
    const struct id_info *array=&ids[type];
    if(array->op!=28 || !array->type || array->type>=bound || !array->count ||
       array->count>=bound)return 0;
    const struct id_info *constant=&ids[array->count];
    /* Only a literal 32-bit integer length is a declaration this profile can
     * bound; a spec constant or a non-integer length is not. */
    if(constant->op!=43 || !constant->type || constant->type>=bound)return 0;
    const struct id_info *constant_type=&ids[constant->type];
    if(constant_type->op!=21 || constant_type->count!=32)return 0;
    if(!constant->count)return 0;
    *length=constant->count;
    *element=array->type;
    return 1;
}

/* Flatten a location-qualified 32-bit interface into occupied locations.
 * Arrays repeat the complete element span; matrices occupy one location per
 * column. Bound both recursion and output so malformed cyclic/huge types fail
 * before any unbounded work. The outer per-vertex array is removed by caller. */
static int interface_locations(const struct id_info *ids,unsigned bound,unsigned id,
                               struct interface_slot *slots,unsigned *count,
                               unsigned depth)
{
    if(!id || id>=bound || depth>=LOCATIONS || *count>=LOCATIONS)return 0;
    const struct id_info *type=&ids[id];
    if(type->op==28 || type->op==24) {
        unsigned length=type->count,element=type->type;
        if(type->op==28) {
            if(!declared_array(ids,bound,id,&length,&element))return 0;
        } else {
            if(length<2 || length>4 || !element || element>=bound ||
               ids[element].op!=23 || !ids[element].type ||
               ids[element].type>=bound || ids[ids[element].type].op!=22)return 0;
        }
        if(!length || length>LOCATIONS-*count)return 0;
        for(unsigned i=0;i<length;++i)
            if(!interface_locations(ids,bound,element,slots,count,depth+1))return 0;
        return 1;
    }
    unsigned components=1;
    if(type->op==23) {
        components=type->count;
        if(components<2 || components>4 || !type->type || type->type>=bound)return 0;
        type=&ids[type->type];
    }
    unsigned numeric=PS5VK_VERTEX_NUMERIC_NONE;
    if(type->op==22 && type->count==32)numeric=PS5VK_VERTEX_NUMERIC_FLOAT;
    else if(type->op==21 && type->count==32)
        numeric=type->signedness?PS5VK_VERTEX_NUMERIC_SINT:PS5VK_VERTEX_NUMERIC_UINT;
    else return 0;
    slots[(*count)++]=(struct interface_slot){components,numeric};
    return 1;
}

/* A declared float32 distance array: the SPIR-V form of gl_ClipDistance and
 * gl_CullDistance, whether the front end writes it as a block member or as a
 * standalone variable. */
static int declared_distance_array(const struct id_info *ids,unsigned bound,
                                   unsigned type,unsigned *length)
{
    unsigned element=0;
    if(!declared_array(ids,bound,type,length,&element))return 0;
    if(*length>PS5VK_MAX_CLIP_DISTANCES)return 0;
    if(!element || element>=bound)return 0;
    const struct id_info *component=&ids[element];
    return component->op==22 && component->count==32;
}

/* gl_PerVertex may declare unused builtin arrays, so a declaration alone is
 * not usage. What the block may NOT do is declare a member this profile cannot
 * deliver: Position is a float32 vec4, PointSize a float32 scalar, and
 * ClipDistance/CullDistance an array of float32 no wider than the exported
 * distance registers. Effective clip/cull usage is independently gated by the
 * native compiler metadata adapter, which is what the pipeline creation path
 * consults before a device advertises either feature. */
/* The number of vertices in one input primitive of a topology, or zero for a
 * topology this profile does not feed a geometry stage from. The geometry
 * stage's declared per-vertex input array must hold exactly one such primitive,
 * which is what binds the shader's declaration to the pipeline's topology. */
/* The EXECUTION MODE a geometry stage must declare for each topology this
 * profile feeds one from: points for a point list, lines for either line
 * topology, triangles for a triangle list, strip or fan, and the adjacency
 * modes (4 and 6 vertices) for the adjacency topologies. */
static unsigned ps5vk_topology_input_mode(VkPrimitiveTopology topology)
{
    switch(topology) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:return MODE_INPUT_POINTS;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:return MODE_INPUT_LINES;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:return MODE_TRIANGLES;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:return MODE_INPUT_LINES_ADJACENCY;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:return MODE_INPUT_TRIANGLES_ADJACENCY;
    default:return 0u;
    }
}

static unsigned ps5vk_topology_input_vertices(VkPrimitiveTopology topology)
{
    switch(topology) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:return 1u;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:return 2u;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:return 3u;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:return 4u;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:return 6u;
    default:return 0u;
    }
}

static int builtin_block(const struct ps5vk_graphics_module_key *m,
                         const struct id_info *ids,unsigned bound,
                         unsigned id,unsigned members,unsigned *clip,unsigned *cull)
{
    if(!members || members>BLOCK_MEMBERS)return 0;
    unsigned seen=0;
    for(size_t at=5;at<m->word_count;at+=m->words[at]>>16) {
        const uint32_t *w=m->words+at;unsigned n=w[0]>>16;
        if((w[0]&65535)==72 && n>=4 && w[1]==id) {
            if(n!=5 || w[3]!=11 || w[2]>=members || (seen&(1u<<w[2])))return 0;
            unsigned type=ids[id].member_types[w[2]];
            if(type>=bound)return 0;
            if(w[4]==BUILTIN_POSITION) {
                if(ids[type].op!=23 || ids[type].count!=4 || !ids[type].type ||
                   ids[type].type>=bound)return 0;
                const struct id_info *component=&ids[ids[type].type];
                if(component->op!=22 || component->count!=32)return 0;
            } else if(w[4]==BUILTIN_POINT_SIZE) {
                if(ids[type].op!=22 || ids[type].count!=32)return 0;
            } else if(w[4]==BUILTIN_CLIP_DISTANCE || w[4]==BUILTIN_CULL_DISTANCE) {
                unsigned length=0;
                if(!declared_distance_array(ids,bound,type,&length))return 0;
                /* A block an earlier stage wrote is an input here: the members
                 * are validated but they are not this stage's exports, so a
                 * geometry stage may hold the unused arrays its predecessor and
                 * successor both declare. */
                unsigned *total=w[4]==BUILTIN_CLIP_DISTANCE?clip:cull;
                if(total) {
                    if(*total)return 0;
                    *total=length;
                }
            } else return 0;
            seen|=1u<<w[2];
        }
    }
    return seen==((1u<<members)-1);
}

static int reflect(const struct ps5vk_graphics_module_key *m,unsigned model,struct interface *out)
{
    if(!m->words || m->word_count<5 || !m->entry || m->words[0]!=0x07230203u ||
       !m->words[3] || m->words[3]>ID_LIMIT)return 0;
    unsigned bound=m->words[3],entries=0;
    /* The entry point this reflection describes; execution modes name it. */
    unsigned entry_id=0;
    struct id_info *ids=calloc(bound,sizeof(*ids));
    if(!ids)return 0;
    for(unsigned i=0;i<bound;++i)ids[i].location=ids[i].builtin=~0u;
    int valid=0;
    for(size_t at=5;at<m->word_count;) {
        const uint32_t *w=m->words+at;unsigned n=w[0]>>16,op=w[0]&65535;
        if(!n || n>m->word_count-at)goto done;
        if(op==15) {
            if(n<4)goto done;
            const char *name=(const char *)(w+3);
            const char *end=memchr(name,0,(n-3)*4u);
            if(!end)goto done;
            if(w[1]==model && !strcmp(name,m->entry)) {
                ++entries;
                /* Operand 1 is the execution model, operand 2 the entry point
                 * id an execution mode names, then the entry point name. */
                entry_id=w[2];
                size_t first=3+((size_t)(end-name)+1+3)/4;
                for(size_t j=first;j<n;++j) {
                    if(!w[j] || w[j]>=bound || ids[w[j]].selected)goto done;
                    ids[w[j]].selected=1;
                }
            }
        } else if(op==16 || op==331) {
            /* OpExecutionMode (literal operand) and OpExecutionModeId (constant
             * id operand). The tessellation contract is stated here, so the
             * modes that describe a patch are read for the stage they belong to
             * and a duplicate is refused rather than silently redefined. Every
             * other execution mode stays outside what this policy describes and
             * is left to the compiler, which is what it was before: a fragment
             * stage states its origin, a geometry stage its maximum vertex
             * count and its strip topology. */
            if(!entry_id || n<3 || w[1]!=entry_id)goto done;
            const unsigned mode=w[2];
            if((model==MODEL_TESS_CTRL || model==MODEL_TESS_EVAL) && mode==MODE_OUTPUT_VERTICES) {
                unsigned value=0;
                if(op==16) {
                    if(n!=4)goto done;
                    value=w[3];
                } else {
                    if(n!=4 || !w[3] || w[3]>=bound || ids[w[3]].op!=43)goto done;
                    value=ids[w[3]].count;
                }
                if(!value || value>PS5VK_MAX_PATCH_CONTROL_POINTS ||
                   out->control_points)goto done;
                out->control_points=value;
            } else if(model==MODEL_GEOMETRY &&
                      (mode==MODE_INPUT_POINTS || mode==MODE_INPUT_LINES ||
                       mode==MODE_INPUT_LINES_ADJACENCY || mode==MODE_TRIANGLES ||
                       mode==MODE_INPUT_TRIANGLES_ADJACENCY || mode==MODE_QUADS ||
                       mode==MODE_ISOLINES)) {
                /* One input primitive per stage, and it is the same execution
                 * mode space the tessellation domains use - keyed by the stage
                 * model above, so the two cannot be confused. */
                if(n!=3 || out->input_primitive)goto done;
                out->input_primitive=mode;
            } else if((model==MODEL_TESS_CTRL || model==MODEL_TESS_EVAL) &&
                      (mode==MODE_DOMAIN_TRIANGLES || mode==MODE_DOMAIN_QUADS ||
                       mode==MODE_DOMAIN_ISOLINES || mode==MODE_SPACING_EQUAL ||
                       mode==MODE_SPACING_FRACTIONAL_EVEN ||
                       mode==MODE_SPACING_FRACTIONAL_ODD ||
                       mode==MODE_VERTEX_ORDER_CW || mode==MODE_VERTEX_ORDER_CCW ||
                       mode==MODE_POINT_MODE)) {
                if(n!=3)goto done;
                if(mode==MODE_DOMAIN_TRIANGLES || mode==MODE_DOMAIN_QUADS ||
                   mode==MODE_DOMAIN_ISOLINES) {
                    if(out->domain)goto done;
                    out->domain=mode;
                } else if(mode==MODE_SPACING_EQUAL ||
                          mode==MODE_SPACING_FRACTIONAL_EVEN ||
                          mode==MODE_SPACING_FRACTIONAL_ODD) {
                    if(out->spacing)goto done;
                    out->spacing=mode;
                } else if(mode==MODE_VERTEX_ORDER_CW || mode==MODE_VERTEX_ORDER_CCW) {
                    if(out->winding)goto done;
                    out->winding=mode;
                } else {
                    if(out->point_mode)goto done;
                    out->point_mode=mode;
                }
            }
        } else if(op==71) {
            if(n<3 || !w[1] || w[1]>=bound)goto done;
            struct id_info *d=&ids[w[1]];
            if(w[2]==30 || w[2]==11) {
                if(n!=4)goto done;
                unsigned *field=w[2]==30?&d->location:&d->builtin;
                if(*field!=~0u)goto done;
                *field=w[3];
            } else if(w[2]==32) { /* Index */
                if(n!=4 || d->index_set || w[3]>1u)goto done;
                d->index=w[3];d->index_set=1;
            } else if(w[2]==14) {
                if(n!=3)goto done;
                d->flat=1;
            } else if(w[2]==DECORATION_PATCH) {
                if(n!=3)goto done;
                d->patch=1;
            } else if(w[2]==13 || w[2]==16 || w[2]==17 || w[2]==31)
                d->forbidden=1;
        } else if(op==43) {
            /* OpConstant: result id in operand 1, literal in operand 2. Only the
             * declared distance array length consumes one. */
            if(n<4 || !w[1] || w[1]>=bound || !w[2] || w[2]>=bound || ids[w[2]].op)goto done;
            struct id_info *d=&ids[w[2]];d->op=op;d->type=w[1];d->count=w[3];
        } else if(op==21 || op==22 || op==23 || op==24 || op==28 || op==30 || op==32 || op==59) {
            unsigned id=op==59?(n>=3?w[2]:0):(n>=2?w[1]:0);
            if(!id || id>=bound || ids[id].op)goto done;
            struct id_info *d=&ids[id];d->op=op;
            if(op==21 || op==22) {
                if(n!=(op==21?4u:3u))goto done;
                d->count=w[2];
                if(op==21) {
                    if(w[3]>1)goto done;
                    d->signedness=w[3];
                }
            } else if(op==23 || op==24 || op==32) {
                if(n!=4)goto done;
                d->type=op==32?w[3]:w[2];
                d->count=w[3];d->storage=w[2];
            } else if(op==28) {
                /* OpTypeArray: element type, then the length constant. */
                if(n!=4)goto done;
                d->type=w[2];d->count=w[3];
            } else if(op==30) {
                d->count=n-2;
                for(unsigned member=0;member<d->count && member<BLOCK_MEMBERS;++member)
                    d->member_types[member]=w[2+member];
            } else {
                if(n<4)goto done;
                d->type=w[1];d->storage=w[3];
            }
        }
        /* Decoration groups/ID decorations require expansion we do not do. */
        if(op==73 || op==74 || op==75 || op==332)goto done;
        at+=n;
    }
    if(entries!=1)goto done;
    unsigned loose_position=0,loose_point_size=0;
    for(unsigned i=1;i<bound;++i) {
        struct id_info *d=&ids[i];
        if(!d->selected)continue;
        if(d->op!=59)goto done;
        if(d->storage!=1 && d->storage!=3)continue;
        if(d->forbidden || !d->type || d->type>=bound)goto done;
        struct id_info *ptr=&ids[d->type];
        if(ptr->op!=32 || ptr->storage!=d->storage || !ptr->type || ptr->type>=bound)goto done;
        struct id_info *type=&ids[ptr->type];
        if(d->builtin!=~0u) {
            /* Built-ins of the tessellation pair. The control stage reads its
             * invocation id and writes the two tessellation level arrays; the
             * evaluation stage reads the patch coordinate. The level arrays are
             * per-patch built-ins, so they carry the Patch decoration as well,
             * and their widths are the ones the tessellator defines (4 outer,
             * 2 inner), which is what a pipeline would have to program. */
            if(model==MODEL_TESS_CTRL && d->builtin==BUILTIN_INVOCATION_ID) {
                if(d->location!=~0u || d->storage!=1 || d->patch || type->op!=21 ||
                   type->count!=32)goto done;
                continue;
            }
            /* The geometry stage's invocation id. A pipeline with more than one
             * invocation per primitive is meaningless without it - an invocation
             * that cannot tell which one it is can only repeat the same work - so
             * this is what the feature's mandatory maxGeometryShaderInvocations
             * minimum needs. It is an input scalar with no location, like the
             * control stage's, and the hardware supplies it to the merged stage
             * the same way it supplies the per-vertex offsets this profile
             * already reads. */
            if(model==MODEL_GEOMETRY && d->builtin==BUILTIN_INVOCATION_ID) {
                if(d->location!=~0u || d->storage!=1 || d->patch || type->op!=21 ||
                   type->count!=32)goto done;
                continue;
            }
            /* Geometry consumes the input primitive index; TCS consumes the
             * input patch index. Both declarations are scalar stage inputs,
             * not Patch-decorated outputs. The fragment-stage PrimitiveId
             * varying is a separate interface and remains refused here. */
            if((model==MODEL_GEOMETRY || model==MODEL_TESS_CTRL || model==MODEL_TESS_EVAL) &&
               d->builtin==BUILTIN_PRIMITIVE_ID) {
                if(d->location!=~0u || d->storage!=1 || d->patch || type->op!=21 ||
                   type->count!=32)goto done;
                continue;
            }
            if((model==MODEL_TESS_CTRL || model==MODEL_TESS_EVAL) &&
               d->builtin==BUILTIN_PATCH_VERTICES) {
                if(d->location!=~0u || d->storage!=1 || d->patch || type->op!=21 ||
                   type->count!=32)goto done;
                continue;
            }
            /* gl_ViewportIndex: the geometry stage's per-primitive selection of
             * one of the viewport banks. Core Vulkan lets only a geometry stage
             * write it, it is a 32-bit integer scalar rather than a varying, and
             * the hardware acts on it through the viewport-index vector the
             * compiler publishes for the merged pair. What this function decides
             * is only that the DECLARATION is one this profile can describe; the
             * capability gate on the enabled multiViewport bit lives in the
             * adapter, because a pipeline that writes an index while the profile
             * programs a single bank would silently route everything to viewport
             * zero. */
            if(model==MODEL_GEOMETRY && d->builtin==BUILTIN_VIEWPORT_INDEX) {
                if(d->location!=~0u || d->storage!=3 || d->patch ||
                   type->op!=21 || type->count!=32 || out->viewport_index)goto done;
                out->viewport_index=1;
                continue;
            }
            if((model==MODEL_TESS_CTRL || model==MODEL_TESS_EVAL) &&
               (d->builtin==BUILTIN_TESS_LEVEL_OUTER || d->builtin==BUILTIN_TESS_LEVEL_INNER)) {
                unsigned length=0,element=0;
                const unsigned want=d->builtin==BUILTIN_TESS_LEVEL_OUTER?4u:2u;
                if(d->location!=~0u || d->storage!=(model==MODEL_TESS_CTRL?3u:1u) || !d->patch ||
                   !declared_array(ids,bound,ptr->type,&length,&element) ||
                   length!=want || !element || element>=bound)goto done;
                if(ids[element].op!=22 || ids[element].count!=32)goto done;
                continue;
            }
            if(model==MODEL_TESS_EVAL && d->builtin==BUILTIN_TESS_COORD) {
                if(d->location!=~0u || d->storage!=1 || d->patch || type->op!=23 ||
                   type->count!=3 || !type->type || type->type>=bound)goto done;
                if(ids[type->type].op!=22 || ids[type->type].count!=32)goto done;
                continue;
            }
            /* gl_ClipDistance (3) and gl_CullDistance (4): a float32 array the
             * pre-raster stage writes and the rasterizer clips or culls
             * against. The declared width must fit the exported distance
             * registers. With a tessellation pair the last pre-raster stage is
             * the control or the evaluation stage, so both of them may declare
             * and write them.
             *
             * The fragment stage declares the same two built-ins as INPUTS: the
             * rasterizer interpolates the packed distance components into the
             * pixel's attribute space exactly like a varying, so a pixel read is
             * the same interface at the other end of the pipeline. It is
             * recorded separately from an export and the pipeline check below
             * requires the last pre-raster stage to export what the fragment
             * stage reads - a read whose producer never wrote it would
             * interpolate against a register the program does not export. */
            if(d->builtin==BUILTIN_CLIP_DISTANCE || d->builtin==BUILTIN_CULL_DISTANCE) {
                unsigned length=0;
                const int preraster=model==MODEL_VERTEX || model==MODEL_GEOMETRY ||
                    model==MODEL_TESS_CTRL || model==MODEL_TESS_EVAL;
                const int fragment=model==MODEL_FRAGMENT;
                if((!preraster && !fragment) || d->patch || d->location!=~0u ||
                   d->storage!=(fragment?1u:3u) ||
                   !declared_distance_array(ids,bound,ptr->type,&length))goto done;
                unsigned *total=fragment?
                    (d->builtin==BUILTIN_CLIP_DISTANCE?
                        &out->clip_distance_reads:&out->cull_distance_reads):
                    (d->builtin==BUILTIN_CLIP_DISTANCE?
                        &out->clip_distances:&out->cull_distances);
                if(*total)goto done; /* one declaration per built-in per stage */
                *total=length;
                continue;
            }
            /* gl_FragCoord: the fragment's window position. Core Vulkan, so
             * no feature gates it, and unlike every varying it needs no export
             * from the pre-raster stage - the hardware launches the pixel wave
             * with the position VGPRs and the pinned compiler asks for them
             * through SPI_PS_INPUT_ENA, which it publishes with the rest of
             * the pixel context registers. Nothing else in the pipeline has to
             * change, so this is an interface rule only.
             *
             * It is accepted in the one shape the built-in has: a fragment
             * Input pointing at a four-component 32-bit float vector, never a
             * patch and never at a location. Refusing it is what kept the only
             * applicable depthClamp leaves out of reach - the fragment shader
             * of dEQP-VK.clipping.clip_volume.depth_clamp.* colours with
             * gl_FragCoord.z, and the pair was refused at pipeline creation
             * (measured as two rc=-8 runtime-graphics cache entries in the
             * 2026-09-20 run, eboot 749756aa). */
            if(d->builtin==BUILTIN_FRAG_COORD) {
                if(model!=MODEL_FRAGMENT || d->storage!=1u || d->patch ||
                   d->location!=~0u || type->op!=23 || type->count!=4 ||
                   !type->type || type->type>=bound)goto done;
                const struct id_info *component=&ids[type->type];
                if(component->op!=22 || component->count!=32)goto done;
                continue;
            }
            /* gl_SampleID is the index of the sample this pixel invocation was
             * launched for (DXVK262-T06). The hardware supplies it when the
             * pipeline shades per sample; a fragment shader that reads it is
             * what the sample-rate witnesses write to their target, so the
             * interface accepts it in its one shape - a fragment Input scalar
             * 32-bit integer, never a patch and never at a location - and
             * leaves every other sample built-in (gl_SamplePosition,
             * gl_SampleMaskIn) outside this profile until their own slice. */
            if(d->builtin==BUILTIN_SAMPLE_ID) {
                if(model!=MODEL_FRAGMENT || d->storage!=1u || d->patch ||
                   d->location!=~0u || type->op!=21 || type->count!=32)goto done;
                continue;
            }
            /* A loose gl_Position / gl_PointSize: the same export as the
             * position block's members, declared as a standalone Output
             * variable instead of a gl_PerVertex member. DXVK 2.6.2's DXBC
             * compiler emits this form (a float32 vec4 Output decorated
             * BuiltIn Position), so a pre-raster stage whose output is not
             * arrayed - vertex, geometry or tessellation evaluation - accepts
             * it under the member rules, once per built-in per stage. The
             * control stage's outputs are per-vertex arrays and keep the
             * block form. */
            if((model==MODEL_VERTEX || model==MODEL_GEOMETRY || model==MODEL_TESS_EVAL) &&
               (d->builtin==BUILTIN_POSITION || d->builtin==BUILTIN_POINT_SIZE)) {
                unsigned *seen=d->builtin==BUILTIN_POSITION?&loose_position:&loose_point_size;
                if(d->location!=~0u || d->storage!=3 || d->patch || *seen)goto done;
                if(d->builtin==BUILTIN_POSITION) {
                    if(type->op!=23 || type->count!=4 || !type->type || type->type>=bound ||
                       ids[type->type].op!=22 || ids[type->type].count!=32)goto done;
                } else if(type->op!=22 || type->count!=32)goto done;
                *seen=1;
                continue;
            }
            /* Any other built-in a tessellation stage declares is outside this
             * profile: the exchange with the tessellator is exactly the set
             * above plus the position block. */
            if(model==MODEL_TESS_CTRL || model==MODEL_TESS_EVAL)goto done;
            /* Vertex-stage scalar built-ins the runtime ABI really delivers:
             * VertexIndex (42) and InstanceIndex (43) come from the geometry
             * path - the compiler lowers the latter as instance id plus the
             * start-instance slot - and BaseVertex (4424), BaseInstance (4425),
             * DrawIndex (4426) and ViewIndex (4440) come from the user-SGPR
             * block the draw emitter fills from the recorded draw. ViewIndex is
             * the multiview one: the compiler declares its slot in metadata v14,
             * and the draw expansion hands each view's index to it. All of them
             * are inputs rather than vertex attributes. Fragment ViewIndex is
             * also delivered through its own declared user-SGPR slot; other
             * fragment built-ins are outside this profile. */
            if(d->location!=~0u || (model!=0 && !(model==4 && d->builtin==4440)) || d->storage!=1 ||
               (d->builtin!=42 && d->builtin!=43 && d->builtin!=4424 &&
                d->builtin!=4425 && d->builtin!=4426 && d->builtin!=4440) ||
               type->op!=21 || type->count!=32)goto done;
            continue;
        }
        /* A per-vertex interface is an array: a geometry stage's inputs, and
         * both tessellation stages' per-vertex inputs and the control stage's
         * per-vertex outputs. For geometry the length is the input primitive's
         * vertex count, which the caller checks against the topology. The front
         * end sizes a tessellation per-vertex array with gl_MaxPatchVertices
         * (32) and a control output array with its output vertex count, so for
         * tessellation the length is a bound rather than the patch size: what
         * binds a pipeline is the control stage's OutputVertices (an execution
         * mode) together with the pipeline's patch control points. The element
         * is what has to match the neighbouring stage in every case. */
        const int per_vertex_array=type->op==28 &&
            ((model==MODEL_GEOMETRY && d->storage==1) ||
             ((model==MODEL_TESS_CTRL ||
               (model==MODEL_TESS_EVAL && d->storage==1)) && !d->patch));
        if(per_vertex_array) {
            unsigned length=0,element=0;
            if(!declared_array(ids,bound,ptr->type,&length,&element) ||
               !element || element>=bound)goto done;
            if(model==MODEL_GEOMETRY) {
                if(!out->input_vertices)out->input_vertices=length;
                else if(out->input_vertices!=length)goto done;
            } else if(length>out->patch_vertices) out->patch_vertices=length;
            struct id_info *component=&ids[element];
            if(component->op==30 && d->location==~0u) {
                /* A per-vertex position block: gl_out is this stage's export, so
                 * the distances it declares are this stage's declarations, and
                 * gl_in carries the neighbouring stage's, validated without
                 * being claimed here. */
                const int exported=d->storage==3;
                if(!builtin_block(m,ids,bound,element,component->count,
                                  exported?&out->clip_distances:NULL,
                                  exported?&out->cull_distances:NULL))goto done;
                continue;
            }
            type=component;
        }
        /* A position block: this stage's export, or the previous stage's, whose
         * members are validated without being claimed as this stage's. */
        if(type->op==30 && d->location==~0u &&
           (model==MODEL_VERTEX || model==MODEL_GEOMETRY ||
            model==MODEL_TESS_CTRL || model==MODEL_TESS_EVAL)) {
            if(d->storage==3) {
                if(!builtin_block(m,ids,bound,ptr->type,type->count,
                                  &out->clip_distances,&out->cull_distances))goto done;
            } else if(!builtin_block(m,ids,bound,ptr->type,type->count,NULL,NULL))goto done;
            continue;
        }
        struct interface_slot occupied[LOCATIONS];
        unsigned occupied_count=0;
        if(!interface_locations(ids,bound,(unsigned)(type-ids),occupied,&occupied_count,0))goto done;
        /* Integer FS inputs are not interpolatable. Flat floats are also
         * valid; their PSBC semantic bit is preserved by the native header.
         * Interpolation decorations need not match VS output decorations. */
        /* Integer outputs cannot be interpolated: both the fragment stage and a
         * geometry stage's per-vertex inputs must be flat for them. */
        if((model==MODEL_FRAGMENT || model==MODEL_GEOMETRY) && d->storage==1 && !d->flat)
            for(unsigned slot=0;slot<occupied_count;++slot)
                if(occupied[slot].numeric!=PS5VK_VERTEX_NUMERIC_FLOAT)goto done;
        /* A Patch-decorated variable is the per-patch interface; it exists only
         * between the control and evaluation stages. */
        if(d->patch && model!=MODEL_TESS_CTRL && model!=MODEL_TESS_EVAL)goto done;
        if(d->patch && model==MODEL_TESS_CTRL && d->storage==1)goto done;
        if(d->location>=LOCATIONS || occupied_count>LOCATIONS-d->location)goto done;
        struct interface_slot *locations;
        if(d->index_set) {
            /* Vulkan's dual-source form is exactly one fragment Output at
             * Location 0, Index 1.  An explicit Index 0 is the primary source;
             * Index on inputs, pre-raster stages, patch variables or a value
             * spanning multiple locations remains outside this profile. */
            if(model!=MODEL_FRAGMENT || d->storage!=3 || d->patch ||
               d->location!=0 || occupied_count!=1)goto done;
            locations=d->index?out->secondary_outputs:out->outputs;
        } else if(d->patch)
            locations=d->storage==1?out->patch_inputs:out->patch_outputs;
        else locations=d->storage==1?out->inputs:out->outputs;
        for(unsigned slot=0;slot<occupied_count;++slot) {
            if(locations[d->location+slot].components)goto done;
            locations[d->location+slot]=occupied[slot];
        }
    }
    /* Each feature has its own floor and the exported registers are shared, so
     * a declaration that fits one bound may still not fit the stage. */
    /* Tessellation modes may reside in either stage. Validate completeness
     * and cross-stage agreement only after resolving the pair below. */
    /* Exports and reads share the two packed distance registers, so both are
     * budgeted against the same per-feature and combined ceilings. A stage
     * declares one built-in once and is either pre-raster or fragment, so the
     * two forms never add up inside one stage - the sum is what keeps the
     * ceiling honest if that ever changes. */
    if(out->clip_distances>PS5VK_MAX_CLIP_DISTANCES ||
       out->cull_distances>PS5VK_MAX_CULL_DISTANCES ||
       out->clip_distance_reads>PS5VK_MAX_CLIP_DISTANCES ||
       out->cull_distance_reads>PS5VK_MAX_CULL_DISTANCES ||
       out->clip_distances+out->cull_distances+
           out->clip_distance_reads+out->cull_distance_reads>
           PS5VK_MAX_COMBINED_CLIP_CULL_DISTANCES)goto done;
    valid=1;
done:
    free(ids);return valid;
}

int ps5vk_spirv_module_uses_extended_gather(
    const struct ps5vk_graphics_module_key *module)
{
    if(!module || !module->words || module->word_count<5 ||
       module->words[0]!=0x07230203u)return -1;
    for(size_t at=5;at<module->word_count;) {
        const uint32_t instruction=module->words[at];
        const size_t words=instruction>>16;
        const uint32_t opcode=instruction&0xffffu;
        if(!words || words>module->word_count-at)return -1;
        if(opcode==17u) { /* OpCapability */
            if(words!=2)return -1;
            if(module->words[at+1]==25u)return 1; /* ImageGatherExtended */
        } else if(opcode==96u || opcode==97u) { /* OpImageGather, OpImageDrefGather */
            /* Both instructions place the optional Image Operands mask after
             * their five required operands. shaderImageGatherExtended governs
             * Offset, ConstOffset, and ConstOffsets even when the front end
             * omits the ImageGatherExtended capability for a constant form. */
            if(words>6u) {
                const uint32_t image_operands=module->words[at+6u];
                if(image_operands & (0x08u | 0x10u | 0x20u))return 1;
            }
        }
        at+=words;
    }
    return 0;
}

int ps5vk_spirv_stage_distance_declarations(const struct ps5vk_graphics_module_key *module,
                                            unsigned *clip_distances,
                                            unsigned *cull_distances)
{
    struct interface stage={0};
    if(clip_distances)*clip_distances=0;
    if(cull_distances)*cull_distances=0;
    /* Distances are a pre-raster export. Every stage that can be the last one
     * before rasterization may declare and write them: the vertex stage, the
     * geometry stage, or either half of a tessellation pair. */
    if(!reflect(module,MODEL_VERTEX,&stage) && !reflect(module,MODEL_GEOMETRY,&stage) &&
       !reflect(module,MODEL_TESS_CTRL,&stage) && !reflect(module,MODEL_TESS_EVAL,&stage))
        return 0;
    if(clip_distances)*clip_distances=stage.clip_distances;
    if(cull_distances)*cull_distances=stage.cull_distances;
    return 1;
}

int ps5vk_spirv_stage_distance_reads(const struct ps5vk_graphics_module_key *module,
                                     unsigned *clip_reads,
                                     unsigned *cull_reads)
{
    struct interface stage={0};
    if(clip_reads)*clip_reads=0;
    if(cull_reads)*cull_reads=0;
    /* The other end of the same built-in: only the fragment stage reads
     * gl_ClipDistance/gl_CullDistance as an input, and the pre-raster stages
     * export them (ps5vk_spirv_stage_distance_declarations above). Reporting
     * the reads is a declaration fact; whether the pixel stage can be handed
     * them is the native metadata adapter's and the pipeline's decision, and a
     * fragment module that is not this profile's fragment interface reports no
     * counts rather than a half-valid pair. */
    if(!reflect(module,MODEL_FRAGMENT,&stage))return 0;
    if(clip_reads)*clip_reads=stage.clip_distance_reads;
    if(cull_reads)*cull_reads=stage.cull_distance_reads;
    return 1;
}

int ps5vk_spirv_fragment_outputs(const struct ps5vk_graphics_module_key *module,
                                 unsigned *primary_mask,int *secondary)
{
    struct interface stage={0};
    if(primary_mask)*primary_mask=0;
    if(secondary)*secondary=0;
    if(!reflect(module,MODEL_FRAGMENT,&stage))return 0;
    unsigned mask=0;
    for(unsigned location=0;location<LOCATIONS;++location)
        if(stage.outputs[location].components)mask|=(1u<<location);
    if(primary_mask)*primary_mask=mask;
    if(secondary)*secondary=stage.secondary_outputs[0].components?1:0;
    return 1;
}

unsigned ps5vk_spirv_tess_output_points(const struct ps5vk_graphics_module_key *module)
{
    struct interface stage={0};
    return reflect(module,MODEL_TESS_CTRL,&stage)?stage.control_points:0;
}

static int resolve_tess_modes(struct interface *control,struct interface *evaluation)
{
    unsigned *a[]={&control->control_points,&control->domain,&control->spacing,&control->winding};
    unsigned *b[]={&evaluation->control_points,&evaluation->domain,&evaluation->spacing,&evaluation->winding};
    for(unsigned i=0;i<4;++i) {
        if(*a[i] && *b[i] && *a[i]!=*b[i])return 0;
        const unsigned resolved=*a[i]?*a[i]:*b[i];
        if(!resolved)return 0;
        *a[i]=*b[i]=resolved;
    }
    control->point_mode=evaluation->point_mode=control->point_mode|evaluation->point_mode;
    return 1;
}

unsigned ps5vk_spirv_tess_pair_output_points(const struct ps5vk_graphics_key *key)
{
    struct interface control={0},evaluation={0};
    return key && reflect(&key->tess_control,MODEL_TESS_CTRL,&control) &&
        reflect(&key->tess_eval,MODEL_TESS_EVAL,&evaluation) &&
        resolve_tess_modes(&control,&evaluation)?control.control_points:0;
}

int ps5vk_spirv_graphics_interface(const struct ps5vk_graphics_key *key)
{
    struct interface vs={0},fs={0},gs={0},tcs={0},tes={0};
    if(!key || !reflect(&key->vertex,MODEL_VERTEX,&vs) ||
       !reflect(&key->fragment,MODEL_FRAGMENT,&fs))return 0;
    const int has_tessellation=ps5vk_graphics_has_tessellation(key);
    const int has_geometry=ps5vk_graphics_has_geometry(key);
    if(has_tessellation) {
        /* patchControlPoints assembles the INPUT patch. OutputVertices declares
         * the independent TCS OUTPUT patch size. Both must be in range, but
         * need not be equal; reflection validates the latter separately. */
        if(!ps5vk_graphics_tessellation_key_valid(key))return 0;
        if(!reflect(&key->tess_control,MODEL_TESS_CTRL,&tcs) ||
           !reflect(&key->tess_eval,MODEL_TESS_EVAL,&tes) ||
           !resolve_tess_modes(&tcs,&tes))return 0;
    }
    if(has_geometry) {
        if(!reflect(&key->geometry,MODEL_GEOMETRY,&gs))return 0;
        /* The geometry stage's per-vertex input array holds the vertices of ONE
         * input primitive, and the pipeline's topology decides how many that is:
         * one for a point, two for a line, three for a triangle. Requiring the
         * declared length to agree with the topology is stronger than naming one
         * topology, and it is what lets the pinned geometry module's points and
         * lines families run: the compiler already accepts merged stages fed by
         * those primitives (measured host-side: es_verts_per_subgroup 29 for
         * points, 56 for lines). Adjacency topologies report zero here and stay
         * refused until the front end is measured to accept their four- and
         * six-vertex inputs. */
        /* With tessellation, PATCH_LIST describes input control points, not
         * the primitives delivered to GS. TES point mode overrides its domain;
         * quads are tessellated to triangles, never GS InputQuads. */
        const unsigned input_vertices=has_tessellation?
            (tes.point_mode?1u:tes.domain==MODE_DOMAIN_ISOLINES?2u:3u):
            ps5vk_topology_input_vertices(key->topology);
        const unsigned input_mode=has_tessellation?
            (tes.point_mode?MODE_INPUT_POINTS:
             tes.domain==MODE_DOMAIN_ISOLINES?MODE_INPUT_LINES:MODE_TRIANGLES):
            ps5vk_topology_input_mode(key->topology);
        /* The stage's declared input primitive is what binds it to the topology
         * the pipeline assembles: a stage that never reads gl_in declares no
         * per-vertex array, so requiring the array's length would refuse a legal
         * stage (the pinned module's vertex_no_op leaf is exactly that), while
         * requiring the execution mode is the fact the hardware and the compiler
         * both act on. A stage that does declare the array must agree with it. */
        if(!input_vertices || !input_mode || gs.input_primitive!=input_mode ||
           (gs.input_vertices && gs.input_vertices!=input_vertices))return 0;
    }
    /* What the fragment stage must export is decided by the attachment it
     * would export into. A colour subpass wants the one four-component float
     * output at location 0 this profile writes. A DEPTH-ONLY subpass has no
     * colour attachment, so the stage must declare no output at all: the
     * pinned upstream depth clamp module ships an empty fragment shader there,
     * and its program exports nothing (SPI_SHADER_COL_FORMAT zero). Requiring
     * an export that has nowhere to go, or accepting one that does, would both
     * be wrong, so the two cases are exclusive. */
    /* Match the fragment output's numeric class to its attachment: UNORM
     * uses float, UINT uses unsigned, and the diagnostic SINT target uses
     * signed lanes. Both attachment locations obey the same rule. */
    const unsigned colour_numeric[PS5VK_MAX_COLOR_ATTACHMENTS] = {
        (unsigned)(key->color_format[0]==VK_FORMAT_R8G8B8A8_SINT &&
            ps5vk_color_target_integer_served(key->color_format[0]) ?
            PS5VK_VERTEX_NUMERIC_SINT :
            (ps5vk_color_target_integer_served(key->color_format[0]) ?
             PS5VK_VERTEX_NUMERIC_UINT : PS5VK_VERTEX_NUMERIC_FLOAT)),
        (unsigned)(key->color_format[1]==VK_FORMAT_R8G8B8A8_SINT &&
            ps5vk_color_target_integer_served(key->color_format[1]) ?
            PS5VK_VERTEX_NUMERIC_SINT :
            (ps5vk_color_target_integer_served(key->color_format[1]) ?
             PS5VK_VERTEX_NUMERIC_UINT : PS5VK_VERTEX_NUMERIC_FLOAT))};
    /* A colour subpass wants the export that belongs to the attachment it
     * writes: the four-component value whose numeric class follows that
     * attachment's format. An attachment the pipeline does not write - its
     * CB_TARGET_MASK field is zero - may legitimately have no export at all,
     * which is what the pinned render-pass module's attachment_write_mask leaf
     * builds when it starts at index 1: target 0's write mask is zero and the
     * fragment stage exports Location 1 alone. Dropping the export of an
     * attachment that IS written, or writing one whose export is absent, is the
     * torn shape and stays refused. A DEPTH-ONLY subpass names no colour
     * attachment, so it requires no export either. */
    if(key->color_format[0]==VK_FORMAT_UNDEFINED) {
        if(fs.outputs[0].components)return 0;
    } else if(fs.outputs[0].components ?
              (fs.outputs[0].components!=4 ||
               fs.outputs[0].numeric!=colour_numeric[0]) :
              key->color_write_mask[0])return 0;
    if(fs.secondary_outputs[0].components &&
       (fs.secondary_outputs[0].components!=4 ||
        fs.secondary_outputs[0].numeric!=PS5VK_VERTEX_NUMERIC_FLOAT))return 0;
    /* The stage the fragment stage reads is the last pre-raster stage that runs
     * before it, and the stage a geometry stage reads is the one before that. */
    const struct interface *previous=has_geometry?&gs:(has_tessellation?&tes:&vs);
    const struct interface *before_geometry=has_tessellation?&tes:&vs;
    /* The pixel stage's distance reads come from the packed position registers
     * the last pre-raster stage exports: a read the producer never wrote would
     * interpolate against a register that stage does not export at all. */
    if(fs.clip_distance_reads>previous->clip_distances ||
       fs.cull_distance_reads>previous->cull_distances)return 0;
    for(unsigned i=0;i<LOCATIONS;++i) {
        unsigned matched=0;
        for(uint32_t a=0;a<key->vertex_attribute_count;++a)
            if(key->vertex_attributes[a].location==i) {
                struct ps5vk_vertex_format format=
                    ps5vk_vertex_format_info(key->vertex_attributes[a].format);
                /* Vulkan component completion/discard permits the attribute
                 * format and shader input to have different component counts.
                 * Their scalar numeric categories must still agree - but only
                 * for an attribute the shader actually consumes: Vulkan lets a
                 * pipeline declare an attribute no input reads (the pinned
                 * geometry module's primitive_id_in leaf does exactly that), and
                 * the fetch path prepares only the spans the compiled vertex
                 * input reads. */
                if(!format.bytes)return 0;
                if(vs.inputs[i].components && format.numeric!=vs.inputs[i].numeric)return 0;
                ++matched;
            }
        /* A shader input must have exactly one matching attribute. The reverse
         * is not a contract: Vulkan lets a pipeline declare a vertex attribute
         * no shader input consumes, and the pinned geometry module's
         * primitive_id_in leaf does exactly that (its vertex shader reads
         * a_position and drops the unused a_color while the pipeline still
         * declares both). The native fetch path prepares the spans the compiled
         * vertex input actually reads, so an unused declaration is dropped
         * rather than fetched against a table nothing names. */
        if(vs.inputs[i].components && matched!=1)return 0;
        /* The fragment stage's primary outputs are bounded to the two MRT
         * locations the pinned compiler's export contract can describe: a
         * whole-location float32 vec4 at Location 0 (required above) and, for
         * the two-MRT shape, the same thing at Location 1. Nothing else is a
         * colour export this profile can classify, and the runtime refuses the
         * two-output pipeline while it serves one colour attachment. */
        if(i>1 && (fs.outputs[i].components || fs.secondary_outputs[i].components))return 0;
        if(i==1 && fs.outputs[i].components &&
           (fs.outputs[i].components!=4 ||
            fs.outputs[i].numeric!=colour_numeric[1]))return 0;
        if(i && fs.secondary_outputs[i].components)return 0;
        if(fs.inputs[i].components &&
           (fs.inputs[i].components!=previous->outputs[i].components ||
            fs.inputs[i].numeric!=previous->outputs[i].numeric))return 0;
        /* A geometry stage's per-vertex inputs must be exactly what the stage
         * before it exported, component for component. */
        if(has_geometry && gs.inputs[i].components &&
           (gs.inputs[i].components!=before_geometry->outputs[i].components ||
            gs.inputs[i].numeric!=before_geometry->outputs[i].numeric))return 0;
        if(has_geometry && before_geometry->outputs[i].components &&
           !gs.inputs[i].components)return 0;
        /* The tessellation chain: the control stage reads the vertex stage's
         * per-vertex outputs, the evaluation stage reads the control stage's,
         * and the per-patch interface crosses the same boundary. The per-vertex
         * array lengths are not compared because a front end sizes them with
         * gl_MaxPatchVertices; the patch count is the execution mode checked
         * above against the pipeline state. */
        if(has_tessellation) {
            /* Interface matching is consumer-directed. In particular, a TCS
             * may use outputs for cross-invocation communication without the
             * TES declaring them. Every declared input still needs a producer.
             * Vulkan Shader Interfaces, "Interface Matching". */
            if(tcs.inputs[i].components &&
               (vs.outputs[i].components!=tcs.inputs[i].components ||
                vs.outputs[i].numeric!=tcs.inputs[i].numeric))return 0;
            if(tes.inputs[i].components &&
               (tcs.outputs[i].components!=tes.inputs[i].components ||
                tcs.outputs[i].numeric!=tes.inputs[i].numeric))return 0;
            if(tes.patch_inputs[i].components &&
               (tcs.patch_outputs[i].components!=tes.patch_inputs[i].components ||
                tcs.patch_outputs[i].numeric!=tes.patch_inputs[i].numeric))return 0;
        }
    }
    return 1;
}

int ps5vk_spirv_stage_viewport_index(const struct ps5vk_graphics_module_key *module)
{
    struct interface stage={0};
    if(!reflect(module,MODEL_GEOMETRY,&stage))return 0;
    return (int)stage.viewport_index;
}
