/* Persistent /private/var for the Lancelot static init. Uses the exact Darwin
 * HFS mount/statfs64 ABIs; libmini and its raw syscalls remain unchanged. */
struct lancelot_hfs_args {
    char *fspec;
    uint32_t uid,gid;
    uint16_t mask,padding;
    uint32_t encoding;
    int32_t timezone_minutes,timezone_dst,flags,journal_tbuffer_size,journal_flags,journal_disable;
};
struct lancelot_data_statfs {
    uint32_t bsize;int32_t iosize;
    uint64_t blocks,bfree,bavail,files,ffree;
    int32_t fsid[2];uint32_t owner,type,flags,subtype;
    char fstype[16],on[1024],from[1024];uint32_t flags_ext,reserved[7];
};
_Static_assert(sizeof(struct lancelot_hfs_args)==48,"Darwin 64-bit HFS mount ABI");
_Static_assert(sizeof(struct lancelot_data_statfs)==2168,"Darwin statfs64 ABI");
static int lancelot_data_persistent;
static uint64_t lancelot_data_capacity;
static int lancelot_data_stat(struct lancelot_data_statfs *out) {
    memset(out,0,sizeof(*out));
    return (int)mini_syscall(345,(long)"/private/var",(long)out,0,0,0,0);
}
static uint64_t lancelot_data_counter(const char *name) {
    uint64_t value=0;size_t size=sizeof(value);
    return sysctlbyname(name,&value,&size,NULL,0)==0&&size==sizeof(value)?value:0;
}
static int lancelot_mount_data(void) {
    const char *wanted=boot_arg("lancelot_data");
    if(!wanted||strcmp(wanted,"1"))return mount("ramfs","/private/var",0,NULL)==0;
    struct lancelot_hfs_args args={.fspec="/dev/lancelot_data",.uid=99,.gid=99,.mask=0755,.flags=4};
    if(mount("hfs","/private/var",0,&args)!=0) {
        printf("lancelot-data-mount: persistent=0 error=%d required=1\n",errno);return 0;
    }
    struct lancelot_data_statfs info;
    uint64_t expected=lancelot_data_counter("kern.redwood.lancelot_data_bytes");
    int ok=lancelot_data_stat(&info)==0&&info.bsize&&!(info.flags&MNT_RDONLY)&&
        strcmp(info.fstype,"hfs")==0&&strcmp(info.from,"/dev/lancelot_data")==0&&
        info.blocks<=UINT64_MAX/info.bsize&&info.bavail<=info.blocks&&expected>=UINT64_C(67108864)&&
        info.blocks*info.bsize==expected&&chmod("/private/var",0755)==0&&chown("/private/var",0,0)==0;
    if(!ok){printf("lancelot-data-mount: validation_failed=1 error=%d expected=%llu\n",errno,(unsigned long long)expected);return 0;}
    lancelot_data_persistent=1;lancelot_data_capacity=expected;
    printf("lancelot-data-mount: persistent=1 filesystem=hfs case_sensitive=1 device_bytes=%llu total_bytes=%llu available_bytes=%llu\n",
        (unsigned long long)expected,(unsigned long long)(info.blocks*info.bsize),(unsigned long long)(info.bavail*info.bsize));
    return 1;
}
/* 1: seed missing file, 0: preserve existing regular file, -1: invalid path. */
static int lancelot_data_seed_file(const char *path) {
    if(!lancelot_data_persistent)return 1;
    struct stat st;
    if(lstat(path,&st)==0)return (st.st_mode&S_IFMT)==S_IFREG?0:-1;
    return errno==2?1:-1;
}
static int lancelot_data_seed_error(const char *path,const char *stage) {
    printf("lancelot-data-seed-error: path=%s stage=%s errno=%d\n",path,stage,errno);
    return -1;
}
/* The eight small default preference plists are the only nonempty files in
 * this image's var skeleton. A fully zero file cannot be a valid plist. Retain
 * all nonzero content, including user changes, and keep each invalid original
 * under an absent reserved name before reseeding it from the packaged source. */
