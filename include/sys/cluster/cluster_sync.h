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

#ifndef _SYS_CLUSTER_CLUSTER_SYNC_H
#define	_SYS_CLUSTER_CLUSTER_SYNC_H

#include <sys/cluster/cluster_types.h>

struct vdev;
struct uberblock;

void cluster_sync_coordinator_pre_mos(cluster_spa_t *cspa);
void cluster_sync_coordinator_post_uberblock(cluster_spa_t *cspa,
    uint64_t txg);
void cluster_sync_participant_begin(cluster_spa_t *cspa, uint64_t txg);
void cluster_sync_participant_complete(cluster_spa_t *cspa, uint64_t txg);
boolean_t cluster_sync_uberblock_validate(cluster_spa_t *cspa,
    struct uberblock *ub);
void cluster_sync_uberblock_update(struct uberblock *ub, cluster_spa_t *cspa,
    struct vdev *rvd, uint64_t txg);
int cluster_sync_coordinator_takeover(cluster_spa_t *cspa);
void cluster_sync_spacemap_flush(cluster_spa_t *cspa, uint64_t txg);

#endif	/* _SYS_CLUSTER_CLUSTER_SYNC_H */
