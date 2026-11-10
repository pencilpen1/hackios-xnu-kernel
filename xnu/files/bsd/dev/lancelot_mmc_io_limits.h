/* Lancelot Android mmcblk0 queue, read on 2026-11-10: 512 KiB software
 * and hardware requests, 128 segments, 64 KiB per segment. These limits
 * describe a filesystem request, not one IRQ-masked native PIO transfer. */
#ifndef LANCELOT_MMC_IO_LIMITS_H
#define LANCELOT_MMC_IO_LIMITS_H
#define LANCELOT_MMC_REQUEST_BYTES (512u * 1024u)
#define LANCELOT_MMC_SEGMENTS 128u
#define LANCELOT_MMC_SEGMENT_BYTES (64u * 1024u)

static inline int
lancelot_mmc_io_limits(u_long command, caddr_t value, uint32_t block_size)
{
    switch (command) {
    case DKIOCGETMAXBLOCKCOUNTREAD:
    case DKIOCGETMAXBLOCKCOUNTWRITE:
        *(uint64_t *)value = LANCELOT_MMC_REQUEST_BYTES / block_size; return 0;
    case DKIOCGETMAXBYTECOUNTREAD:
    case DKIOCGETMAXBYTECOUNTWRITE:
        *(uint64_t *)value = LANCELOT_MMC_REQUEST_BYTES; return 0;
    case DKIOCGETMAXSEGMENTCOUNTREAD:
    case DKIOCGETMAXSEGMENTCOUNTWRITE:
        *(uint64_t *)value = LANCELOT_MMC_SEGMENTS; return 0;
    case DKIOCGETMAXSEGMENTBYTECOUNTREAD:
    case DKIOCGETMAXSEGMENTBYTECOUNTWRITE:
        *(uint64_t *)value = LANCELOT_MMC_SEGMENT_BYTES; return 0;
    case DKIOCGETMINSEGMENTALIGNMENTBYTECOUNT:
        *(uint64_t *)value = 512; return 0;
    case DKIOCGETCOMMANDPOOLSIZE:
        /* Android nr_requests is a software queue limit. This backend has
         * one synchronous controller, so it advertises one active command. */
        *(uint32_t *)value = 1; return 0;
    case DKIOCGETFEATURES:
        /* Full flushes and verified writes; no hardware FUA or UNMAP. */
        *(uint32_t *)value = 0; return 0;
    case DKIOCISSOLIDSTATE:
        *(uint32_t *)value = 1; return 0;
    default: return ENOTTY;
    }
}
#endif
