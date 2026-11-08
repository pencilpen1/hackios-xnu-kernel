/* SPDX-License-Identifier: APSL-2.0
 * Build target facts for this static HFS port: embedded ARM64, little endian.
 */
#ifndef LANCELOT_HFS_TARGET_CONDITIONALS_H
#define LANCELOT_HFS_TARGET_CONDITIONALS_H
#define TARGET_OS_MAC 1
#define TARGET_OS_OSX 0
#define TARGET_OS_IPHONE 1
#define TARGET_OS_IOS 1
#define TARGET_OS_SIMULATOR 0
#define TARGET_OS_EMBEDDED 1
#define TARGET_OS_WIN32 0
#define TARGET_OS_UNIX 0
#define TARGET_CPU_ARM64 1
#define TARGET_CPU_ARM 0
#define TARGET_CPU_X86_64 0
#define TARGET_CPU_X86 0
#define TARGET_CPU_PPC 0
#define TARGET_CPU_PPC64 0
#define TARGET_RT_64_BIT 1
#define TARGET_RT_LITTLE_ENDIAN 1
#define TARGET_RT_BIG_ENDIAN 0
#define TARGET_RT_MAC_CFM 0
#define TARGET_RT_MAC_MACHO 1
#endif
