/* AP target-writer lease only; opaque SSPM/EL3 permissions stay unverified. */
#ifndef MTK_GPU_LEASE_H
#define MTK_GPU_LEASE_H
#include <Uefi.h>
#define LGH_EDK2
#include <Library/LancelotGpuHandoff.h>
#undef LGH_EDK2
#define MTK_GPU_LEASE_GUID {0x583f0279,0x6ae2,0x4f41,{0xa8,0x44,0x67,0x68,0x47,0x50,0x55,0x31}}
extern EFI_GUID gMediaTekGpuLeaseProtocolGuid;
typedef struct MTK_GPU_LEASE_PROTOCOL MTK_GPU_LEASE_PROTOCOL;
struct MTK_GPU_LEASE_PROTOCOL {
    UINT32 Revision, Size;
    EFI_STATUS (EFIAPI *WriterBegin)(MTK_GPU_LEASE_PROTOCOL *,UINT32,UINT64 *);
    EFI_STATUS (EFIAPI *WriterEnd)(MTK_GPU_LEASE_PROTOCOL *,UINT64,BOOLEAN);
    EFI_STATUS (EFIAPI *Prepare)(MTK_GPU_LEASE_PROTOCOL *,UINT64,UINT32,UINT32,struct lgh_offer *);
    EFI_STATUS (EFIAPI *Seal)(MTK_GPU_LEASE_PROTOCOL *,UINT64,struct lgh_offer *);
    EFI_STATUS (EFIAPI *Query)(MTK_GPU_LEASE_PROTOCOL *,struct lgh_offer *);
    EFI_STATUS (EFIAPI *Release)(MTK_GPU_LEASE_PROTOCOL *,const struct lgh_return *);
};
#endif
