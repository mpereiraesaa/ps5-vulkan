#include "spirv_graphics_interface.h"
#include "graphics_formats.h"
#include <stdlib.h>
#include <string.h>

enum { ID_LIMIT=65536, LOCATIONS=32, BLOCK_MEMBERS=8 };
/* SPIR-V execution models this profile describes. */
enum { MODEL_VERTEX=0, MODEL_FRAGMENT=4 };
/* Built-in decoration ids this profile knows by name. */
enum { BUILTIN_POSITION=0, BUILTIN_POINT_SIZE=1, BUILTIN_CLIP_DISTANCE=3,
       BUILTIN_CULL_DISTANCE=4, BUILTIN_VERTEX_INDEX=42, BUILTIN_INSTANCE_INDEX=43,
       BUILTIN_BASE_VERTEX=4424, BUILTIN_BASE_INSTANCE=4425, BUILTIN_DRAW_INDEX=4426,
       BUILTIN_VIEW_INDEX=4440 };
struct id_info {
    unsigned op, type, count, signedness, storage, location, builtin, forbidden, selected, flat;
    /* OpTypeStruct member type ids, for the bounded built-in block below. */
    unsigned member_types[BLOCK_MEMBERS];
};
struct interface_slot { unsigned components, numeric; };
struct interface {
    struct interface_slot inputs[LOCATIONS], outputs[LOCATIONS];
    /* Declared gl_ClipDistance/gl_CullDistance array lengths, in components.
     * They start at zero and are set at most once per stage. */
    unsigned clip_distances, cull_distances;
};

/* A declared float32 array of exactly `length` elements: the SPIR-V form of
 * gl_ClipDistance/gl_CullDistance, whether the front end writes it as a block
 * member or as a standalone variable. */
static int declared_distance_array(const struct id_info *ids,unsigned bound,
                                   unsigned type,unsigned *length)
{
    if(!type || type>=bound)return 0;
    const struct id_info *array=&ids[type];
    /* OpTypeArray: operand 1 is the element type, operand 2 the length. */
    if(array->op!=28 || !array->type || array->type>=bound || !array->count ||
       array->count>=bound)return 0;
    const struct id_info *element=&ids[array->type];
    const struct id_info *constant=&ids[array->count];
    if(element->op!=22 || element->count!=32)return 0;
    /* Only a literal 32-bit integer length is a declaration this profile can
     * bound; a spec constant or a non-integer length is not. */
    if(constant->op!=43 || !constant->type || constant->type>=bound)return 0;
    const struct id_info *constant_type=&ids[constant->type];
    if(constant_type->op!=21 || constant_type->count!=32)return 0;
    if(!constant->count || constant->count>PS5VK_MAX_CLIP_DISTANCES)return 0;
    *length=constant->count;
    return 1;
}

