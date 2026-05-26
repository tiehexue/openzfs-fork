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

#ifndef _SYS_CLUSTER_CLUSTER_METASLAB_H
#define	_SYS_CLUSTER_CLUSTER_METASLAB_H

#include <sys/cluster/cluster_types.h>

struct spa;
struct dmu_tx;
struct metaslab;
struct vdev;

int cluster_ms_assign_init(cluster_ms_assign_t *cma, uint64_t vdev_id,
    uint64_t ms_count);
void cluster_ms_assign_fini(cluster_ms_assign_t *cma);
void cluster_ms_partition_static(cluster_ms_assign_t *cma,
    cluster_node_id_t *active_nodes, uint64_t num_active);
void cluster_ms_partition_adaptive(cluster_ms_assign_t *cma,
    cluster_node_id_t *active_nodes, uint64_t num_active,
    uint64_t *write_bytes, uint64_t total_ms_free);
uint64_t *cluster_ms_get_owned(cluster_ms_assign_t *cma,
    cluster_node_id_t node_id, uint64_t *count);
boolean_t cluster_ms_is_owned(cluster_ms_assign_t *cma, uint64_t ms_index,
    cluster_node_id_t node_id);
void cluster_ms_release(cluster_ms_assign_t *cma, uint64_t ms_index,
    cluster_node_id_t old_owner);
void cluster_ms_assign_one(cluster_ms_assign_t *cma, uint64_t ms_index,
    cluster_node_id_t new_owner);
int cluster_ms_assign_to_new_node(cluster_ms_assign_t *cma,
    cluster_node_id_t new_node, uint64_t target_count);
void cluster_ms_reclaim_from_dead_node(cluster_ms_assign_t *cma,
    cluster_node_id_t dead_node);
int cluster_ms_assign_write(struct spa *spa, cluster_ms_assign_t *cma,
    struct dmu_tx *tx);
int cluster_ms_assign_read(struct spa *spa, uint64_t vdev_id,
    cluster_ms_assign_t *cma);
boolean_t cluster_metaslab_alloc_filter(struct metaslab *msp,
    cluster_node_id_t local_id, cluster_ms_assign_t *cma);
boolean_t cluster_metaslab_owns(struct spa *spa, struct metaslab *msp);
void cluster_metaslab_group_update(struct vdev *vd, cluster_node_id_t local_id,
    cluster_ms_assign_t *cma);

#endif	/* _SYS_CLUSTER_CLUSTER_METASLAB_H */
