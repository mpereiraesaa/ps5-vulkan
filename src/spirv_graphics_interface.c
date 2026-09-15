#include "spirv_graphics_interface.h"
#include "graphics_formats.h"
#include <stdlib.h>
#include <string.h>

enum { ID_LIMIT=65536, LOCATIONS=32 };
struct id_info {
    unsigned op, type, count, signedness, storage, location, builtin, forbidden, selected, flat;
};
struct interface_slot { unsigned components, numeric; };
struct interface {
    struct interface_slot inputs[LOCATIONS], outputs[LOCATIONS];
};

/* gl_PerVertex may declare unused builtin arrays. Actual clip/cull/streamout
 * usage is independently rejected by the native compiler metadata adapter. */
static int builtin_block(const struct ps5vk_graphics_module_key *m,unsigned id,unsigned members)
{
    if(!members || members>8)return 0;
    unsigned seen=0;
    for(size_t at=5;at<m->word_count;at+=m->words[at]>>16) {
        const uint32_t *w=m->words+at;unsigned n=w[0]>>16;
        if((w[0]&65535)==72 && n>=4 && w[1]==id) {
            if(n!=5 || w[3]!=11 || w[2]>=members || (seen&(1u<<w[2])))return 0;
            if(w[4]!=0 && w[4]!=1 && w[4]!=3 && w[4]!=4)return 0;
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
        } else if(op==21 || op==22 || op==23 || op==30 || op==32 || op==59) {
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
            } else if(op==30) d->count=n-2;
            else {
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
            /* Vertex-stage scalar built-ins the runtime ABI really delivers:
             * VertexIndex (42) and InstanceIndex (43) come from the geometry
             * path - the compiler lowers the latter as instance id plus the
             * start-instance slot - and BaseVertex (4424), BaseInstance (4425)
             * and DrawIndex (4426) come from the user-SGPR block the draw
             * emitter fills from the recorded draw. They are not vertex
             * attributes, so they never enter the input map, and anything else
             * stays refused. */
            if(d->location!=~0u || model!=0 || d->storage!=1 ||
               (d->builtin!=42 && d->builtin!=43 && d->builtin!=4424 &&
                d->builtin!=4425 && d->builtin!=4426) ||
               type->op!=21 || type->count!=32)goto done;
            continue;
        }
        if(type->op==30 && model==0 && d->storage==3 && d->location==~0u &&
           builtin_block(m,ptr->type,type->count))continue;
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
    valid=1;
done:
    free(ids);return valid;
}

int ps5vk_spirv_graphics_interface(const struct ps5vk_graphics_key *key)
{
    struct interface vs={0},fs={0};
    if(!key || !reflect(&key->vertex,0,&vs) || !reflect(&key->fragment,4,&fs))return 0;
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
