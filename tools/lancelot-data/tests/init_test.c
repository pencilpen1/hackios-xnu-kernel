/* Real persistent init helpers with Darwin syscalls/filesystem modeled. */
#define errno h_errno
#define stat h_stat
#define lstat h_lstat
#define mount h_mount
#define chmod h_chmod
#define chown h_chown
#define mkdir h_mkdir
#define symlink h_symlink
#define open h_open
#define close h_close
#define fsync h_fsync
#define pread h_pread
#define pwrite h_pwrite
#define rename h_rename
#define sysctlbyname h_sysctl
#define mini_syscall h_syscall
#include "mini.h"
#include <assert.h>
int h_errno;
static uint8_t persistent_file[4096],cached_file[4096],backup_file[4096],persistent_backup[4096];
static int probe_exists,backup_exists,kind=S_IFREG,sync_error,readonly_fs,fullsync_error,no_media_commit,rename_error;
static const char *seed_path="/private/var/mobile/Library/Preferences/test.plist",*seed_source="/template/test.plist";
static const uint8_t default_plist[]="bplist00default-preference";
static uint8_t seeded_file[128],seeded_backup[128];
static off_t seeded_size=sizeof(default_plist)-1,seeded_backup_size;
static int seeded_exists=1,seeded_backup_exists,seed_read_error,template_corrupt,seed_sync_calls;
static const char *archive_paths[]={"/private/var/db/diagnostics","/private/var/db/uuidtext","/private/var/log","/private/var/logs"};
static unsigned ramfs_mount_mask,ramfs_mount_calls;static int ramfs_mount_failure,ramfs_bad_stat;
static uint64_t writes,flushes;
static const char*boot_arg(const char*n){assert(!strcmp(n,"lancelot_data"));return "1";}
#include "lancelot_data_init.inc"
long h_syscall(long number,long path,long out,long c,long d,long e,long f){
 (void)c;(void)d;(void)e;(void)f;
 if(number==92){
  assert(path==3&&out==51);if(fullsync_error){h_errno=5;return -1;}
  if(!no_media_commit){memcpy(persistent_file,cached_file,sizeof(persistent_file));
   memcpy(persistent_backup,backup_file,sizeof(persistent_backup));writes+=8;flushes++;}
  return 0;
 }
 assert(number==345);
 struct lancelot_data_statfs*s=(void*)out;memset(s,0,sizeof(*s));s->bsize=4096;s->blocks=16384;s->bavail=15000;
 if(strcmp((const char*)path,"/private/var")){
  unsigned i;for(i=0;i<4;i++)if(!strcmp((const char*)path,archive_paths[i]))break;
  assert(i<4&&(ramfs_mount_mask&(1u<<i)));strcpy(s->fstype,ramfs_bad_stat?"hfs":"ramfs");strcpy(s->on,(const char*)path);return 0;
 }
 s->flags=readonly_fs?MNT_RDONLY:0;strcpy(s->fstype,"hfs");strcpy(s->from,"/dev/lancelot_data");return 0;
}
int h_sysctl(const char*n,void*out,size_t*size,const void*newp,size_t newlen){
 assert(!newp&&!newlen&&*size==8);uint64_t v;
 if(!strcmp(n,"kern.redwood.lancelot_data_bytes"))v=UINT64_C(67108864);
 else if(!strcmp(n,"kern.redwood.lancelot_data_writes"))v=writes;
 else{assert(!strcmp(n,"kern.redwood.lancelot_data_flushes"));v=flushes;}
 memcpy(out,&v,8);return 0;
}
int h_mount(const char*type,const char*path,int flags,void*data){
 if(!strcmp(type,"ramfs")){
  assert(!flags&&!data);unsigned i;for(i=0;i<4;i++)if(!strcmp(path,archive_paths[i]))break;
  assert(i<4);ramfs_mount_calls++;if(ramfs_mount_failure){h_errno=5;return -1;}
  ramfs_mount_mask|=1u<<i;return 0;
 }
 assert(!strcmp(type,"hfs")&&!strcmp(path,"/private/var")&&!flags);
 const struct lancelot_hfs_args*a=data;assert(!strcmp(a->fspec,"/dev/lancelot_data")&&a->flags==4&&!a->journal_disable);return 0;
}
int h_chmod(const char*p,int mode){assert(mode==0755);if(!strcmp(p,"/private/var"))return 0;
 unsigned i;for(i=0;i<4;i++)if(!strcmp(p,archive_paths[i]))break;assert(i<4);return 0;}
int h_chown(const char*p,int uid,int gid){if(!strcmp(p,"/private/var")){assert(!uid&&!gid);return 0;}
 unsigned i;for(i=0;i<4;i++)if(!strcmp(p,archive_paths[i]))break;assert(i<4&&uid==(i<2?272:0)&&gid==uid);return 0;}
