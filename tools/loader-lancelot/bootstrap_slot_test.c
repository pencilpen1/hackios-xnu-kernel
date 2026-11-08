/* The included function uses the actual generated table and loader admission
 * statements, plus the production SHA256, on real root bytes. No device I/O. */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sha256.h"
#define PAYLOAD_MAX_SIZE (32u * 1024u * 1024u)
struct ext4_fs { int unused; };
struct ext4_inode { uint64_t size; };
struct payload_header { uint32_t magic, size; };
struct payload_entry { char name[16]; uint32_t size, reserved; };
static uint8_t *input,*payload;
static size_t input_size;
static unsigned reads,hashes;
static int fail_lookup,fail_read,error_kind;
static const char *verified_hash;
static int ext4_lookup(struct ext4_fs *fs,const char *name,struct ext4_inode *inode) {
    (void)fs;(void)name;inode->size=input_size;return !fail_lookup;
}
static int ext4_read_file(struct ext4_fs *fs,struct ext4_inode *inode,uint64_t off,uint64_t size,void *out) {
    (void)fs;assert(inode->size==input_size&&!off&&size==input_size);reads++;
    if(fail_read)return 0;
    memcpy(out,input,size);return 1;
}
static size_t str_len(const char *s) {return strlen(s);}
static void counted_sha(const void *data,size_t bytes,uint8_t hash[32]) {
    hashes++;sha256(data,bytes,hash);
}
#define sha256 counted_sha
static void print(const char *format,...) {
    if(strstr(format,"wrong-size"))error_kind=1;
    else if(strstr(format,"SHA256 mismatch"))error_kind=2;
    else if(strstr(format,"failed APFS read"))error_kind=3;
    else if(strstr(format,"missing"))error_kind=4;
    else if(strstr(format,"SHA256 verified")) {
        va_list args;va_start(args,format);(void)va_arg(args,const char*);
        (void)va_arg(args,uint64_t);verified_hash=va_arg(args,const char*);va_end(args);
    }
}
#include "actual-bootstrap-slot.h"
static void reset(void) {
    reads=hashes=0;fail_lookup=fail_read=error_kind=0;verified_hash=NULL;
    memset(payload,0,PAYLOAD_MAX_SIZE);
}
int main(int argc,char **argv) {
    assert(argc==4);FILE *f=fopen(argv[1],"rb");assert(f);
    assert(!fseek(f,0,SEEK_END));long size=ftell(f);assert(size>36);rewind(f);
    input_size=(size_t)size;input=malloc(input_size+4096);payload=malloc(PAYLOAD_MAX_SIZE);assert(input&&payload);
    assert(fread(input,1,input_size,f)==input_size);fclose(f);memset(input+input_size,0,4096);
    int expect=atoi(argv[3]);reset();int accepted=verify_slot(0);
    assert(accepted==expect&&reads==1&&hashes==1);
    if(accepted) {
        assert(!error_kind&&verified_hash&&!strcmp(verified_hash,argv[2]));
        struct payload_entry *entry=(void*)(payload+sizeof(struct payload_header));
        assert(entry->size==input_size&&!strcmp(entry->name,"ramdisk"));
        assert(!memcmp(entry+1,input,input_size));
    } else assert(error_kind==2); /* Old first-size-only code rejects root49. */
    reset();input[input_size-1]^=1;assert(!verify_slot(0)&&error_kind==2&&reads==1&&hashes==1);input[input_size-1]^=1;
    reset();input[32]^=1;assert(!verify_slot(0)&&error_kind==3&&reads==1&&!hashes);input[32]^=1;
    reset();input_size++;assert(!verify_slot(0)&&error_kind==1&&!reads&&!hashes);input_size--;
    reset();fail_read=1;assert(!verify_slot(0)&&error_kind==3&&reads==1&&!hashes);
    reset();fail_lookup=1;assert(!verify_slot(0)&&error_kind==4&&!reads&&!hashes);
    reset();assert(!verify_slot(1)); /* A root image cannot satisfy the bootdisk slot. */
    assert(error_kind==1||error_kind==2);
    free(input);free(payload);
    puts("Actual generated bootstrap: expected root admission, all32 digest bytes, APFS/read/size/missing rejection, one SHA/read, separate file slots PASS");
    return 0;
}
