#pragma once
#include <stdint.h>
#include <sys/nvpair.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Build an nvlist result for mount preflight.
 * Pool-scope result shape:
 * {"ok":true, "scope":"pool", "pool":"tank", "locked":[{"name":"tank/
 *  secure", "keyformat":"passphrase", "keylocation":"prompt"}]}
 *
 * Dataset-scope result shape:
 * {"ok":true, "scope":"dataset", "dataset":"tank/fs", "ds_type":"filesystem",
 *  "encroot":"tank/secure", "locked":[ ... ]}
 * If dataset is not a filesystem: ds_type is "volume"/"snapshot"/"other"
 * and "locked" is empty.
 */
nvlist_t *zed_mount_preflight_pool_nvl(libzfs_handle_t *lzh,
    const char *pool_name);
nvlist_t *zed_mount_preflight_dataset_nvl(libzfs_handle_t *lzh,
    const char *dataset);

/*
 * Load a key for a single encryption root / dataset.
 * Returns nvlist: {"ok":true,"dataset":"..."} or
 * {"ok":false,"dataset":"...","err":"..."}.
 * `pass` may be NULL/0 (if your tree accepts non-interactive key sources).
 */
nvlist_t *zed_load_key_one_nvl(libzfs_handle_t *lzh,
    const char *dataset_utf8,
    const uint8_t *pass,
    uint32_t passlen);

#ifdef __cplusplus
}
#endif
