"""Exercise production framebuffer caching with Mesa's actual equality/ref helpers."""
import argparse, hashlib, json, os, subprocess, sys
from pathlib import Path
R=Path(__file__).resolve().parents[1]
if os.name=='nt':
    def linux(s): return '/mnt/'+s[0].lower()+s[2:].replace('\\','/') if len(s)>2 and s[1]==':' else s
    raise SystemExit(subprocess.call(['wsl','-d','Ubuntu','--exec','python3','-B','-u',linux(str(Path(__file__).resolve())),*map(linux,sys.argv[1:])]))
p=argparse.ArgumentParser();p.add_argument('--mesa-source',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
O=a.out.resolve();O.mkdir(parents=True,exist_ok=False)
air=R/'tools/mesa-lancelot/air/air_metal.c';util=a.mesa_source/'src/gallium/auxiliary/util/u_framebuffer.c'
text=air.read_text();mesa=util.read_text()
def body(source,name):
    start=source.index(name+'(');pos=source.index('{',start)+1;depth=1
    while depth: depth+=(source[pos]=='{')-(source[pos]=='}');pos+=1
    return source[start:pos]
helpers='bool '+body(mesa,'util_framebuffer_state_equal')+'\nvoid '+body(mesa,'util_copy_framebuffer_state')
start=text.index('static uint64_t rwa_framebuffer_counters[8];');end=text.index('\n}',text.index('rwa_framebuffer_forget(',start))+2
production=text[start:end]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
struct pipe_surface {unsigned refs,dead;};
struct pipe_resource {unsigned refs,dead;};
struct pipe_framebuffer_state {
 unsigned width,height,samples,layers,nr_cbufs,viewmask;
 struct pipe_surface *cbufs[8],*zsbuf;struct pipe_resource *resolve;
};
static void pipe_surface_reference(struct pipe_surface **p,struct pipe_surface *v) {
 if(*p==v)return;if(v){assert(v->refs&&!v->dead);v->refs++;}
 if(*p){assert((*p)->refs);if(!--(*p)->refs)(*p)->dead=1;}*p=v;
}
static void pipe_resource_reference(struct pipe_resource **p,struct pipe_resource *v) {
 if(*p==v)return;if(v){assert(v->refs&&!v->dead);v->refs++;}
 if(*p){assert((*p)->refs);if(!--(*p)->refs)(*p)->dead=1;}*p=v;
}
'''+helpers+r'''
struct pipe_context {
 void (*flush)(struct pipe_context *,void *,unsigned);
 void (*set_framebuffer_state)(struct pipe_context *,const struct pipe_framebuffer_state *);
 struct pipe_framebuffer_state framebuffer;unsigned sets,flushes,batch;
};
struct rwa_device {struct pipe_context *pipe;struct pipe_framebuffer_state framebuffer;};
static uint64_t ticks;
static uint64_t os_time_get_nano(void){return ++ticks;}
static void set_fb(struct pipe_context *p,const struct pipe_framebuffer_state *f) {
 util_copy_framebuffer_state(&p->framebuffer,f);p->batch=0;p->sets++;
}
static void flush(struct pipe_context *p,void *f,unsigned flags){assert(!f&&!flags);p->flushes++;ticks+=100;}
'''+production+r'''
int main(void) {
 struct pipe_surface a={1,0},b={1,0},c={1,0},z={1,0},other={1,0};struct pipe_resource resolve={1,0};
 struct pipe_context old={.flush=flush,.set_framebuffer_state=set_fb},ctx=old,second=old;
 struct rwa_device d={.pipe=&ctx},e={.pipe=&second};
 struct pipe_framebuffer_state empty={0},fb={.width=16,.height=16,.samples=1,.nr_cbufs=2,.cbufs={&a,&b},.zsbuf=&z};
 for(unsigned i=0;i<1000;i++){old.set_framebuffer_state(&old,&fb);old.set_framebuffer_state(&old,&empty);}
 assert(old.sets==2000&&a.refs==1&&b.refs==1&&z.refs==1);
 rwa_framebuffer_bind(&d,&fb,true);ctx.batch=77;
 for(unsigned i=1;i<1000;i++)rwa_framebuffer_bind(&d,&fb,true);
 assert(ctx.sets==1&&ctx.batch==77&&!ctx.flushes&&rwa_framebuffer_counters[1]==999);
 assert(a.refs==3&&b.refs==3&&z.refs==3);
 /* A second context does not inherit another device's last framebuffer. */
 struct pipe_framebuffer_state f2={.width=8,.height=8,.samples=1,.nr_cbufs=1,.cbufs={&other}};
 rwa_framebuffer_bind(&e,&f2,true);assert(!second.flushes&&second.sets==1);
 /* MRT attachment 1 matters even when color 0 and depth are unchanged. */
 fb.cbufs[1]=&c;rwa_framebuffer_bind(&d,&fb,true);
 assert(ctx.flushes==1&&ctx.sets==2&&b.refs==1&&c.refs==3);
 /* Clear-only callers update the same cache, without forcing a flush. */
 rwa_framebuffer_bind(&d,&f2,false);assert(ctx.flushes==1&&a.refs==1&&c.refs==1&&z.refs==1);
 rwa_framebuffer_bind(&d,&fb,true);assert(ctx.flushes==2&&a.refs==3);
 fb.samples=2;rwa_framebuffer_bind(&d,&fb,true);assert(ctx.flushes==3);
 fb.layers=2;rwa_framebuffer_bind(&d,&fb,true);assert(ctx.flushes==4);
 fb.viewmask=3;rwa_framebuffer_bind(&d,&fb,true);assert(ctx.flushes==5);
 fb.resolve=&resolve;rwa_framebuffer_bind(&d,&fb,true);assert(ctx.flushes==6&&resolve.refs==3);
 unsigned sets=ctx.sets;rwa_framebuffer_forget(&d,&b);assert(ctx.sets==sets);
 rwa_framebuffer_forget(&d,&a);
 assert(!d.framebuffer.nr_cbufs&&!ctx.framebuffer.nr_cbufs&&a.refs==1&&c.refs==1&&z.refs==1&&resolve.refs==1);
 assert(e.framebuffer.cbufs[0]==&other&&other.refs==3&&rwa_framebuffer_counters[6]==1);
 rwa_framebuffer_bind(&e,&empty,false);assert(other.refs==1);
 struct pipe_surface *owner=&a;pipe_surface_reference(&owner,NULL);assert(!owner&&a.dead);
 assert(rwa_framebuffer_counters[4]>0&&rwa_framebuffer_counters[5]>0);
 puts("PASS actual framebuffer cache and Mesa ref/equality helpers: 2000 old binds become 1; batch retained; MRT/clear/context/lifetime/shape boundaries preserved");
}
'''
(O/'framebuffer.c').write_text(code)
runs=[]
for cc in ['clang','gcc']:
    binary=O/cc
    for stage,command in [('build',[cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined',str(O/'framebuffer.c'),'-o',str(binary)]),('run',[str(binary)])]:
        q=subprocess.run(command,capture_output=True);log=O/(cc+'-'+stage+'.log');log.write_bytes(q.stdout+q.stderr)
        runs.append({'compiler':cc,'stage':stage,'exit':q.returncode})
        if q.returncode: print(log.read_text());raise SystemExit(q.returncode)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
(O/'result.json').write_text(json.dumps({'passed':True,'runs':runs,'sources':{air.relative_to(R).as_posix():sha(air),Path(__file__).relative_to(R).as_posix():sha(Path(__file__))},'mesa_utility_source':str(util),'mesa_utility_sha256':sha(util),'hardware_verified':False},indent=2)+'\n')
print('PASS production framebuffer cache with exact Mesa comparison and reference helpers')
