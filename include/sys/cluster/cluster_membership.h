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

/*
 * Cluster ZFS - Membership Header
 */

#ifndef _SYS_CLUSTER_CLUSTER_MEMBERSHIP_H
#define	_SYS_CLUSTER_CLUSTER_MEMBERSHIP_H

#include <sys/cluster/cluster_types.h>

struct spa;
struct dmu_tx;

int cluster_membership_init(cluster_membership_t *cm);
void cluster_membership_fini(cluster_membership_t *cm);
boolean_t cluster_membership_has_quorum(cluster_membership_t *cm);
void cluster_membership_recalc_quorum(cluster_membership_t *cm);
int cluster_membership_add_node(cluster_membership_t *cm,
    cluster_node_id_t id, uint64_t guid, const char *hostname,
    cluster_node_role_t role);
int cluster_membership_remove_node(cluster_membership_t *cm,
    cluster_node_id_t id);
void cluster_membership_fence_node(cluster_membership_t *cm,
    cluster_node_id_t id, const char *reason);
void cluster_membership_activate_node(cluster_membership_t *cm,
    cluster_node_id_t id);
void cluster_membership_heartbeat(cluster_membership_t *cm,
    cluster_node_id_t id);
void cluster_membership_check_heartbeats(cluster_membership_t *cm);
boolean_t cluster_paxos_prepare(cluster_membership_t *cm,
    cluster_term_t term, cluster_node_id_t proposer);
boolean_t cluster_paxos_accept(cluster_membership_t *cm,
    cluster_term_t term, cluster_node_id_t proposed_coordinator);
void cluster_paxos_commit(cluster_membership_t *cm,
    cluster_node_id_t new_coord);
int cluster_membership_write(struct spa *spa, cluster_membership_t *cm,
    struct dmu_tx *tx);
int cluster_membership_read(struct spa *spa, cluster_membership_t *cm);

/* MOS object name for membership persistence */
#define	CLUSTER_MEMBERSHIP_OBJ_NAME	"cluster_membership"

/* Cluster node lifecycle */
cluster_node_t *cluster_node_alloc(cluster_node_id_t id, uint64_t guid);
void cluster_node_free(cluster_node_t *cn);
cluster_node_t *cluster_membership_find(cluster_membership_t *cm,
    cluster_node_id_t id);
boolean_t cluster_membership_is_active(cluster_membership_t *cm,
    cluster_node_id_t id);
cluster_node_t *cluster_membership_find(cluster_membership_t *cm,
    cluster_node_id_t id);
boolean_t cluster_membership_is_active(cluster_membership_t *cm,
    cluster_node_id_t id);

#endif	/* _SYS_CLUSTER_CLUSTER_MEMBERSHIP_H */
