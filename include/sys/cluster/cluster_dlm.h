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

#ifndef _SYS_CLUSTER_CLUSTER_DLM_H
#define	_SYS_CLUSTER_CLUSTER_DLM_H

#include <sys/cluster/cluster_types.h>

int cluster_dlm_init(cluster_dlm_t *dlm);
void cluster_dlm_fini(cluster_dlm_t *dlm);
int cluster_dlm_request(cluster_dlm_t *dlm, cluster_node_id_t requester,
    cluster_lock_type_t type, cluster_lock_resource_t resource,
    uint64_t objset, uint64_t object, uint64_t txg,
    void (*callback)(void *), void *callback_arg);
int cluster_dlm_release(cluster_dlm_t *dlm, cluster_node_id_t holder,
    cluster_lock_resource_t resource, uint64_t objset, uint64_t object);
void cluster_dlm_release_all(cluster_dlm_t *dlm, cluster_node_id_t node_id);
void cluster_dlm_release_txg(cluster_dlm_t *dlm, uint64_t txg);
boolean_t cluster_dlm_is_locked(cluster_dlm_t *dlm, cluster_node_id_t node_id,
    cluster_lock_resource_t resource, uint64_t objset, uint64_t object);
boolean_t cluster_dlm_has_conflict(cluster_dlm_t *dlm,
    cluster_node_id_t node_id, cluster_lock_resource_t resource,
    uint64_t objset, uint64_t object, cluster_lock_type_t requested_type);

#endif	/* _SYS_CLUSTER_CLUSTER_DLM_H */
