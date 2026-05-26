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

#ifndef _SYS_CLUSTER_CLUSTER_RECOVERY_H
#define	_SYS_CLUSTER_CLUSTER_RECOVERY_H

#include <sys/cluster/cluster_types.h>

struct spa;

int cluster_fence_write(struct spa *spa, cluster_node_id_t fenced_node,
    uint64_t epoch, const char *reason);
boolean_t cluster_fence_check(struct spa *spa, cluster_node_id_t local_id,
    uint64_t current_epoch);
int cluster_fence_unfence(struct spa *spa, cluster_node_id_t unfenced_node,
    uint64_t epoch);
int cluster_recovery_node(cluster_spa_t *cspa, struct spa *spa,
    cluster_node_id_t dead_node);
int cluster_recovery_zil(cluster_spa_t *cspa, struct spa *spa,
    cluster_node_id_t dead_node);
void cluster_recovery_deferred_frees(cluster_spa_t *cspa, struct spa *spa,
    cluster_node_id_t dead_node);
void cluster_recovery_reassign_metaslabs(cluster_spa_t *cspa);
int cluster_recovery_coordinator(cluster_spa_t *cspa, struct spa *spa);
void cluster_recovery_self_fence(cluster_spa_t *cspa, struct spa *spa);

#endif	/* _SYS_CLUSTER_CLUSTER_RECOVERY_H */
