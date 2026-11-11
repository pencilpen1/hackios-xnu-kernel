#include "rwm.h"
#include <assert.h>
#include <stdio.h>
struct rwa_compute{};
struct rwa_texture{};
struct rwm_texture {uint32_t sid; rwa_texture texture;};
static unsigned access_mask[RWM_MAX_BUFFERS];
static unsigned access(const rwa_compute*,unsigned i){return access_mask[i];}
static struct rwm_air air={};
extern "C" const struct rwm_air *rwm_air(){return &air;}
extern "C" void rwm_print(const char*,...){}
extern "C" rwa_texture *rwm_texture_mesa(const rwm_texture *t){return const_cast<rwa_texture*>(&t->texture);}
extern "C" uint32_t rwm_texture_in_place(const rwm_texture *t){return t->sid;}
extern "C" void rwm_work_keep(rwm_work *w,void *o){if(o){assert(w->keep_count<RWM_WORK_KEEP);w->keep[w->keep_count++]=o;}}
int main(){
 air.compute_texture_access=access;rwa_compute compute;int pipeline;
 rwm_compute_state state={};state.compute=&compute;state.pipeline=&pipeline;
 rwm_texture input={17,{}},output={23,{}};
 state.texture_count=2;state.textures[0]={&input,&input};state.textures[1]={&output,&output};
 access_mask[0]=1;access_mask[1]=2;unsigned long grid[]={2,2,1},group[]={8,8,1};rwm_work work;
 assert(rwm_compute_dispatch(&state,grid,group,&work));
 assert(work.copy_in_count==1&&work.copy_in[0]==&input&&work.sampled_count==1&&work.sampled[0]==17);
 assert(work.target_count==1&&work.targets[0]==&output&&work.target_surface_count==1&&work.target_surface[0]==23);
 access_mask[1]=3;assert(rwm_compute_dispatch(&state,grid,group,&work));assert(work.sampled_count==2&&work.copy_in_count==2&&work.target_count==1);
 access_mask[0]=0;access_mask[1]=0;assert(rwm_compute_dispatch(&state,grid,group,&work));assert(!work.sampled_count&&!work.target_count&&!work.copy_in_count);
 puts("PASS compute IOSurface ownership: read-only inputs stay sampled; written outputs receive target/fence tracking; unused textures cause no CPU copy");
}
