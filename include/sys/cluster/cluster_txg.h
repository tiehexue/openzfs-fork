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

#ifndef _SYS_CLUSTER_CLUSTER_TXG_H
#define	_SYS_CLUSTER_CLUSTER_TXG_H

#include <sys/cluster/cluster_types.h>

int cluster_txg_init(cluster_txg_t *ctx, cluster_node_id_t coordinator);
void cluster_txg_fini(cluster_txg_t *ctx);
uint64_t cluster_txg_open(cluster_txg_t *ctx, cluster_membership_t *cm);
void cluster_txg_quiesce(cluster_txg_t *ctx, cluster_membership_t *cm);
void cluster_txg_sync_start(cluster_txg_t *ctx, cluster_membership_t *cm);
void cluster_txg_sync_done(cluster_txg_t *ctx, cluster_membership_t *cm);
void cluster_txg_handle_open(cluster_txg_t *ctx, uint64_t txg);
void cluster_txg_handle_quiesce(cluster_txg_t *ctx, uint64_t txg,
    cluster_node_id_t local_id);
void cluster_txg_handle_sync_start(cluster_txg_t *ctx, uint64_t txg);
void cluster_txg_handle_sync_done(cluster_txg_t *ctx, uint64_t txg);
void cluster_txg_report_dirty(cluster_txg_t *ctx, cluster_node_id_t node_id,
    uint64_t dirty_amount);
void cluster_txg_report_holds(cluster_txg_t *ctx, cluster_node_id_t node_id,
    uint64_t hold_count);
boolean_t cluster_txg_all_holds_released(cluster_txg_t *ctx,
    cluster_membership_t *cm);
boolean_t cluster_txg_all_sync_complete(cluster_txg_t *ctx,
    cluster_membership_t *cm);
void cluster_txg_allocate_ranges(cluster_txg_t *ctx,
    cluster_membership_t *cm, uint64_t range_size);
void cluster_txg_sync_barrier_enter(cluster_spa_t *cspa);
void cluster_txg_sync_barrier_exit(cluster_spa_t *cspa);
void cluster_txg_participant_sync(cluster_spa_t *cspa, uint64_t txg);
void cluster_txg_wait_quiesce(cluster_spa_t *cspa, uint64_t txg);
void cluster_txg_wait_sync_start(cluster_spa_t *cspa, uint64_t txg);
void cluster_txg_broadcast_sync_done(cluster_spa_t *cspa, uint64_t txg);

#endif	/* _SYS_CLUSTER_CLUSTER_TXG_H */
