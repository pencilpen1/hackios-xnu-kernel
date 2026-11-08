/* Included AFTER pinned ClockDxe mutators renamed *Impl by the overlay. All
 * public mutators serialize on this shared protocol's lock before evaluating
 * their actual parent chain. No CPU PLL/rate/rail policy changes. */
#include <Library/UefiLib.h>
#include <Library/ArmLib.h>
#include <Library/ArmSmcLib.h>
#include <Library/BootLogLib.h>
#include <Protocol/MtkGpuLease.h>
static EFI_LOCK mGpuLeaseLock=EFI_INITIALIZE_LOCK_VARIABLE(TPL_NOTIFY);
static struct lgh_offer mGpuOffer;
static UINT64 mGpuSerial, mGpuWriter;
static UINT32 mGpuWriterMask, mMainLeaseReferences;
static MTK_GPU_LEASE_PROTOCOL mGpuLease;
static BOOLEAN GpuBootMatches(UINT64 wanted) {
    UINT64 actual=0,start=0;UINT8 unused;
    BootLogSnapshot(0,&actual,&start,&unused,0);return actual && actual==wanted;
}
static VOID GpuObserveClockWrite(MTK_CLOCK_CONTROLLER_TYPE bank,UINT32 reg,UINT32 value) {
    if(bank!=ClkApMixed)return;
    if(reg!=0x248 && reg!=0x24c && reg!=0x254 && reg!=0x258 && reg!=0x25c && reg!=0x264)return;
    UINT32 mask=(reg==0x24c || reg==0x25c)?0x7fffffffu:0xffffffffu;
    if((ClockRead(bank,reg)&mask)!=(value&mask))mGpuClockWriteFailed=TRUE;
}
static BOOLEAN GpuOtherApsOff(UINT32 mpidr) {
    if((ArmReadMpidr()&0xffffffu)!=mpidr)return FALSE;
    for(UINT32 id=0;id<=0x700;id+=0x100)if(id!=mpidr) {
        ARM_SMC_ARGS a={0};a.Arg0=0xc4000004ULL;a.Arg1=id;a.Arg2=0;
        ArmCallSmc(&a);if(a.Arg0!=1)return FALSE; /* standard PSCI read-only query */
    }
    return TRUE;
}
static UINT32 GpuClockMask(const MTK_CLOCK_DESC *c) {
    if(!c)return 0;
    if(c->Type==ClockTypePll && c->Controller==ClkApMixed) {
        if(c->Pll.BaseOffset==0x248)return LGH_MFGPLL;
        if(c->Pll.BaseOffset==0x258)return LGH_MAINPLL;
    }
    if((c->Type==ClockTypeMux || c->Type==ClockTypeMuxGate) && c->Controller==ClkTopCkGen &&
       c->MuxGate.MuxOffset==0x50 && c->MuxGate.MuxShift==0)return LGH_MFGMUX;
    return 0;
}
static EFI_STATUS GpuEnableMask(UINT32 id,UINT32 *mask,UINTN depth) {
    const MTK_CLOCK_DESC *c=ClockGetById(id);UINT32 parent;EFI_STATUS s;
    if(!c||depth>gClockCount)return EFI_INVALID_PARAMETER;
    *mask|=GpuClockMask(c);
    switch(c->Type) {
    case ClockTypeFixed:return EFI_SUCCESS;
    case ClockTypePll:parent=c->Pll.Parent;break;
    case ClockTypeFactors:parent=c->Factor.Parent;break;
    case ClockTypeGate:parent=c->Gate.Parent;break;
    case ClockTypeMux:case ClockTypeMuxGate:
        s=ClockGetParent(id,&parent);if(EFI_ERROR(s))return s;break;
    default:return EFI_INVALID_PARAMETER;
    }
    return GpuEnableMask(parent,mask,depth+1);
}
static EFI_STATUS GpuWriterLocked(UINT32 mask,UINT64 *token) {
    if(!token || (mask&~LGH_RESOURCES) || mGpuWriter)return EFI_INVALID_PARAMETER;
    if(mask && (mGpuOffer.state!=LGH_IDLE || mMainLeaseReferences))return EFI_ACCESS_DENIED;
    if(++mGpuSerial==0)++mGpuSerial;
    mGpuWriter=mGpuSerial;mGpuWriterMask=mask;mGpuClockWriteFailed=FALSE;*token=mGpuWriter;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI GpuWriterBegin(MTK_GPU_LEASE_PROTOCOL *p,UINT32 mask,UINT64 *token) {
    if(p!=&mGpuLease)return EFI_INVALID_PARAMETER;
    EFI_STATUS s=EfiAcquireLockOrFail(&mGpuLeaseLock);if(EFI_ERROR(s))return s;
    s=GpuWriterLocked(mask,token);if(EFI_ERROR(s))EfiReleaseLock(&mGpuLeaseLock);return s;
}
static EFI_STATUS EFIAPI GpuWriterEnd(MTK_GPU_LEASE_PROTOCOL *p,UINT64 token,BOOLEAN known) {
    if(p!=&mGpuLease || !token || token!=mGpuWriter)return EFI_INVALID_PARAMETER;
    if(!known && mGpuWriterMask) {mGpuOffer.state=LGH_UNKNOWN;lgh_seal_offer(&mGpuOffer);}
    mGpuWriter=0;mGpuWriterMask=0;EfiReleaseLock(&mGpuLeaseLock);return EFI_SUCCESS;
}
static EFI_STATUS GpuClockBegin(UINT32 id,BOOLEAN ancestors,UINT64 *token) {
    EFI_STATUS s=EfiAcquireLockOrFail(&mGpuLeaseLock);UINT32 mask=0;if(EFI_ERROR(s))return s;
    const MTK_CLOCK_DESC *c=ClockGetById(id);
    s=c?EFI_SUCCESS:EFI_INVALID_PARAMETER;
    if(!EFI_ERROR(s)) {mask=GpuClockMask(c);if(ancestors)s=GpuEnableMask(id,&mask,0);}
    if(!EFI_ERROR(s))s=GpuWriterLocked(mask,token);
    if(EFI_ERROR(s))EfiReleaseLock(&mGpuLeaseLock);
    return s;
}
EFI_STATUS ClockSetEnable(UINT32 id,BOOLEAN on) {
    UINT64 token;EFI_STATUS s=GpuClockBegin(id,TRUE,&token);if(EFI_ERROR(s))return s;
    s=ClockSetEnableImpl(id,on);if(mGpuClockWriteFailed)s=EFI_DEVICE_ERROR;
    if(!EFI_ERROR(s) && (mGpuWriterMask&LGH_MFGMUX)) {
        const MTK_CLOCK_DESC *mux=ClockGetByName("TOP_MFG_SEL");BOOLEAN enabled;
        if(!mux || EFI_ERROR(ClockIsEnabled(mux->Id,&enabled)) || enabled!=on)s=EFI_DEVICE_ERROR;
    }
    GpuWriterEnd(&mGpuLease,token,!EFI_ERROR(s));return s;
}
EFI_STATUS ClockSetFrequency(UINT32 id,UINT64 hz) {
    UINT64 token;EFI_STATUS s=GpuClockBegin(id,FALSE,&token);if(EFI_ERROR(s))return s;
    s=ClockSetFrequencyImpl(id,hz);if(mGpuClockWriteFailed)s=EFI_DEVICE_ERROR;
    GpuWriterEnd(&mGpuLease,token,!EFI_ERROR(s));return s;
}
EFI_STATUS ClockSetParent(UINT32 id,UINT32 parent) {
    UINT64 token;EFI_STATUS s=GpuClockBegin(id,FALSE,&token);if(EFI_ERROR(s))return s;
    s=ClockSetParentImpl(id,parent);
    /* SET/CLR/UPDATE aliases are not their live mux status. Verify the actual
     * parent while still holding the resource lock; a dropped write must not
     * release a protected writer as a known-success transaction. */
    if(!EFI_ERROR(s) && (mGpuWriterMask&LGH_MFGMUX)) {
        UINT32 actual;
        if(EFI_ERROR(ClockGetParent(id,&actual)) || actual!=parent)s=EFI_DEVICE_ERROR;
    }
    GpuWriterEnd(&mGpuLease,token,!EFI_ERROR(s));return s;
}
static EFI_STATUS EFIAPI GpuPrepare(MTK_GPU_LEASE_PROTOCOL *p,UINT64 boot,UINT32 mpidr,UINT32 off,struct lgh_offer *out) {
    EFI_STATUS s;UINT64 hz;const MTK_CLOCK_DESC *c=ClockGetByName("AP_MAINPLL");
    if(p!=&mGpuLease || !out || !boot || mpidr>0x700 || (mpidr&255) || off!=(255u^(1u<<(mpidr>>8))) ||
       !c || GpuClockMask(c)!=LGH_MAINPLL)return EFI_INVALID_PARAMETER;
    s=EfiAcquireLockOrFail(&mGpuLeaseLock);if(EFI_ERROR(s))return s;
    if(mGpuOffer.state!=LGH_IDLE || mGpuWriter || mMainLeaseReferences) {s=EFI_ACCESS_DENIED;goto done;}
    if(!GpuOtherApsOff(mpidr) || !GpuBootMatches(boot)) {s=EFI_ACCESS_DENIED;goto done;}
    lgh_zero(&mGpuOffer,sizeof(mGpuOffer));mGpuOffer.magic=LGH_MAGIC;mGpuOffer.version=1;mGpuOffer.size=sizeof(mGpuOffer);
    mGpuOffer.boot_id=boot;mGpuOffer.nonce=GetPerformanceCounter()^boot^(++mGpuSerial);
    if(!mGpuOffer.nonce)mGpuOffer.nonce=++mGpuSerial;
    mGpuOffer.state=LGH_PREPARED;mGpuOffer.resources=LGH_RESOURCES;mGpuOffer.capabilities=LGH_AP_LOCKED;
    mGpuOffer.boot_mpidr=mpidr;mGpuOffer.other_ap_off_mask=off;
    static const UINT8 pin[20]={0x83,0xf6,0x41,0xfd,0x2a,0x64,0x29,0x68,0xaa,0xc4,0xfb,0x36,0x98,0x69,0x56,0xdb,0x70,0x76,0x11,0x8e};
    for(unsigned i=0;i<20;i++)mGpuOffer.baseline_mu_commit[i]=pin[i];
    /* A genuine counted reference: request enable through the actual driver,
     * then verify hardware. Never just observe ON and set an ownership bool.
     * Failure retains the reference and writer denial; no guessed rollback. */
    mMainLeaseReferences++;mGpuOffer.main_refs=mMainLeaseReferences;mGpuClockWriteFailed=FALSE;
    s=ClockSetEnableImpl(c->Id,TRUE);if(EFI_ERROR(s))goto unknown;
    if(mGpuClockWriteFailed) {s=EFI_DEVICE_ERROR;goto unknown;}
    mGpuOffer.main_con0=ClockRead(ClkApMixed,0x258);mGpuOffer.main_con1=ClockRead(ClkApMixed,0x25c);
    mGpuOffer.main_con3=ClockRead(ClkApMixed,0x264);
    if(!(mGpuOffer.main_con0&1u)||(mGpuOffer.main_con3&3u)!=1u||
       mGpuOffer.main_con1!=ClockRead(ClkApMixed,0x25c)) {s=EFI_DEVICE_ERROR;goto unknown;}
    s=ClockGetFrequency(c->Id,&hz);if(EFI_ERROR(s)||!hz) {s=EFI_DEVICE_ERROR;goto unknown;}
    mGpuOffer.main_hz=hz;mGpuOffer.capabilities|=LGH_MAIN_HELD;
    lgh_seal_offer(&mGpuOffer);*out=mGpuOffer;goto done;
unknown:mGpuOffer.state=LGH_UNKNOWN;lgh_seal_offer(&mGpuOffer);
done:EfiReleaseLock(&mGpuLeaseLock);return s;
}
static EFI_STATUS EFIAPI GpuSeal(MTK_GPU_LEASE_PROTOCOL *p,UINT64 nonce,struct lgh_offer *out) {
    if(p!=&mGpuLease||!out)return EFI_INVALID_PARAMETER;
    EFI_STATUS s=EfiAcquireLockOrFail(&mGpuLeaseLock);if(EFI_ERROR(s))return s;
    if(mGpuOffer.state!=LGH_PREPARED || nonce!=mGpuOffer.nonce || mMainLeaseReferences!=1) s=EFI_ACCESS_DENIED;
    else if(!GpuOtherApsOff(mGpuOffer.boot_mpidr) || !GpuBootMatches(mGpuOffer.boot_id) ||
       mGpuOffer.main_con0!=ClockRead(ClkApMixed,0x258) || mGpuOffer.main_con1!=ClockRead(ClkApMixed,0x25c) ||
       mGpuOffer.main_con3!=ClockRead(ClkApMixed,0x264)) {mGpuOffer.state=LGH_UNKNOWN;lgh_seal_offer(&mGpuOffer);s=EFI_ACCESS_DENIED;}
    else {mGpuOffer.state=LGH_SEALED;lgh_seal_offer(&mGpuOffer);*out=mGpuOffer;s=EFI_SUCCESS;}
    EfiReleaseLock(&mGpuLeaseLock);return s;
}
static EFI_STATUS EFIAPI GpuQuery(MTK_GPU_LEASE_PROTOCOL *p,struct lgh_offer *out) {
    if(p!=&mGpuLease||!out)return EFI_INVALID_PARAMETER;
    EFI_STATUS s=EfiAcquireLockOrFail(&mGpuLeaseLock);if(EFI_ERROR(s))return s;
    *out=mGpuOffer;EfiReleaseLock(&mGpuLeaseLock);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI GpuRelease(MTK_GPU_LEASE_PROTOCOL *p,const struct lgh_return *r) {
    if(p!=&mGpuLease||!r)return EFI_INVALID_PARAMETER;
    EFI_STATUS s=EfiAcquireLockOrFail(&mGpuLeaseLock);if(EFI_ERROR(s))return s;
    if(mMainLeaseReferences!=1 || !lgh_return_valid(&mGpuOffer,r) || !GpuOtherApsOff(mGpuOffer.boot_mpidr) || !GpuBootMatches(mGpuOffer.boot_id) ||
       mGpuOffer.main_con0!=ClockRead(ClkApMixed,0x258) ||
       mGpuOffer.main_con1!=ClockRead(ClkApMixed,0x25c) ||
       mGpuOffer.main_con3!=ClockRead(ClkApMixed,0x264)) {mGpuOffer.state=LGH_UNKNOWN;lgh_seal_offer(&mGpuOffer);s=EFI_ACCESS_DENIED;}
    else {mMainLeaseReferences--;mGpuOffer.main_refs=mMainLeaseReferences;mGpuOffer.state=LGH_IDLE;
        mGpuOffer.capabilities=0;lgh_seal_offer(&mGpuOffer);s=EFI_SUCCESS;}
    /* Drop counted reference only; do NOT disable/restore a shared PLL. */
    EfiReleaseLock(&mGpuLeaseLock);return s;
}
static MTK_GPU_LEASE_PROTOCOL mGpuLease={1,sizeof(mGpuLease),GpuWriterBegin,GpuWriterEnd,GpuPrepare,GpuSeal,GpuQuery,GpuRelease};
