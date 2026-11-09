#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include "boot_policy.h"
static char mode[12],compositor[12];
static pid_t fork_result;
static int collect_status,wait_errno,executed,waited;
static const char*boot_arg(const char*key){return !strcmp(key,"metal")?mode:!strcmp(key,"camtl")?compositor:NULL;}
static pid_t model_fork(void){executed++;return fork_result;}
static pid_t model_wait4(pid_t pid,int*status,int options,void*usage){
    assert(pid>0 && !options && !usage);waited++;
    if(wait_errno){errno=wait_errno;wait_errno=0;return -1;}
    *status=collect_status;return pid;
}
static int model_execve(const char*path,char*const argv[],char*const env[]){
    assert(!strcmp(path,"/redwood/bin/lancelot_metal_test") && !strcmp(argv[0],path));
    assert(!strcmp(argv[1],!strcmp(compositor,"1")?"--ca":"--basic") && !argv[2] && env[0]);return 0;
}
#define fork model_fork
#define wait4 model_wait4
#define execve model_execve
static void _exit(int status){(void)status;assert(false);}
#include "lancelot_metal_init.inc"
int main(void){
    assert(lancelot_metal_boot_token("metal=1 camtl=1","metal=1"));
    assert(!lancelot_metal_boot_token("metal=10 xmetal=1 camtl=1","metal=1"));
    assert(lancelot_metal_boot_token("\tmetal=1\tcamtl=1 ","camtl=1"));
    assert(!lancelot_metal_boot_token(NULL,"metal=1"));
    strcpy(mode,"0");assert(lancelot_metal_prepare()==1&&!executed&&!waited);
    strcpy(mode,"1");strcpy(compositor,"1");fork_result=4;
    assert(lancelot_metal_prepare()==1&&executed==1&&waited==1);
    collect_status=1<<8;assert(!lancelot_metal_prepare());
    collect_status=9;assert(!lancelot_metal_prepare());
    fork_result=-1;int calls=waited;assert(!lancelot_metal_prepare()&&waited==calls);
    fork_result=4;collect_status=0;wait_errno=4;calls=waited;
    assert(lancelot_metal_prepare()&&waited==calls+2);
    puts("PASS exact Metal boot admission: off/success/failure/signal/fork error/EINTR; no fixed addresses and no substring flag match");
}
