/* Query the existing Android JM driver's cached public property stream only.
 * No SET_FLAGS/context, GPU allocation, mmap, job, register or power ioctl.
 * Public version/property wire layouts agree in supplied r25p0/r32p1 headers;
 * the running driver is not claimed to match those source revisions. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct version { uint16_t major, minor; };
struct props { uint64_t buffer; uint32_t size, flags; };
_Static_assert(sizeof(struct version)==4, "version ABI");
_Static_assert(sizeof(struct props)==16, "property ABI");
#define VERSION _IOWR(0x80, 0, struct version)
#define PROPS _IOW(0x80, 3, struct props)
int main(void) {
    int fd=open("/dev/mali0",O_RDWR|O_CLOEXEC);
    if(fd<0){perror("open mali0");return 1;}
    struct version v={0,0};
    if(ioctl(fd,VERSION,&v)<0){perror("JM version");close(fd);return 2;}
    if(v.major!=11){fprintf(stderr,"Unsupported ABI; no further ioctl\n");close(fd);return 3;}
    struct props p={0};
    int n=ioctl(fd,PROPS,&p);
    if(n<=0||n>16384){fprintf(stderr,"Property size rejected: %d errno=%d\n",n,errno);close(fd);return 4;}
    void *buffer=calloc(1,(size_t)n);
    if(!buffer){close(fd);return 5;}
    p.buffer=(uintptr_t)buffer;p.size=(uint32_t)n;
    int got=ioctl(fd,PROPS,&p);
    /* adb's remote shell may merge stderr into stdout. No textual success
     * diagnostics: a fixed, length-checked envelope preserves binary framing. */
    const uint32_t header[4]={UINT32_C(0x3150474c),(uint32_t)n,
        (uint32_t)v.major|((uint32_t)v.minor<<16),0};
    int ok=got==n&&fwrite(header,1,sizeof(header),stdout)==sizeof(header)&&
        fwrite(buffer,1,(size_t)n,stdout)==(size_t)n&&fflush(stdout)==0;
    if(!ok)fprintf(stderr,"Property fetch/output rejected: %d expected=%d errno=%d\n",got,n,errno);
    free(buffer);close(fd);return ok?0:6;
}
