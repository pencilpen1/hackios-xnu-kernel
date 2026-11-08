/* SPDX-License-Identifier: APSL-2.0
 * HFS catalog fields retain their on-disk layout. Copy each declared field
 * within its own bound instead of treating adjacent fields as one C object.
 */
#ifndef HFS_FINDERINFO_COPY_H
#define HFS_FINDERINFO_COPY_H
#include <stddef.h>
#include "hfs_format.h"
_Static_assert(sizeof(((HFSPlusCatalogFolder *)0)->userInfo) == 16, "HFS folder Finder info");
_Static_assert(sizeof(((HFSPlusCatalogFolder *)0)->finderInfo) == 16, "HFS folder extended Finder info");
_Static_assert(sizeof(((HFSPlusCatalogFile *)0)->userInfo) == 16, "HFS file Finder info");
_Static_assert(sizeof(((HFSPlusCatalogFile *)0)->finderInfo) == 16, "HFS file extended Finder info");
_Static_assert(offsetof(HFSPlusCatalogFolder, finderInfo) == offsetof(HFSPlusCatalogFolder, userInfo) + 16, "HFS folder disk layout");
_Static_assert(offsetof(HFSPlusCatalogFile, finderInfo) == offsetof(HFSPlusCatalogFile, userInfo) + 16, "HFS file disk layout");
static inline void hfs_copy_finderinfo_to_folder(HFSPlusCatalogFolder *record, const unsigned char source[32])
{
    bcopy(source, &record->userInfo, sizeof(record->userInfo));
    bcopy(source + sizeof(record->userInfo), &record->finderInfo, sizeof(record->finderInfo));
}
static inline void hfs_copy_finderinfo_to_file(HFSPlusCatalogFile *record, const unsigned char source[32])
{
    bcopy(source, &record->userInfo, sizeof(record->userInfo));
    bcopy(source + sizeof(record->userInfo), &record->finderInfo, sizeof(record->finderInfo));
}
static inline void hfs_copy_finderinfo_from_file(unsigned char destination[32], const HFSPlusCatalogFile *record)
{
    bcopy(&record->userInfo, destination, sizeof(record->userInfo));
    bcopy(&record->finderInfo, destination + sizeof(record->userInfo), sizeof(record->finderInfo));
}
static inline void hfs_clear_catalog_forks(HFSPlusCatalogFile *record)
{
    bzero(&record->dataFork, sizeof(record->dataFork));
    bzero(&record->resourceFork, sizeof(record->resourceFork));
}
#endif