/* gl_PerVertex may declare unused builtin arrays, so a declaration alone is
 * not usage. What the block may NOT do is declare a member this profile cannot
 * deliver: Position is a float32 vec4, PointSize a float32 scalar, and
 * ClipDistance/CullDistance an array of float32 no wider than the exported
 * distance registers. Effective clip/cull usage is independently gated by the
 * native compiler metadata adapter, which is what the pipeline creation path
 * consults before a device advertises either feature. */
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
                unsigned *total=w[4]==BUILTIN_CLIP_DISTANCE?clip:cull;
                if(*total)return 0;
                *total=length;
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
                size_t first=3+((size_t)(end-name)+1+3)/4;
                for(size_t j=first;j<n;++j) {
                    if(!w[j] || w[j]>=bound || ids[w[j]].selected)goto done;
                    ids[w[j]].selected=1;
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
            } else if(w[2]==14) {
                if(n!=3)goto done;
                d->flat=1;
            } else if(w[2]==13 || w[2]==16 || w[2]==17 ||
                      w[2]==31 || w[2]==32) d->forbidden=1;
        } else if(op==43) {
            /* OpConstant: result id in operand 1, literal in operand 2. Only the
             * declared distance array length consumes one. */
            if(n<4 || !w[1] || w[1]>=bound || !w[2] || w[2]>=bound || ids[w[2]].op)goto done;
            struct id_info *d=&ids[w[2]];d->op=op;d->type=w[1];d->count=w[3];
        } else if(op==21 || op==22 || op==23 || op==28 || op==30 || op==32 || op==59) {
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
            } else if(op==23 || op==32) {
                if(n!=4)goto done;
                d->type=op==23?w[2]:w[3];
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
            /* gl_ClipDistance (3) and gl_CullDistance (4): a float32 array the
             * pre-raster stage writes and the rasterizer clips or culls
             * against. The fragment stage may not declare either, and the
             * declared width must fit the exported distance registers. */
            if(d->builtin==BUILTIN_CLIP_DISTANCE || d->builtin==BUILTIN_CULL_DISTANCE) {
                unsigned length=0;
                if(model!=MODEL_VERTEX || d->storage!=3 || d->location!=~0u ||
                   !declared_distance_array(ids,bound,ptr->type,&length))goto done;
                unsigned *total=d->builtin==BUILTIN_CLIP_DISTANCE?
                    &out->clip_distances:&out->cull_distances;
                if(*total)goto done; /* one declaration per built-in per stage */
                *total=length;
                continue;
            }
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
        if(type->op==30 && model==MODEL_VERTEX && d->storage==3 && d->location==~0u &&
           builtin_block(m,ids,bound,ptr->type,type->count,
                         &out->clip_distances,&out->cull_distances))continue;
        unsigned components=1;
        if(type->op==23) {
            components=type->count;
            if(components<2 || components>4 || !type->type || type->type>=bound)goto done;
            type=&ids[type->type];
        }
        unsigned numeric=PS5VK_VERTEX_NUMERIC_NONE;
        if(type->op==22 && type->count==32)numeric=PS5VK_VERTEX_NUMERIC_FLOAT;
        else if(type->op==21 && type->count==32)
            numeric=type->signedness?PS5VK_VERTEX_NUMERIC_SINT:PS5VK_VERTEX_NUMERIC_UINT;
        else goto done;
        /* Integer FS inputs are not interpolatable. Flat floats are also
         * valid; their PSBC semantic bit is preserved by the native header.
         * Interpolation decorations need not match VS output decorations. */
        if(model==4 && d->storage==1 && numeric!=PS5VK_VERTEX_NUMERIC_FLOAT && !d->flat)
            goto done;
        if(d->location>=LOCATIONS)goto done;
        struct interface_slot *locations=d->storage==1?out->inputs:out->outputs;
        if(locations[d->location].components)goto done;
        locations[d->location]=(struct interface_slot){components,numeric};
    }
    /* Each feature has its own floor and the exported registers are shared, so
     * a declaration that fits one bound may still not fit the stage. */
    if(out->clip_distances>PS5VK_MAX_CLIP_DISTANCES ||
       out->cull_distances>PS5VK_MAX_CULL_DISTANCES ||
       out->clip_distances+out->cull_distances>PS5VK_MAX_COMBINED_CLIP_CULL_DISTANCES)goto done;
    valid=1;
done:
    free(ids);return valid;
}

int ps5vk_spirv_stage_distance_declarations(const struct ps5vk_graphics_module_key *module,
                                            unsigned *clip_distances,
                                            unsigned *cull_distances)
{
    struct interface stage={0};
    if(clip_distances)*clip_distances=0;
    if(cull_distances)*cull_distances=0;
    if(!reflect(module,MODEL_VERTEX,&stage))return 0;
    if(clip_distances)*clip_distances=stage.clip_distances;
    if(cull_distances)*cull_distances=stage.cull_distances;
    return 1;
}

int ps5vk_spirv_graphics_interface(const struct ps5vk_graphics_key *key)
{
    struct interface vs={0},fs={0};
    if(!key || !reflect(&key->vertex,MODEL_VERTEX,&vs) ||
       !reflect(&key->fragment,MODEL_FRAGMENT,&fs))return 0;
    if(fs.outputs[0].components!=4 ||
       fs.outputs[0].numeric!=PS5VK_VERTEX_NUMERIC_FLOAT)return 0;
    for(unsigned i=0;i<LOCATIONS;++i) {
        unsigned matched=0;
        for(uint32_t a=0;a<key->vertex_attribute_count;++a)
            if(key->vertex_attributes[a].location==i) {
                struct ps5vk_vertex_format format=
                    ps5vk_vertex_format_info(key->vertex_attributes[a].format);
                /* Vulkan component completion/discard permits the attribute
                 * format and shader input to have different component counts.
                 * Their scalar numeric categories must still agree. */
                if(!format.bytes || !vs.inputs[i].components ||
                   format.numeric!=vs.inputs[i].numeric)return 0;
                ++matched;
            }
        if((vs.inputs[i].components && matched!=1) ||
           (!vs.inputs[i].components && matched))return 0;
        if(i && fs.outputs[i].components)return 0;
        if(fs.inputs[i].components &&
           (fs.inputs[i].components!=vs.outputs[i].components ||
            fs.inputs[i].numeric!=vs.outputs[i].numeric))return 0;
    }
    return 1;
}