static int lancelot_data_seed_template(const char *path,const char *source) {
    int seed=lancelot_data_seed_file(path);if(seed||!lancelot_data_persistent)return seed;
    const char *prefix="/private/var/mobile/Library/Preferences/";
    size_t length=strlen(path);
    if(strncmp(path,prefix,strlen(prefix))||length<6||strcmp(path+length-6,".plist"))return 0;
    struct stat current,expected;
    if(lstat(path,&current))return lancelot_data_seed_error(path,"stat_current");
    if(current.st_size<0||current.st_size>4096)return 0;
    static uint8_t content[4096],template[4096];
    int fd=open(path,O_RDONLY,0);if(fd<0)return lancelot_data_seed_error(path,"open_current");
    ssize_t got=pread(fd,content,(size_t)current.st_size,0);int read_error=got<0?errno:0;
    int closed=close(fd);int close_error=closed?errno:0;
    if(got!=current.st_size){errno=read_error;return lancelot_data_seed_error(path,"read_current");}
    if(closed){errno=close_error;return lancelot_data_seed_error(path,"close_current");}
    for(off_t i=0;i<current.st_size;i++)if(content[i])return 0;
    if(lstat(source,&expected)||(expected.st_mode&S_IFMT)!=S_IFREG||expected.st_size<8||expected.st_size>4096)
        return lancelot_data_seed_error(path,"stat_template");
    fd=open(source,O_RDONLY,0);if(fd<0)return lancelot_data_seed_error(path,"open_template");
    got=pread(fd,template,(size_t)expected.st_size,0);read_error=got<0?errno:0;
    closed=close(fd);close_error=closed?errno:0;
    if(got!=expected.st_size){errno=read_error;return lancelot_data_seed_error(path,"read_template");}
    if(closed){errno=close_error;return lancelot_data_seed_error(path,"close_template");}
    if(memcmp(template,"bplist00",8)&&memcmp(template,"<?xml",5)) {
        errno=22;return lancelot_data_seed_error(path,"template_plist_header");
    }
    static char quarantine[1200];const char *suffix=".invalid-p12r2";
    if(length+strlen(suffix)>=sizeof(quarantine)){errno=63;return lancelot_data_seed_error(path,"quarantine_path");}
    memcpy(quarantine,path,length);strcpy(quarantine+length,suffix);
    struct stat saved;
    if(lstat(quarantine,&saved)==0){errno=17;return lancelot_data_seed_error(path,"quarantine_exists");}
    if(errno!=2)return lancelot_data_seed_error(path,"quarantine_stat");
    if(rename(path,quarantine))return lancelot_data_seed_error(path,"quarantine_rename");
    printf("lancelot-data-seed-recovery: path=%s invalid_record_retained=%s original_bytes=%lld template_bytes=%lld\n",
        path,quarantine,(long long)current.st_size,(long long)expected.st_size);
    return 1;
}
static int lancelot_data_sync_seed(const char *path,const char *source) {
    if(!lancelot_data_persistent)return 1;
    int fd=open(path,O_RDWR,0);if(fd<0){lancelot_data_seed_error(path,"sync_open");return 0;}
    int result=fsync(fd),error=result?errno:0;
    int closed=close(fd),close_error=closed?errno:0;
    if(result){errno=error;lancelot_data_seed_error(path,"file_fsync");return 0;}
    if(closed){errno=close_error;lancelot_data_seed_error(path,"sync_close");return 0;}
    struct stat st,current;
    if(lstat(source,&st)||st.st_size<0||st.st_size>4096||lstat(path,&current)||current.st_size!=st.st_size){lancelot_data_seed_error(path,"readback_size");return 0;}
    static uint8_t expected[4096],actual[4096];
    fd=open(source,O_RDONLY,0);if(fd<0){lancelot_data_seed_error(path,"readback_template_open");return 0;}
    ssize_t got=pread(fd,expected,(size_t)st.st_size,0);error=got<0?errno:0;
    closed=close(fd);close_error=closed?errno:0;
    if(got!=st.st_size||closed){errno=got!=st.st_size?error:close_error;lancelot_data_seed_error(path,"readback_template");return 0;}
    fd=open(path,O_RDONLY,0);if(fd<0){lancelot_data_seed_error(path,"readback_open");return 0;}
    got=pread(fd,actual,(size_t)st.st_size,0);error=got<0?errno:0;
    closed=close(fd);close_error=closed?errno:0;
    if(got!=st.st_size||closed){errno=got!=st.st_size?error:close_error;lancelot_data_seed_error(path,"readback");return 0;}
    if(memcmp(expected,actual,(size_t)st.st_size)){errno=5;lancelot_data_seed_error(path,"readback_content");return 0;}
    return 1;
}
static int lancelot_data_directory(const char *path,int mode) {
    if(mkdir(path,mode)==0)return 1;
    struct stat st;
    return lancelot_data_persistent&&errno==17&&lstat(path,&st)==0&&(st.st_mode&S_IFMT)==S_IFDIR;
}
/* Match the public MTK var_store policy for rebuilt log archives. These four
 * submounts retain the persistent /private/var device and all application data.
 * Existing HFS archive contents remain underneath the temporary mount. */
