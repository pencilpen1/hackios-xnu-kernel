/* SPDX-License-Identifier: APSL-2.0
 * Static filesystem integration for lancelot's native emmc data device.
 */
#include <sys/systm.h>
#include <mach/mach_types.h>
#include "hfs_iokit.h"
bool panic_on_assert = false;
int hfs_is_ejectable(const char *name) { (void)name; return 0; }
void hfs_iterate_media_with_content(const char *content,
    int (*function)(const char *,const char *,void *),void *argument) {
    (void)content;(void)function;(void)argument;
}
kern_return_t hfs_get_platform_serial_number(char *number,uint32_t length) {
    if (length) number[0]=0;
    return KERN_NOT_SUPPORTED;
}
