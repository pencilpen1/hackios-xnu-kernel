/* Draw-time copied shader identifiers. No GPU pointers retained or data read. */
#ifndef LANCELOT_SHADER_PROFILE_H
#define LANCELOT_SHADER_PROFILE_H
#include <stdint.h>
#define LM_SHADER_PAIRS 4u
struct lm_shader_meta {
    uint32_t id,stage,bytes,regs,tls,ubos,textures,samplers,cache;
    uint64_t compile_us,materialize_us;
    char hash[41];
};
struct lm_shader_pair {uint32_t vs,fs,draws;};
struct lm_shader_work {
    uint64_t scissor_pixels;
    uint32_t full_framebuffer,blend_destination,framebuffer_fetch,forward_kill,side_effects;
};
struct lm_shader_mix {
    struct lm_shader_pair pair[LM_SHADER_PAIRS];uint32_t other;
    struct lm_shader_meta vs[LM_SHADER_PAIRS],fs[LM_SHADER_PAIRS];
    struct lm_shader_work work[LM_SHADER_PAIRS];
};
static inline void lm_shader_mix_record(struct lm_shader_mix *m,uint32_t vs,uint32_t fs) {
    for(unsigned i=0;i<LM_SHADER_PAIRS;i++) {
        struct lm_shader_pair *p=&m->pair[i];
        if(p->draws&&(p->vs!=vs||p->fs!=fs))continue;
        if(!p->draws){p->vs=vs;p->fs=fs;}p->draws++;return;
    }
    m->other++;
}
static inline void lm_shader_mix_record_meta(struct lm_shader_mix *m,
        const struct lm_shader_meta *vs,const struct lm_shader_meta *fs) {
    uint32_t v=vs?vs->id:0,f=fs?fs->id:0;
    lm_shader_mix_record(m,v,f);
    for(unsigned i=0;i<LM_SHADER_PAIRS;i++)if(m->pair[i].draws&&m->pair[i].vs==v&&m->pair[i].fs==f) {
        if(vs)m->vs[i]=*vs;
        if(fs)m->fs[i]=*fs;
        return;
    }
}
/* Current draw's clipped viewport area is an upper bound, not rasterized pixels.
 * Record only pairs already retained by lm_shader_mix_record_meta. */
static inline void lm_shader_mix_record_work(struct lm_shader_mix *m,uint32_t vs,uint32_t fs,
        uint64_t area,uint32_t full,uint32_t blend,uint32_t fetch,uint32_t kill,uint32_t sidefx) {
    for(unsigned i=0;i<LM_SHADER_PAIRS;i++)if(m->pair[i].draws&&m->pair[i].vs==vs&&m->pair[i].fs==fs) {
        struct lm_shader_work *w=&m->work[i];
        w->scissor_pixels+=area;w->full_framebuffer+=full;w->blend_destination+=blend;
        w->framebuffer_fetch+=fetch;w->forward_kill+=kill;w->side_effects+=sidefx;
        return;
    }
}
#endif