static int lancelot_data_prepare_log_cache(void) {
    if(!lancelot_data_persistent)return 1;
    static const struct {const char *path;int uid,gid;} paths[]={
        {"/private/var/db/diagnostics",272,272},{"/private/var/db/uuidtext",272,272},
        {"/private/var/log",0,0},{"/private/var/logs",0,0}
    };
    if(!lancelot_data_directory("/private/var/db",0755)) {
        printf("lancelot-data-log-cache: ready=0 stage=parent errno=%d\n",errno);return 0;
    }
    for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);i++) {
        const char *path=paths[i].path;
        if(!lancelot_data_directory(path,0755)||mount("ramfs",path,0,NULL)||
           chmod(path,0755)||chown(path,paths[i].uid,paths[i].gid)) {
            printf("lancelot-data-log-cache: ready=0 path=%s stage=mount errno=%d\n",path,errno);return 0;
        }
        struct lancelot_data_statfs info;memset(&info,0,sizeof(info));
        if(mini_syscall(345,(long)path,(long)&info,0,0,0,0)||strcmp(info.fstype,"ramfs")||
           strcmp(info.on,path)||(info.flags&MNT_RDONLY)) {
            printf("lancelot-data-log-cache: ready=0 path=%s stage=validation errno=%d\n",path,errno);return 0;
        }
    }
    printf("lancelot-data-log-cache: ready=1 filesystem=ramfs paths=4 persistent_data=1 archives_rebuilt=1\n");
    return 1;
}
static int lancelot_data_link(const char *from,const char *to) {
    if(symlink(from,to)==0)return 1;
    struct stat st;
    return lancelot_data_persistent&&errno==17&&lstat(to,&st)==0&&(st.st_mode&S_IFMT)==S_IFLNK;
}
static int lancelot_data_probe_error(const char *stage,long result,int error) {
    printf("lancelot-data-probe-error: stage=%s result=%ld errno=%d\n",stage,result,error);
    return 0;
}
/* A fixed record is synchronized, closed and reopened. On the next real boot,
 * prior_valid/prior_boots prove persistence rather than same-boot caching. */
