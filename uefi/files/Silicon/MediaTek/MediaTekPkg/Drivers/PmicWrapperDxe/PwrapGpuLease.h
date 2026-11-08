#include <Protocol/MtkGpuLease.h>
#include "GpuRailMasks.h"
static MTK_GPU_LEASE_PROTOCOL *mGpuLeasePeer;
/* All currently source-defined GPU enable/selector/DA targets are protected,
 * including raw Write/UpdateBits entrypoints. Not a peer/whole-PMIC claim. */
static BOOLEAN GpuRailTarget(UINT16 a,UINT16 mask) {
    for(unsigned i=0;i<sizeof(mGpuRailMasks)/sizeof(mGpuRailMasks[0]);i++)
        if(a==mGpuRailMasks[i][0])return (mask&mGpuRailMasks[i][1])!=0;
    /* Exact non-GPU regulator control registers from pinned MT6358PmicDxe.c
     * SHA788a70ee..; preserve ordinary CPU/LDO control, not guessed disjointness
     * of arbitrary raw PMIC writes (global reset could affect GPU as well). */
    static const UINT16 safe[]={0x1388,0x13a6,0x1408,0x1426,0x1488,0x14aa,0x1588,0x15a6,0x1608,0x1626,0x1688,0x16ae,0x1708,0x172e,0x1788,0x178a,
        0x1a88,0x1a9c,0x1ab0,0x1ac4,0x1ad8,0x1aec,0x1b08,0x1b1c,0x1b30,0x1c08,0x1c1c,0x1c30,0x1c44,0x1c58,0x1c6c,0x1c88,0x1c9c,
        0x1cb0,0x1cc4,0x1cd8,0x1d08,0x1d1c,0x1d32,0x1d48,0x1d5c,0x1d88,0x1d9e,0x1e08,0x1e0c,0x1e28,0x1e2c,0x1e30,0x1e34,0x1e38,0x1e3c,0x1e44,0x1e48,0x1e4c,0x1e98,0x1eaa,0x1eae};
    for(unsigned i=0;i<sizeof(safe)/sizeof(safe[0]);i++)if(a==safe[i])return FALSE;
    return TRUE; /* unclassified raw write: conservatively GPU-affecting */
}
static EFI_STATUS PwrapGpuBegin(UINT16 address,UINT16 mask,UINT64 *token) {
    *token=0;if(!GpuRailTarget(address,mask))return EFI_SUCCESS;
    if(!mGpuLeasePeer) {
        EFI_STATUS s=gBS->LocateProtocol(&gMediaTekGpuLeaseProtocolGuid,NULL,(VOID **)&mGpuLeasePeer);
        /* Before protocol publication no producer can acquire a lease. Keep
         * original early-boot semantics; once published it stays resident and
         * every protected/unknown target enters the actual shared writer gate. */
        if(EFI_ERROR(s)) {mGpuLeasePeer=NULL;return s==EFI_NOT_FOUND?EFI_SUCCESS:s;}
    }
    if(!mGpuLeasePeer||mGpuLeasePeer->Revision!=1||mGpuLeasePeer->Size!=sizeof(*mGpuLeasePeer)||
       !mGpuLeasePeer->WriterBegin||!mGpuLeasePeer->WriterEnd) {mGpuLeasePeer=NULL;return EFI_NOT_READY;}
    return mGpuLeasePeer->WriterBegin(mGpuLeasePeer,LGH_GPU_RAILS,token);
}
static VOID PwrapGpuEnd(UINT64 token,BOOLEAN known) {
    if(token)mGpuLeasePeer->WriterEnd(mGpuLeasePeer,token,known);
}