int h_lstat(const char*p,struct h_stat*s){
 memset(s,0,sizeof(*s));
 if(!strcmp(p,"/private/var/db")){s->st_mode=S_IFDIR;return 0;}
 for(unsigned i=0;i<4;i++)if(!strcmp(p,archive_paths[i])){s->st_mode=S_IFDIR;return 0;}
 if(!strcmp(p,seed_path)){
  if(!seeded_exists){h_errno=2;return -1;}s->st_mode=S_IFREG;s->st_size=seeded_size;return 0;
 }
 if(!strcmp(p,seed_source)){s->st_mode=S_IFREG;s->st_size=sizeof(default_plist)-1;return 0;}
 if(!strcmp(p,"/private/var/mobile/Library/Preferences/test.plist.invalid-p12r2")){
  if(!seeded_backup_exists){h_errno=2;return -1;}s->st_mode=S_IFREG;s->st_size=seeded_backup_size;return 0;
 }
 if(!strcmp(p,"/private/var/db/lancelot-data-probe")) {
  if(!probe_exists){h_errno=2;return -1;}s->st_mode=S_IFREG;s->st_size=4096;return 0;
 }
 if(!strcmp(p,"/private/var/db/lancelot-data-probe.invalid-p12r2")) {
  if(!backup_exists){h_errno=2;return -1;}s->st_mode=S_IFREG;s->st_size=4096;return 0;
 }
 if(!strcmp(p,"/missing")){h_errno=2;return -1;}s->st_mode=(uint16_t)kind;return 0;
}
int h_mkdir(const char*p,int mode){(void)p;(void)mode;h_errno=17;return -1;}
int h_symlink(const char*a,const char*b){(void)a;(void)b;h_errno=17;return -1;}
int h_open(const char*p,int flags,int mode){
 if(!strcmp(p,seed_path)){assert(seeded_exists);return 5;}
 if(!strcmp(p,seed_source)){assert(!flags&&!mode);return 6;}
 if(!strcmp(p,"/private/var/db")){assert((flags&O_DIRECTORY)&&!mode);return 4;}
 assert(!strcmp(p,"/private/var/db/lancelot-data-probe"));
 if(flags&O_CREAT){assert(!probe_exists&&(flags&O_EXCL)&&mode==0600);probe_exists=1;memset(cached_file,0,sizeof(cached_file));}
 assert(probe_exists);return 3;
}
int h_close(int fd){assert(fd==3||fd==4||fd==5||fd==6);return 0;}
int h_rename(const char*from,const char*to){
 if(!strcmp(from,seed_path)){
  assert(!strcmp(to,"/private/var/mobile/Library/Preferences/test.plist.invalid-p12r2")&&seeded_exists&&!seeded_backup_exists);
  if(rename_error){h_errno=5;return -1;}
  memcpy(seeded_backup,seeded_file,(size_t)seeded_size);seeded_backup_size=seeded_size;seeded_backup_exists=1;seeded_exists=0;return 0;
 }
 assert(!strcmp(from,"/private/var/db/lancelot-data-probe")&&
  !strcmp(to,"/private/var/db/lancelot-data-probe.invalid-p12r2")&&probe_exists&&!backup_exists);
 if(rename_error){h_errno=5;return -1;}
 memcpy(backup_file,cached_file,sizeof(backup_file));backup_exists=1;probe_exists=0;return 0;
}
int h_fsync(int fd){assert(fd==3||fd==4||fd==5);if(sync_error){h_errno=5;return -1;}if(fd==5)seed_sync_calls++;return 0;}
ssize_t h_pread(int fd,void*out,size_t n,off_t at){
 assert(!at);
 if(fd==5){assert(n<sizeof(seeded_file));if(seed_read_error){h_errno=5;return -1;}memcpy(out,seeded_file,n);return (ssize_t)n;}
 if(fd==6){assert(n==sizeof(default_plist)-1);memcpy(out,default_plist,n);if(template_corrupt)((uint8_t*)out)[0]='?';return (ssize_t)n;}
 assert(fd==3&&n==4096);memcpy(out,cached_file,n);return (ssize_t)n;
}
ssize_t h_pwrite(int fd,const void*in,size_t n,off_t at){assert(fd==3&&n==4096&&!at);memcpy(cached_file,in,n);return (ssize_t)n;}
int main(int argc,char**argv,char**envp){
 (void)argc;(void)argv;(void)envp;
 assert(lancelot_mount_data()&&lancelot_data_persistent&&lancelot_data_capacity==(64u<<20));
#ifndef LANCELOT_OLD_NO_DIAG
 assert(lancelot_data_prepare_log_cache()&&ramfs_mount_mask==15&&ramfs_mount_calls==4&&lancelot_data_persistent);
 ramfs_mount_failure=1;assert(!lancelot_data_prepare_log_cache());ramfs_mount_failure=0;
 ramfs_bad_stat=1;assert(!lancelot_data_prepare_log_cache());ramfs_bad_stat=0;
 lancelot_data_persistent=0;unsigned prior_mounts=ramfs_mount_calls;assert(lancelot_data_prepare_log_cache()&&ramfs_mount_calls==prior_mounts);lancelot_data_persistent=1;
#endif
 assert(lancelot_data_probe());uint64_t boots;memcpy(&boots,persistent_file+16,8);assert(boots==1);
 lancelot_data_persistent=0;writes=flushes=0;assert(lancelot_mount_data()&&lancelot_data_probe());
 memcpy(&boots,persistent_file+16,8);assert(boots==2);
 memcpy(cached_file,persistent_file,sizeof(cached_file));
 unsigned before=(unsigned)writes;cached_file[0]^=1;uint8_t invalid_record[4096];memcpy(invalid_record,cached_file,4096);
 rename_error=1;assert(!lancelot_data_probe()&&writes==before&&probe_exists&&!backup_exists);rename_error=0;
 assert(lancelot_data_probe()&&backup_exists&&!memcmp(backup_file,invalid_record,4096)&&!memcmp(persistent_backup,invalid_record,4096));
 memcpy(&boots,persistent_file+16,8);assert(boots==1);before=(unsigned)writes;
 cached_file[100]^=1;assert(!lancelot_data_probe()&&writes==before&&!memcmp(backup_file,invalid_record,4096));cached_file[100]^=1;
 sync_error=1;assert(!lancelot_data_probe()&&writes==before);sync_error=0;
 memcpy(cached_file,persistent_file,sizeof(cached_file));fullsync_error=1;
 assert(!lancelot_data_probe()&&writes==before);fullsync_error=0;
 memcpy(cached_file,persistent_file,sizeof(cached_file));no_media_commit=1;
 assert(!lancelot_data_probe()&&writes==before);no_media_commit=0;
#ifndef LANCELOT_OLD_NO_DIAG
 memcpy(seeded_file,default_plist,sizeof(default_plist)-1);seeded_file[9]='X';
 assert(lancelot_data_seed_template(seed_path,seed_source)==0&&seeded_exists&&!seeded_backup_exists&&seeded_file[9]=='X');
 memset(seeded_file,0,sizeof(seeded_file));seed_read_error=1;
 assert(lancelot_data_seed_template(seed_path,seed_source)==-1&&!seeded_backup_exists&&seeded_exists);seed_read_error=0;
 template_corrupt=1;assert(lancelot_data_seed_template(seed_path,seed_source)==-1&&!seeded_backup_exists);template_corrupt=0;
 rename_error=1;assert(lancelot_data_seed_template(seed_path,seed_source)==-1&&!seeded_backup_exists&&seeded_exists);rename_error=0;
 assert(lancelot_data_seed_template(seed_path,seed_source)==1&&seeded_backup_exists&&!seeded_exists&&seeded_backup_size==seeded_size);
 for(off_t i=0;i<seeded_backup_size;i++)assert(!seeded_backup[i]);
 seeded_exists=1;memcpy(seeded_file,default_plist,sizeof(default_plist)-1);
 assert(lancelot_data_sync_seed(seed_path,seed_source)&&seed_sync_calls==1);
 seeded_file[1]^=1;assert(!lancelot_data_sync_seed(seed_path,seed_source));seeded_file[1]^=1;
 sync_error=1;assert(!lancelot_data_sync_seed(seed_path,seed_source));sync_error=0;
 memset(seeded_file,0,sizeof(seeded_file));assert(lancelot_data_seed_template(seed_path,seed_source)==-1&&seeded_exists&&seeded_backup_exists);
#endif
 assert(lancelot_data_seed_file("/missing")==1&&lancelot_data_seed_file("/existing")==0);
 kind=S_IFDIR;assert(lancelot_data_directory("/existing",0755)&&lancelot_data_seed_file("/existing")==-1);
 kind=S_IFLNK;assert(lancelot_data_link("target","/existing")&&!lancelot_data_directory("/existing",0755));
 readonly_fs=1;lancelot_data_persistent=0;assert(!lancelot_mount_data()&&!lancelot_data_persistent);
 puts("PASS actual persistent init: HFS/statfs64 ABI, exact volatile archive submounts, data remains HFS, mount/validation failure, two modeled boots, invalid probe/plist quarantined intact, user changes retained, seed sync/readback, generation reset, existing evidence never replaced, rename/fsync/full-sync failure and false cached durability rejected");return 0;
}
