#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#define GENERIC_PSCI_SMP 1
#include "lancelot_sched.h"
#define BASEPRI_DEFAULT 31
#define BASEPRI_RTQUEUES 97
#define PROCESSOR_IDLE 0
#define PROCESSOR_NULL NULL
typedef struct processor *processor_t;
typedef struct pset *processor_set_t;
typedef struct thread *thread_t;
struct pset {uint64_t cpu_state_map[1],primary_map,recommended_bitmask,pending_AST_URGENT_cpu_mask,pending_AST_PREEMPT_cpu_mask;bool is_SMT;};
struct processor {unsigned cpu_id;processor_set_t processor_set;};
struct thread {int sched_pri;processor_t bound_processor;};
static struct processor cpus[8];
static processor_t processor_array[8];
static uint64_t fast=6,slow=0xf9;
uint64_t lancelot_sched_capacity_mask(bool high){return high?fast:slow;}
static int lsb_first(uint64_t mask){assert(mask);return __builtin_ctzll(mask);}
#include "lancelot_sched_hint.inc"
int main(void){
 struct pset p={.primary_map=255,.recommended_bitmask=255};
 struct thread t={.sched_pri=47};
 for(unsigned i=0;i<8;i++){cpus[i]=(struct processor){i,&p};processor_array[i]=&cpus[i];}
 assert(lsc_capacity_class(0x410fd050,0)==1);
 assert(lsc_capacity_class(0x410fd0a0,0x600)==2);
 assert(lsc_capacity_class(0x410fd0a0,0x700)==2);
 assert(!lsc_capacity_class(0x510fd0a0,0x700));
 assert(!lsc_capacity_class(0x410fd050,0x600));
 assert(!lsc_capacity_class(0x410fd0a0,0x100));
 assert(!lsc_capacity_class(0x410fd0a0,0x701));
 p.cpu_state_map[0]=255;
 assert(lancelot_sched_idle_hint(&p,&cpus[0],&t)==&cpus[1]);
 assert(lancelot_sched_idle_hint(&p,&cpus[2],&t)==&cpus[2]);
 t.sched_pri=4;assert(lancelot_sched_idle_hint(&p,&cpus[1],&t)==&cpus[0]);
 unsigned cases=0;
 for(unsigned idle=0;idle<256;idle++)for(unsigned recommended=0;recommended<256;recommended++)
 for(unsigned priority=0;priority<2;priority++) {
  p.cpu_state_map[0]=idle;p.recommended_bitmask=recommended;t.sched_pri=priority?47:4;
  processor_t choice=lancelot_sched_idle_hint(&p,NULL,&t);
  uint64_t allowed=idle&recommended&(priority?fast:slow);
  assert((choice!=NULL)==(allowed!=0));
  if(choice)assert(allowed&(1u<<choice->cpu_id));
  cases++;
 }
 p.cpu_state_map[0]=255;p.recommended_bitmask=255;t.sched_pri=47;
 p.pending_AST_URGENT_cpu_mask=2;assert(lancelot_sched_idle_hint(&p,NULL,&t)==&cpus[2]);
 p.pending_AST_PREEMPT_cpu_mask=4;assert(lancelot_sched_idle_hint(&p,&cpus[0],&t)==&cpus[0]);
 p.pending_AST_PREEMPT_cpu_mask=p.pending_AST_URGENT_cpu_mask=0;
 t.bound_processor=&cpus[0];assert(lancelot_sched_idle_hint(&p,NULL,&t)==NULL);
 t.bound_processor=NULL;t.sched_pri=97;assert(lancelot_sched_idle_hint(&p,&cpus[0],&t)==&cpus[0]);
 t.sched_pri=47;p.is_SMT=true;assert(lancelot_sched_idle_hint(&p,NULL,&t)==NULL);p.is_SMT=false;
 fast=0;assert(lancelot_sched_idle_hint(&p,NULL,&t)==NULL);
 fast=slow;assert(lancelot_sched_idle_hint(&p,NULL,&t)==NULL);
 fast=0x90;slow=0x6f;assert(lancelot_sched_idle_hint(&p,&cpus[0],&t)==&cpus[4]);
 printf("PASS %u idle/recommended/priority combinations, hardware ID checks, busy fallback, binding/RT/SMT and logical CPU reordering\n",cases);
}
