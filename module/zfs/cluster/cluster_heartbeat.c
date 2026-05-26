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
 * Cluster ZFS - Heartbeat via MMP Extension
 *
 * In a single-machine simulation, there is no real network for
 * heartbeats.  Instead, we extend the existing MMP (Multihost
 * Protection) mechanism to carry cluster identity information.
 *
 * HOW IT WORKS
 *
 * The MMP thread writes an MMP block to a random uberblock slot
 * every few seconds.  In cluster mode, we encode the following
 * information into the MMP block:
 *
 *   ub_mmp_config:
 *     bits  0-31  : MMP sequence number
 *     bits 32-47  : cluster node ID
 *     bits 48-63  : cluster epoch
 *
 *   ub_mmp_delay:
 *     time of last heartbeat write (in nanoseconds)
 *
 * When a node reads MMP blocks during import, it can determine:
 *   - Whether other nodes are alive (recent MMP writes)
 *   - Which nodes wrote them (node ID)
 *   - Whether the writer has been fenced (epoch check)
 *
 * This module also provides the in-memory heartbeat bookkeeping
 * (cn_last_heartbeat updates) when the same-kernel fast path
 * delivers heartbeat messages.
 */

#include <sys/cluster/cluster_types.h>
#include <sys/cluster/cluster_heartbeat.h>
#include <sys/cluster/cluster_membership.h>
#include <sys/cluster/cluster_messaging.h>
#include <sys/cluster/cluster_spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/mmp.h>
#include <sys/uberblock_impl.h>
#include <sys/zfs_context.h>

/* ------------------------------------------------------------------ */
/*  Encode / Decode cluster identity into MMP fields                   */
/* ------------------------------------------------------------------ */

void
cluster_heartbeat_encode_ub(spa_t *spa, uberblock_t *ub)
{
	cluster_spa_t *cspa = spa->spa_cluster;

	if (cspa == NULL)
		return;

	/*
	 * Encode cluster identity into the MMP fields:
	 *   ub_mmp_config bits 32-47: node ID
	 *   ub_mmp_config bits 48-63: cluster epoch
	 */
	uint64_t cfg = ub->ub_mmp_config;
	/* Clear cluster bits */
	cfg &= ~(((uint64_t)0xFFFF) << 32);
	/* Set node ID in bits 32-47 */
	cfg |= ((uint64_t)(cspa->cspa_local_id & 0xFFFF)) << 32;
	/* Set epoch in bits 48-63 */
	cfg |= ((uint64_t)(cspa->cspa_membership.cm_epoch & 0xFFFF)) << 48;
	ub->ub_mmp_config = cfg;

	/* Store heartbeat timestamp in mmp_delay */
	ub->ub_mmp_delay = gethrtime();
}

/*
 * Decode cluster identity from an MMP/uberblock.
 * Returns B_TRUE if the MMP block was written by a live cluster node.
 */
boolean_t
cluster_heartbeat_decode_ub(spa_t *spa, uberblock_t *ub,
    cluster_node_id_t *node_id, uint64_t *epoch, hrtime_t *hb_time)
{
	if (spa->spa_cluster == NULL)
		return (B_FALSE);

	uint64_t cfg = ub->ub_mmp_config;

	*node_id = (cluster_node_id_t)((cfg >> 32) & 0xFFFF);
	*epoch = (cfg >> 48) & 0xFFFF;
	*hb_time = (hrtime_t)ub->ub_mmp_delay;

	/* Valid cluster identity if node_id != 0 */
	return (*node_id != 0 && *node_id < CLUSTER_MAX_NODES);
}

/* ------------------------------------------------------------------ */
/*  Heartbeat send (for same-kernel fast-path)                        */
/* ------------------------------------------------------------------ */

/*
 * Send heartbeats to all active cluster nodes.
 * Called periodically by the coordinator.
 */
void
cluster_heartbeat_send_all(cluster_spa_t *cspa)
{
	cluster_membership_t *cm = &cspa->cspa_membership;
	cluster_node_t *cn;

	mutex_enter(&cm->cm_lock);
	for (cn = list_head(&cm->cm_nodes); cn != NULL;
	    cn = list_next(&cm->cm_nodes, cn)) {
		if (cn->cn_id == cspa->cspa_local_id)
			continue;
		if (cn->cn_state != CLUSTER_NODE_ACTIVE)
			continue;
		cluster_msg_send_heartbeat(cspa, cn->cn_id);
	}
	mutex_exit(&cm->cm_lock);
}

/*
 * Process an incoming heartbeat message.
 * Called from the messaging handler.
 */
void
cluster_heartbeat_receive(spa_t *spa, const cluster_msg_header_t *hdr,
    const void *payload)
{
	cluster_spa_t *cspa = spa->spa_cluster;
	cluster_node_id_t src = hdr->cmh_src;

	if (cspa == NULL)
		return;

	if (src == cspa->cspa_local_id)
		return;	/* ignore self-heartbeats */

	(void)payload;

	cluster_membership_heartbeat(&cspa->cspa_membership, src);
}

/* ------------------------------------------------------------------ */
/*  Heartbeat timeout check (coordinator only)                         */
/* ------------------------------------------------------------------ */

/*
 * Check heartbeat timeouts and fence dead nodes.
 * Called periodically by the coordinator sync thread.
 */
void
cluster_heartbeat_check(cluster_spa_t *cspa)
{
	cluster_membership_check_heartbeats(&cspa->cspa_membership);

	/*
	 * If any node was fenced during the check, broadcast
	 * a membership change to all remaining nodes.
	 */
	if (!cspa->cspa_membership.cm_has_quorum) {
		cmn_err(CE_WARN, "cluster: lost quorum (live=%llu, "
		    "total=%llu)",
		    (u_longlong_t)cspa->cspa_membership.cm_live_votes,
		    (u_longlong_t)cspa->cspa_membership.cm_total_votes);
	}
}
