"""Exercise the real HFS inline setter block and the deployed two-byte failure."""
from pathlib import Path
from datetime import datetime,timezone
import hashlib,json,os,subprocess,sys

P=Path(__file__).resolve().parent;R=P.parent
if os.name=='nt':
    path='/mnt/'+P.drive[0].lower()+P.as_posix()[2:]+'/' + Path(__file__).name
    raise SystemExit(subprocess.call(['wsl','-d','Ubuntu','--exec','python3','-B',path]))
E=R/'out/lancelot/tests';E.mkdir(parents=True,exist_ok=True)
O=E/('inline-xattr-'+datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f'));O.mkdir()
H=R/'xnu/files/bsd/hfs';current=H/'hfs_xattr.c'
old=R/'tools/lancelot-data/tests/old_inline_xattr.inc'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
sources={str(p):sha(p) for p in [current,old,H/'hfs_format.h',H/'hfs_inline_xattr.h',Path(__file__)]}
stub=O/'stubs/sys';stub.mkdir(parents=True)
(stub/'appleapiopts.h').write_text('#define __APPLE_API_PRIVATE 1\n#define __APPLE_API_UNSTABLE 1\n',encoding='utf-8')
def block(text):
    start=text.index('} else { /* Inline data */');brace=text.index('{',start);depth=1;end=brace+1
    while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[:start],text[brace:end],text[end:]
prefix,new_body,suffix=block(current.read_text(encoding='utf-8'))
old_prefix,old_body,old_suffix=block(old.read_text(encoding='utf-8'))
assert '#include "hfs_inline_xattr.h"' in prefix
assert old_prefix=='' and not old_suffix.strip()
fixture=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <errno.h>
typedef char uuid_string_t[37];
#include "hfs_inline_xattr.h"
#define OLD_CONTROL OLD_VALUE
static size_t last_bound,checked_copies;
void *__memmove_chk(void *destination,const void *source,size_t bytes,size_t capacity){
    last_bound=capacity;checked_copies++;
    if(bytes>capacity){fprintf(stderr,"DEPLOYED_INLINE_SUBOBJECT_CHECK: %zu < %zu\n",capacity,bytes);exit(42);}
    volatile uint8_t *out=destination;const volatile uint8_t *in=source;
    if((uintptr_t)out<(uintptr_t)in){for(size_t i=0;i<bytes;i++)out[i]=in[i];}
    else {for(size_t i=bytes;i;i--)out[i-1]=in[i-1];}
    return destination;
}
/* The deployed ARM64 call passes bound=2, confirmed in its instructions and
 * panic. Model that argument; host flexible-array inference can differ. */
#define bcopy(source,destination,bytes) __builtin___memmove_chk((destination),(source),(bytes),sizeof(((HFSPlusAttrData*)0)->attrData))
typedef struct {void *bufferAddress;uint32_t itemSize,itemCount;} FSBufferDescriptor;
struct BTreeIterator {uint8_t key[300];};
struct vnop_setxattr_args {const char *a_name;};
struct fixture_uio {const uint8_t *data;size_t bytes,offset;bool fault;};
typedef struct fixture_uio *uio_t;
static uint8_t data[65520],*allocation;
static size_t allocated_bytes,expected_bytes;
static unsigned inserts,removes,frees;static bool allocation_failure;
__attribute__((noinline)) static void *hfs_malloc_data(size_t bytes){
    allocated_bytes=bytes;if(allocation_failure)return NULL;
    allocation=malloc(bytes+64);assert(allocation);memset(allocation,0xa5,bytes+64);
    memset(allocation+32,0xcd,bytes);return allocation+32;
}
static void release(void *record,size_t bytes){
    assert(record==allocation+32&&bytes==allocated_bytes);
    for(unsigned i=0;i<32;i++)assert(allocation[i]==0xa5&&allocation[32+bytes+i]==0xa5);
    free(allocation);allocation=NULL;frees++;
}
static int remove_attribute_records(void *file,struct BTreeIterator *it){assert(file&&it);removes++;return 0;}
static int hfs_buildattrkey(uint32_t id,const char *name,HFSPlusAttrKey *key){assert(id==7&&name&&key);return 0;}
static int uiomove(char *out,size_t bytes,uio_t uio){
    assert(uio&&bytes<=uio->bytes-uio->offset);
    size_t n=uio->fault?bytes/2:bytes;memcpy(out,uio->data+uio->offset,n);uio->offset+=n;
    return uio->fault?EFAULT:0;
}
static int BTInsertRecord(void *file,struct BTreeIterator *it,FSBufferDescriptor *buffer,uint16_t bytes){
    assert(file&&it&&buffer->itemCount==1&&buffer->itemSize==bytes&&bytes==allocated_bytes);
    HFSPlusAttrRecord *record=buffer->bufferAddress;
    assert(record->recordType==kHFSPlusAttrInlineData&&record->attrData.attrSize==expected_bytes);
    assert(!record->attrData.reserved[0]&&!record->attrData.reserved[1]);
    uint8_t *payload=(uint8_t*)record+offsetof(HFSPlusAttrData,attrData);
    assert(!memcmp(payload,data,expected_bytes));
    if(!OLD_CONTROL&&(expected_bytes&1))assert(payload[expected_bytes]==0);
    inserts++;return 0;
}
__attribute__((noinline)) static int setter(size_t attrsize,const void *data_ptr,uio_t uio,int exists){
    FSBufferDescriptor btdata={0};HFSPlusAttrRecord *recp=NULL;size_t recp_size=0;int result=0;
    struct BTreeIterator iterator_storage,*iterator=&iterator_storage;
    struct vnop_setxattr_args args={"com.apple.fixture"},*ap=&args;
    uint32_t target_id=7;void *btfile=(void*)1,*hfsmp=(void*)2;
    INLINE_BODY
exit:
    if(recp)release(recp,recp_size);
    return result;
}
static void reset(size_t bytes){allocated_bytes=0;expected_bytes=bytes;inserts=removes=frees=0;allocation_failure=false;}
int main(void){
    assert(offsetof(HFSPlusAttrData,attrData)==16&&sizeof(((HFSPlusAttrData*)0)->attrData)==2);
    for(unsigned i=0;i<sizeof(data);i++)data[i]=(uint8_t)(i*17+3);
    if(OLD_CONTROL){reset(15);(void)setter(15,data,NULL,0);assert(!"deployed 2 < 15 failure was not reproduced");}
    const size_t sizes[]={0,1,2,3,15,16,31,255,4096,65518};
    for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++){
        size_t bytes=sizes[i],record_bytes=0;assert(!hfs_inline_xattr_record_size(bytes,&record_bytes));
        assert(record_bytes==16+bytes+(bytes&1));
        for(unsigned exists=0;exists<2;exists++){
            reset(bytes);assert(setter(bytes,data,NULL,exists)==0&&inserts==1&&removes==exists&&frees==1);
            struct fixture_uio uio={data,bytes,0,false};reset(bytes);
            assert(setter(bytes,NULL,&uio,exists)==0&&inserts==1&&removes==exists&&frees==1&&uio.offset==bytes);
        }
    }
    reset(15);assert(setter(15,NULL,NULL,0)==EPERM&&!inserts&&frees==1);
    struct fixture_uio bad={data,15,0,true};reset(15);
    assert(setter(15,NULL,&bad,0)==EFAULT&&!inserts&&frees==1&&bad.offset==7);
    reset(15);allocation_failure=true;assert(setter(15,data,NULL,1)==ENOMEM&&!removes&&!inserts&&!frees);
    reset(65519);assert(setter(65519,data,NULL,1)==E2BIG&&!removes&&!inserts&&!allocated_bytes);
    size_t unchanged=123;assert(hfs_inline_xattr_record_size((size_t)-1,&unchanged)==E2BIG&&unchanged==123);
    assert(hfs_inline_xattr_record_size(15,NULL)==EINVAL);
    if(sizeof(size_t)>4)assert(hfs_inline_xattr_record_size((size_t)UINT32_MAX+1,&unchanged)==E2BIG);
    uint8_t buffer[64];memset(buffer,0xa5,sizeof(buffer));uint8_t before[64];memcpy(before,buffer,sizeof(buffer));
    assert(hfs_inline_xattr_copy(buffer,30,data,15)==EINVAL&&!memcmp(before,buffer,sizeof(buffer)));
    assert(hfs_inline_xattr_copy(buffer,15,data,1)==EINVAL&&!memcmp(before,buffer,sizeof(buffer)));
    assert(hfs_inline_xattr_copy(buffer,32,NULL,15)==EINVAL&&!memcmp(before,buffer,sizeof(buffer)));
    assert(hfs_inline_xattr_copy(NULL,32,data,15)==EINVAL);
    assert(hfs_inline_xattr_copy(buffer,16,NULL,0)==0&&!memcmp(before,buffer,sizeof(buffer)));
    assert(hfs_inline_xattr_copy(buffer,32,data,15)==0&&!memcmp(buffer+16,data,15));
    puts("Production inline HFS setter: real variable record layout, 15-byte reproduction, kernel/UIO paths, zero/max/alignment, errors, allocation-before-replace and copy bounds passed");
}
'''
runs=[];passed=True
for label,body in [('current',new_body),('deployed-r13',old_body)]:
    cfile=O/(label+'.c');cfile.write_text(fixture.replace('INLINE_BODY',body).replace('OLD_VALUE','1' if label=='deployed-r13' else '0'),encoding='utf-8')
    for cc in ['gcc','clang']:
        exe=O/(label+'-'+cc)
        for phase,command in [('build',[cc,'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(O/'stubs'),'-I'+str(H),cfile,'-o',exe]),('run',[exe])]:
            result=subprocess.run(list(map(str,command)),capture_output=True,timeout=90)
            log=O/(label+'-'+cc+'-'+phase+'.log');log.write_bytes(result.stdout+result.stderr)
            expected=42 if label=='deployed-r13' and phase=='run' else 0
            matched=result.returncode==expected
            if expected==42:matched=matched and b'DEPLOYED_INLINE_SUBOBJECT_CHECK: 2 < 15' in result.stderr
            runs.append({'name':log.stem,'exit':result.returncode,'expected':expected,'matched':matched,'log':log.name,'sha256':sha(log)})
            print(log.stem,result.returncode,'expected',expected,(result.stdout+result.stderr).decode(errors='replace')[-1800:],flush=True)
            if not matched:passed=False;break
        if not passed:break
    if not passed:break
assert all(sha(Path(n))==h for n,h in sources.items())
with (O/'result.json').open('x',encoding='utf-8') as file:
    json.dump({'passed':passed and len(runs)==8,'sources':sources,'runs':runs,
               'actual_inline_setter_branch_and_HFS_disk_types':True,'historical_inline_branch_fixture_retained':True,
               'allocator_UIO_and_BTree_insertion_modeled':True,'deployed_R13_two_byte_negative_control':True,
               'native_two_byte_checker_bound_modeled_from_verified_ARM64_call':True,
               'device_access':False,'GPU_complete':False},file,indent=2)
print('INLINE XATTR TESTS',O,flush=True);raise SystemExit(0 if passed and len(runs)==8 else 1)
