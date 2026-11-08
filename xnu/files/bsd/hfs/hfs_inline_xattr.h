/* SPDX-License-Identifier: APSL-2.0
 * attrData[2] is the on-disk variable payload marker. Bounds come from the
 * allocated record, including the existing two-byte record alignment.
 */
#ifndef HFS_INLINE_XATTR_H
#define HFS_INLINE_XATTR_H
#include <stddef.h>
#include <stdint.h>
#include <sys/errno.h>
#include "hfs_format.h"

static inline int
hfs_inline_xattr_record_size(size_t payload_bytes, size_t *record_bytes)
{
    const size_t header=offsetof(HFSPlusAttrData,attrData);
    if(!record_bytes)return EINVAL;
    if(payload_bytes>UINT32_MAX||payload_bytes>(size_t)-1-header-1)return E2BIG;
    *record_bytes=header+payload_bytes+(payload_bytes&1);
    return 0;
}
static inline int
hfs_inline_xattr_copy(void *record, size_t record_bytes,
    const void *data, size_t payload_bytes)
{
    const size_t header=offsetof(HFSPlusAttrData,attrData);
    if(!record||record_bytes<header||payload_bytes>record_bytes-header||
       (payload_bytes&&!data))return EINVAL;
    if(payload_bytes) {
        /* Supply the allocation bound explicitly. The placeholder's two
         * bytes do not describe the storage allocated by hfs_malloc_data. */
        __builtin___memmove_chk((uint8_t *)record+header,data,payload_bytes,
            record_bytes-header);
    }
    return 0;
}
#endif
