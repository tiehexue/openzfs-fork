#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mount flags */
#define	ZMNT_READONLY 0x0001 /* dataset-level: try mount -o ro */

/* Unmount flags */
#define	ZUMNT_FORCE 0x0001 /* force unmount */

/*
 * JSON builders: return HeapAlloc'ed UTF-8 JSON; caller HeapFree()s.
 * Return shape examples:
 *   {"ok":true,"pool":"tank"}                      // pool ops
 *   {"ok":false,"pool":"tank","err":"<msg>"}       // pool ops err
 *   {"ok":true,"dataset":"tank/fs"}                // dataset ops
 *   {"ok":false,"dataset":"tank/fs","err":"<msg>"} // dataset err
 */

/* Pool-wide */
nvlist_t *
    zed_mount_pool_nvl(uint64_t pool_guid, const char *pool_name,
	const char *mntopts, uint32_t flags);

nvlist_t *
    zed_unmount_pool_nvl(uint64_t pool_guid, const char *pool_name,
	uint32_t flags);

nvlist_t *
    zed_mount_dataset_nvl(const char *dataset,
	const char *mntpoint_opt, /* NULL = default */
	uint32_t flags);

nvlist_t *
    zed_unmount_dataset_nvl(const char *dataset,
	uint32_t flags);


#ifdef __cplusplus
}
#endif
