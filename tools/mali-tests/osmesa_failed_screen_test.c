/* Extracted production OSMesa context-admission code, not an independent reimplementation. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
struct pipe_screen { unsigned tag; };
struct pipe_frontend_screen { struct pipe_screen *screen; };
struct st_context { void *frontend_context; };
struct osmesa_context { struct st_context *st; };
typedef struct osmesa_context *OSMesaContext;
static struct pipe_frontend_screen *manager;
static unsigned api_calls,context_frees;
static bool allocation_fails,api_fails;
static void context_free(OSMesaContext p) {assert(p);context_frees++;free(p);}
#define FREE(p) context_free(p)
static struct pipe_frontend_screen *get_st_manager(void) {return manager;}
static struct st_context *st_api_create_context(struct pipe_frontend_screen *fs,
    int *attribs,int *error,struct st_context *share)
{
    assert(fs&&fs->screen&&attribs&&error&&!share);api_calls++;
    return api_fails?NULL:calloc(1,sizeof(struct st_context));
}
static OSMesaContext make_context(void) {
    OSMesaContext osmesa=allocation_fails?NULL:calloc(1,sizeof(*osmesa));
    if(!osmesa)return NULL;
    int attribs=0,st_error=0;struct st_context *st_shared=NULL;
#include "actual-osmesa-guard.h"
    osmesa->st->frontend_context=osmesa;
    return osmesa;
}
int main(void) {
    struct pipe_screen screen={7};struct pipe_frontend_screen fs={NULL};
    for(unsigned test=0;test<5;test++) {
        manager=test==0?NULL:&fs;fs.screen=test==1?NULL:&screen;
        api_calls=context_frees=0;api_fails=test==2;allocation_fails=test==3;
        struct pipe_frontend_screen *saved=manager;struct pipe_screen *backing=fs.screen;
        OSMesaContext ctx=make_context();
        assert(manager==saved&&fs.screen==backing); /* No software replacement/revival. */
        if(test<4) {
            assert(!ctx);assert(api_calls==(test==2?1u:0u));
            assert(context_frees==(test==3?0u:1u));
        } else {
            assert(ctx&&ctx->st&&ctx->st->frontend_context==ctx&&api_calls==1&&!context_frees);
            free(ctx->st);context_free(ctx);assert(context_frees==1);
        }
    }
    puts("OSMesa production guard: NULL manager/NULL screen/context failure/allocation failure fail closed; exact context cleanup, no ST call on NULL, normal hardware path unchanged PASS");
    return 0;
}
