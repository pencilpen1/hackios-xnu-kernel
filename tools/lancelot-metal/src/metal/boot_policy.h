/* SPDX-License-Identifier: MIT */
#ifndef LANCELOT_METAL_BOOT_POLICY_H
#define LANCELOT_METAL_BOOT_POLICY_H
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
static inline bool lancelot_metal_boot_token(const char *arguments, const char *token)
{
    if (!arguments || !token || !*token) return false;
    size_t length = strlen(token);
    for (const char *p = arguments; *p;) {
        while (*p == ' ' || *p == '\t') p++;
        const char *end = p;
        while (*end && *end != ' ' && *end != '\t') end++;
        if ((size_t)(end - p) == length && !memcmp(p, token, length)) return true;
        p = end;
    }
    return false;
}
#endif
