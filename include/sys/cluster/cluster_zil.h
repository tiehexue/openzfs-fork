/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2026 Cluster ZFS Project. All rights reserved.
 */

#ifndef _SYS_CLUSTER_CLUSTER_ZIL_H
#define	_SYS_CLUSTER_CLUSTER_ZIL_H

#include <sys/cluster/cluster_types.h>

struct spa;
struct objset;
struct dmu_tx;
struct blkptr;

#include <sys/zil.h>

#define	CLUSTER_ZIL_HEADER_SIZE	(64 * 1024)	/* 64KB header */
#define	CLUSTER_ZIL_MIN_REGION_SIZE	(64 * 1024 * 1024) /* 64MB min */

int cluster_zil_init(cluster_zil_t *cz, struct spa *spa);
void cluster_zil_fini(cluster_zil_t *cz);
int cluster_zil_reserve(cluster_zil_t *cz, cluster_node_id_t node_id);
int cluster_zil_release(cluster_zil_t *cz, cluster_node_id_t node_id);
uint64_t cluster_zil_alloc_offset(cluster_zil_t *cz,
    cluster_node_id_t node_id, uint64_t size);
void cluster_zil_free_block(cluster_zil_t *cz, cluster_node_id_t node_id,
    uint64_t offset, uint64_t size);
int cluster_zil_claim(cluster_zil_t *cz, cluster_node_id_t node_id);
int cluster_zil_replay(cluster_zil_t *cz, cluster_node_id_t dead_node,
    zil_replay_func_t *replay_func);
void cluster_zil_destroy(cluster_zil_t *cz, cluster_node_id_t dead_node);
int cluster_zil_write_map(cluster_zil_t *cz, struct objset *mos,
    struct dmu_tx *tx);
int cluster_zil_read_map(cluster_zil_t *cz, struct objset *mos);

/*
 * Convenience wrapper for zio_alloc_zil() integration.
 * In cluster mode, attempts to allocate from this node's reserved
 * ZIL region. Returns 0 on success, ENOSPC if region is full,
 * or ENOTSUP if cluster ZIL is not active.
 */
int cluster_zil_alloc_block(cluster_spa_t *cspa, struct objset *os,
    uint64_t txg, struct blkptr *new_bp, uint64_t min_size,
    uint64_t max_size, boolean_t *slog);

#endif	/* _SYS_CLUSTER_CLUSTER_ZIL_H */
