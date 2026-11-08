/* lancelot writable HFS+ data, built into XNU; public APSL sources. */
#ifndef LANCELOT_HFS_CONFIG_H
#define LANCELOT_HFS_CONFIG_H
#include <TargetConditionals.h>
#define _hfs_config_ 1
#define LANCELOT_HFS_STATIC 1
#define HFS_COMPRESSION 1
#ifndef FIFO
#define FIFO 1
#endif
#ifndef QUOTA
#define QUOTA 0
#endif
#undef CONFIG_PROTECT
#define CONFIG_PROTECT 0
#define CONFIG_SECLUDED_RENAME 1
#define CONFIG_HFS_DIRLINK 0
#define CONFIG_SEARCHFS 1
#undef NAMEDSTREAMS
#define NAMEDSTREAMS 0
#define HFS_CONFIG_KEY_ROLL 0
#endif