static int lancelot_data_probe(void) {
    if(!lancelot_data_persistent)return 1;
    static uint8_t record[4096],readback[4096];
    const char *path="/private/var/db/lancelot-data-probe";struct stat st;
    uint64_t prior_boots=0,prior_bytes=0;int prior_valid=0,fd;
    int exists=lstat(path,&st)==0;
    if(!exists&&errno!=2)return lancelot_data_probe_error("stat",-1,errno);
    if(exists) {
        if((st.st_mode&S_IFMT)!=S_IFREG||st.st_size!=(off_t)sizeof(record))return lancelot_data_probe_error("prior_type_size",st.st_size,0);
        fd=open(path,O_RDONLY,0);if(fd<0)return lancelot_data_probe_error("prior_open",fd,errno);
        ssize_t read_result=pread(fd,record,sizeof(record),0);int read_error=read_result<0?errno:0;
        int close_result=close(fd);int close_error=close_result<0?errno:0;
        if(read_result!=(ssize_t)sizeof(record))return lancelot_data_probe_error("prior_read",read_result,read_error);
        if(close_result)return lancelot_data_probe_error("prior_close",close_result,close_error);
        const char *invalid_stage=memcmp(record,"LHDPROB1",8)?"prior_magic":NULL;long invalid_detail=0;
        memcpy(&prior_boots,record+16,8);memcpy(&prior_bytes,record+24,8);
        for(unsigned i=32;!invalid_stage&&i<sizeof(record);i++)if(record[i]!=(uint8_t)(i*37u+11u)) {
            invalid_stage="prior_content";invalid_detail=i;
        }
        if(!invalid_stage&&(!prior_boots||prior_boots==UINT64_MAX||prior_bytes!=lancelot_data_capacity))invalid_stage="prior_generation";
        if(invalid_stage) {
            /* Only our fixed-size regular probe is eligible. Init runs before
             * launchd; the reserved quarantine name must be absent, so old
             * evidence is never replaced. A repaired probe starts a new
             * generation and still needs a second real boot for persistence. */
            const char *quarantine="/private/var/db/lancelot-data-probe.invalid-p12r2";struct stat saved;
            lancelot_data_probe_error(invalid_stage,invalid_detail,0);
            if(lstat(quarantine,&saved)==0)return lancelot_data_probe_error("quarantine_exists",0,17);
            if(errno!=2)return lancelot_data_probe_error("quarantine_stat",-1,errno);
            int result=rename(path,quarantine);
            if(result)return lancelot_data_probe_error("quarantine_rename",result,errno);
            printf("lancelot-data-probe-recovery: invalid_record_retained=%s prior_valid=0 generation_reset=1\n",quarantine);
            exists=0;prior_boots=0;prior_bytes=0;
        } else prior_valid=1;
    }
    memset(record,0,32);memcpy(record,"LHDPROB1",8);
    uint64_t boots=prior_boots+1;memcpy(record+16,&boots,8);memcpy(record+24,&lancelot_data_capacity,8);
    for(unsigned i=32;i<sizeof(record);i++)record[i]=(uint8_t)(i*37u+11u);
    uint64_t writes_before=lancelot_data_counter("kern.redwood.lancelot_data_writes");
    uint64_t flushes_before=lancelot_data_counter("kern.redwood.lancelot_data_flushes");
    fd=open(path,O_RDWR|(exists?0:(O_CREAT|O_EXCL)),0600);if(fd<0)return lancelot_data_probe_error("write_open",fd,errno);
    ssize_t write_result=pwrite(fd,record,sizeof(record),0);int ok=write_result==(ssize_t)sizeof(record);
    if(!ok)lancelot_data_probe_error("write",write_result,write_result<0?errno:0);
    if(ok) {int result=fsync(fd);if(result){lancelot_data_probe_error("file_fsync",result,errno);ok=0;}}
    int dirfd=open("/private/var/db",O_RDONLY|O_DIRECTORY,0);
    if(dirfd<0){lancelot_data_probe_error("directory_open",dirfd,errno);ok=0;}
    else {
        int result=fsync(dirfd);if(result){lancelot_data_probe_error("directory_fsync",result,errno);ok=0;}
        result=close(dirfd);if(result){lancelot_data_probe_error("directory_close",result,errno);ok=0;}
    }
    /* Darwin fsync alone need not flush a journal or the drive's cache.
     * fcntl(92), F_FULLFSYNC(51) uses HFS's full journal/device sync path. */
    if(ok) {long result=mini_syscall(92,fd,51,0,0,0,0);
        if(result){lancelot_data_probe_error("full_fsync",result,errno);ok=0;}}
    int close_result=close(fd);if(close_result){lancelot_data_probe_error("write_close",close_result,errno);ok=0;}
    fd=open(path,O_RDONLY,0);
    if(fd<0){lancelot_data_probe_error("readback_open",fd,errno);ok=0;}
    else {
        ssize_t result=pread(fd,readback,sizeof(readback),0);
        if(result!=(ssize_t)sizeof(readback)){lancelot_data_probe_error("readback_read",result,result<0?errno:0);ok=0;}
        else if(memcmp(record,readback,sizeof(record))){lancelot_data_probe_error("readback_content",0,0);ok=0;}
        close_result=close(fd);if(close_result){lancelot_data_probe_error("readback_close",close_result,errno);ok=0;}
    }
    uint64_t writes_after=lancelot_data_counter("kern.redwood.lancelot_data_writes");
    uint64_t flushes_after=lancelot_data_counter("kern.redwood.lancelot_data_flushes");
    if(ok&&!(writes_after>writes_before&&flushes_after>flushes_before)) {
        lancelot_data_probe_error("native_write_flush_counters",0,0);ok=0;
    }
    printf("lancelot-data-probe: ok=%d prior_valid=%d prior_boots=%llu current_boots=%llu bytes=%llu writes_before=%llu writes_after=%llu flushes_before=%llu flushes_after=%llu persistence_verified=%d\n",
        ok,prior_valid,(unsigned long long)prior_boots,(unsigned long long)boots,(unsigned long long)lancelot_data_capacity,
        (unsigned long long)writes_before,(unsigned long long)writes_after,(unsigned long long)flushes_before,
        (unsigned long long)flushes_after,ok&&prior_valid);
    return ok;
}
