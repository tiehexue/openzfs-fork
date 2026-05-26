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
 * Cluster ZFS - Membership and Quorum Management
 *
 * This module implements cluster membership using a disk-backed
 * Paxos protocol. The key design principles are:
 *
 * 1. Majority quorum: A cluster can only make progress when a
 *    majority of voting members are alive and communicating.
 *
 * 2. Disk-backed state: Membership decisions and fencing records
 *    are written to reserved areas on shared disks, so they
 *    survive node failures and provide ground truth.
 *
 * 3. Coordinator election: Node 0 is the default coordinator,
 *    but any node can become coordinator through a Paxos election
 *    triggered by coordinator failure.
 *
 * 4. Incarnation numbers: Each node has a monotonically increasing
 *    incarnation number that prevents stale join requests from
 *    creating confusion.
 *
 * MEMBERSHIP LIFECYCLE
 * ====================
 *
 *   Joining:
 *     Node writes join request to disk → coordinator processes →
 *     coordinator writes new membership epoch → all nodes see
 *     new epoch and update local state → metaslabs reassigned
 *
 *   Leaving (graceful):
 *     Node writes leave request → coordinator reassigns metaslabs →
 *     coordinator writes new epoch → node shuts down cleanly
 *
 *   Failure detection:
 *     Heartbeat timeout → node marked SUSPECT → coordinator
 *     attempts contact → if no response, node marked FENCED →
 *     fencing records written to disk → node's I/O rejected
 *
 *   Coordinator failover:
 *     Coordinator heartbeat timeout → Paxos election →
 *     new coordinator announced → recovery of dead node's state
 */

#include <sys/cluster/cluster_types.h>
#include <sys/cluster/cluster_membership.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/dsl_pool.h>
#include <sys/dmu_tx.h>
#include <sys/zap.h>

/* ------------------------------------------------------------------ */
/*  Node management helpers                                            */
/* ------------------------------------------------------------------ */

cluster_node_t *
cluster_node_alloc(cluster_node_id_t id, uint64_t guid)
{
	cluster_node_t *cn;

	cn = kmem_zalloc(sizeof (*cn), KM_SLEEP);
	cn->cn_id = id;
	cn->cn_guid = guid;
	cn->cn_state = CLUSTER_NODE_JOINING;
	cn->cn_role = CLUSTER_ROLE_NONE;
	cn->cn_incarnation = 0;
	cn->cn_last_heartbeat = 0;
	cn->cn_heartbeat_timeout = MSEC2NSEC(5000); /* 5 seconds default */
	cn->cn_fenced = B_FALSE;
	cn->cn_fence_token = 0;

	return (cn);
}

void
cluster_node_free(cluster_node_t *cn)
{
	if (cn->cn_metaslab_bm != NULL) {
		kmem_free(cn->cn_metaslab_bm, cn->cn_metaslab_bm_size);
		cn->cn_metaslab_bm = NULL;
	}
	kmem_free(cn, sizeof (*cn));
}

/* ------------------------------------------------------------------ */
/*  AVL comparator for nodes by ID                                     */
/* ------------------------------------------------------------------ */

static int
cluster_node_compare(const void *a, const void *b)
{
	const cluster_node_t *na = a;
	const cluster_node_t *nb = b;

	if (na->cn_id < nb->cn_id)
		return (-1);
	if (na->cn_id > nb->cn_id)
		return (1);
	return (0);
}

/* ------------------------------------------------------------------ */
/*  Membership initialization                                          */
/* ------------------------------------------------------------------ */

int
cluster_membership_init(cluster_membership_t *cm)
{
	mutex_init(&cm->cm_lock, NULL, MUTEX_DEFAULT, NULL);
	cm->cm_epoch = 0;
	cm->cm_coordinator = CLUSTER_NODE_ID_COORDINATOR;
	cm->cm_num_active = 0;
	list_create(&cm->cm_nodes, sizeof (cluster_node_t),
	    offsetof(cluster_node_t, cn_link));
	avl_create(&cm->cm_nodes_by_id, cluster_node_compare,
	    sizeof (cluster_node_t), offsetof(cluster_node_t, cn_avl_link));

	cm->cm_has_quorum = B_FALSE;
	cm->cm_total_votes = 0;
	cm->cm_live_votes = 0;

	/* Paxos state */
	cm->cm_paxos_term = 0;
	cm->cm_paxos_promised = 0;
	cm->cm_paxos_accepted = 0;
	cm->cm_paxos_accepted_val = CLUSTER_NODE_ID_NONE;

	return (0);
}

void
cluster_membership_fini(cluster_membership_t *cm)
{
	cluster_node_t *cn;

	while ((cn = list_remove_head(&cm->cm_nodes)) != NULL)
		cluster_node_free(cn);
	list_destroy(&cm->cm_nodes);
	avl_destroy(&cm->cm_nodes_by_id);
	mutex_destroy(&cm->cm_lock);
}

/* ------------------------------------------------------------------ */
/*  Quorum calculation                                                 */
/* ------------------------------------------------------------------ */

/*
 * A cluster has quorum when a strict majority of voting members
 * are in ACTIVE state and their heartbeats are current.
 *
 * For a 2-node cluster, we require both nodes (special case).
 * For a 1-node cluster, always has quorum.
 */
boolean_t
cluster_membership_has_quorum(cluster_membership_t *cm)
{
	ASSERT(MUTEX_HELD(&cm->cm_lock));

	if (cm->cm_total_votes == 0)
		return (B_FALSE);

	if (cm->cm_total_votes == 1)
		return (cm->cm_live_votes >= 1);

	if (cm->cm_total_votes == 2)
		return (cm->cm_live_votes >= 2);

	return (cm->cm_live_votes > cm->cm_total_votes / 2);
}

void
cluster_membership_recalc_quorum(cluster_membership_t *cm)
{
	cluster_node_t *cn;

	ASSERT(MUTEX_HELD(&cm->cm_lock));

	cm->cm_total_votes = 0;
	cm->cm_live_votes = 0;

	for (cn = list_head(&cm->cm_nodes); cn != NULL;
	    cn = list_next(&cm->cm_nodes, cn)) {
		cm->cm_total_votes++;
		if (cn->cn_state == CLUSTER_NODE_ACTIVE &&
		    !cn->cn_fenced) {
			cm->cm_live_votes++;
		}
	}

	cm->cm_has_quorum = cluster_membership_has_quorum(cm);
}

/* ------------------------------------------------------------------ */
/*  Node join/leave                                                    */
/* ------------------------------------------------------------------ */

/*
 * Add a node to the cluster membership.
 * Called by the coordinator when processing a join request.
 */
int
cluster_membership_add_node(cluster_membership_t *cm, cluster_node_id_t id,
    uint64_t guid, const char *hostname, cluster_node_role_t role)
{
	cluster_node_t *cn, *existing;
	int error = 0;

	mutex_enter(&cm->cm_lock);

	/* Check for duplicate */
	existing = avl_find(&cm->cm_nodes_by_id, &id, NULL);
	if (existing != NULL) {
		/* Re-join of a previously failed node */
		if (existing->cn_state == CLUSTER_NODE_DEAD ||
		    existing->cn_state == CLUSTER_NODE_FENCED) {
			existing->cn_state = CLUSTER_NODE_JOINING;
			existing->cn_incarnation++;
			existing->cn_role = role;
			existing->cn_fenced = B_FALSE;
			existing->cn_last_heartbeat = gethrtime();
		} else {
			/* Already active - reject duplicate join */
			error = EEXIST;
			goto out;
		}
		cn = existing;
	} else {
		cn = cluster_node_alloc(id, guid);
		if (hostname != NULL)
			(void) strlcpy(cn->cn_hostname, hostname,
			    sizeof (cn->cn_hostname));
		cn->cn_role = role;
		cn->cn_incarnation = 1;
		cn->cn_last_heartbeat = gethrtime();

		list_insert_tail(&cm->cm_nodes, cn);
		avl_add(&cm->cm_nodes_by_id, cn);
	}

	/* Advance epoch */
	cm->cm_epoch++;
	cluster_membership_recalc_quorum(cm);

out:
	mutex_exit(&cm->cm_lock);
	return (error);
}

/*
 * Remove a node from the cluster membership (graceful leave).
 * Marks node as LEAVING, then transitions to DEAD after
 * resource cleanup.
 */
int
cluster_membership_remove_node(cluster_membership_t *cm, cluster_node_id_t id)
{
	cluster_node_t *cn;

	mutex_enter(&cm->cm_lock);

	cn = avl_find(&cm->cm_nodes_by_id, &id, NULL);
	if (cn == NULL) {
		mutex_exit(&cm->cm_lock);
		return (ENOENT);
	}

	cn->cn_state = CLUSTER_NODE_LEAVING;

	/*
	 * Resource cleanup happens asynchronously:
	 * - Metaslab reassignment
	 * - Lock revocation
	 * - ZIL slot release
	 * When all resources are cleaned up, the node transitions
	 * to DEAD and epoch is advanced.
	 */

	cm->cm_epoch++;
	cluster_membership_recalc_quorum(cm);

	mutex_exit(&cm->cm_lock);
	return (0);
}

/*
 * Mark a node as fenced (I/O blocked).
 * Called when heartbeat timeout occurs or manual fence requested.
 */
void
cluster_membership_fence_node(cluster_membership_t *cm, cluster_node_id_t id,
    const char *reason)
{
	cluster_node_t *cn;

	mutex_enter(&cm->cm_lock);

	cn = avl_find(&cm->cm_nodes_by_id, &id, NULL);
	if (cn == NULL) {
		mutex_exit(&cm->cm_lock);
		return;
	}

	cn->cn_state = CLUSTER_NODE_FENCED;
	cn->cn_fenced = B_TRUE;
	cn->cn_fence_token = cm->cm_epoch;

	cmn_err(CE_WARN, "cluster: node %u fenced: %s", id,
	    reason ? reason : "heartbeat timeout");

	cluster_membership_recalc_quorum(cm);

	/*
	 * If we just lost quorum, we must suspend I/O.
	 * The coordinator (or new coordinator after election)
	 * will handle recovery.
	 */
	if (!cm->cm_has_quorum) {
		cmn_err(CE_WARN, "cluster: quorum lost after fencing node %u",
		    id);
	}

	cm->cm_epoch++;
	mutex_exit(&cm->cm_lock);
}

/*
 * Activate a joining node (after metaslab assignment).
 * Transitions from JOINING to ACTIVE.
 */
void
cluster_membership_activate_node(cluster_membership_t *cm, cluster_node_id_t id)
{
	cluster_node_t *cn;

	mutex_enter(&cm->cm_lock);
	cn = avl_find(&cm->cm_nodes_by_id, &id, NULL);
	if (cn != NULL && cn->cn_state == CLUSTER_NODE_JOINING) {
		cn->cn_state = CLUSTER_NODE_ACTIVE;
		cn->cn_last_heartbeat = gethrtime();
		cluster_membership_recalc_quorum(cm);
	}
	mutex_exit(&cm->cm_lock);
}

/* ------------------------------------------------------------------ */
/*  Heartbeat processing                                               */
/* ------------------------------------------------------------------ */

/*
 * Process a received heartbeat from a node.
 * Updates the last-heartbeat timestamp and clears any SUSPECT state.
 */
void
cluster_membership_heartbeat(cluster_membership_t *cm, cluster_node_id_t id)
{
	cluster_node_t *cn;

	mutex_enter(&cm->cm_lock);
	cn = avl_find(&cm->cm_nodes_by_id, &id, NULL);
	if (cn != NULL) {
		cn->cn_last_heartbeat = gethrtime();
		if (cn->cn_state == CLUSTER_NODE_SUSPECT)
			cn->cn_state = CLUSTER_NODE_ACTIVE;
	}
	mutex_exit(&cm->cm_lock);
}

/*
 * Check for heartbeat timeouts.
 * Called periodically by the coordinator's monitoring thread.
 * Transitions ACTIVE nodes with stale heartbeats to SUSPECT,
 * and SUSPECT nodes with continued timeouts to FENCED.
 */
void
cluster_membership_check_heartbeats(cluster_membership_t *cm)
{
	cluster_node_t *cn;
	hrtime_t now = gethrtime();

	mutex_enter(&cm->cm_lock);

	for (cn = list_head(&cm->cm_nodes); cn != NULL;
	    cn = list_next(&cm->cm_nodes, cn)) {

		if (cn->cn_state != CLUSTER_NODE_ACTIVE &&
		    cn->cn_state != CLUSTER_NODE_SUSPECT)
			continue;

		hrtime_t elapsed = now - cn->cn_last_heartbeat;

		if (cn->cn_state == CLUSTER_NODE_ACTIVE &&
		    elapsed > cn->cn_heartbeat_timeout) {
			cn->cn_state = CLUSTER_NODE_SUSPECT;
			cmn_err(CE_NOTE, "cluster: node %u suspect "
			    "(no heartbeat for %lld ms)",
			    cn->cn_id, NSEC2MSEC(elapsed));
		} else if (cn->cn_state == CLUSTER_NODE_SUSPECT &&
		    elapsed > cn->cn_heartbeat_timeout * 2) {
			/*
			 * Double timeout before fencing to avoid
			 * false positives during transient delays.
			 */
			cluster_membership_fence_node(cm, cn->cn_id,
			    "heartbeat timeout");
		}
	}

	mutex_exit(&cm->cm_lock);
}

/* ------------------------------------------------------------------ */
/*  Paxos-based Coordinator Election                                   */
/* ------------------------------------------------------------------ */

/*
 * Simplified Paxos for coordinator election.
 *
 * Phase 1a (Prepare): Candidate sends Prepare(n) with term n.
 * Phase 1b (Promise): Responders promise not to accept lower terms.
 * Phase 2a (Accept):  Candidate sends Accept(n, value).
 * Phase 2b (Accepted): Responders accept the value.
 *
 * For simplicity and ZFS integration, we use a single-round
 * Paxos where the candidate with the highest GUID wins ties.
 */

/*
 * Process a Paxos Prepare message (Phase 1a).
 * Returns B_TRUE if we promise not to accept lower terms.
 */
boolean_t
cluster_paxos_prepare(cluster_membership_t *cm, cluster_term_t term,
    cluster_node_id_t proposer)
{
	boolean_t promised = B_FALSE;

	(void)proposer;
	mutex_enter(&cm->cm_lock);

	if (term > cm->cm_paxos_promised) {
		cm->cm_paxos_promised = term;
		promised = B_TRUE;
	}

	mutex_exit(&cm->cm_lock);
	return (promised);
}

/*
 * Process a Paxos Accept message (Phase 2a).
 * Returns B_TRUE if we accept the proposed coordinator.
 */
boolean_t
cluster_paxos_accept(cluster_membership_t *cm, cluster_term_t term,
    cluster_node_id_t proposed_coordinator)
{
	boolean_t accepted = B_FALSE;

	mutex_enter(&cm->cm_lock);

	if (term >= cm->cm_paxos_promised) {
		cm->cm_paxos_term = term;
		cm->cm_paxos_promised = term;
		cm->cm_paxos_accepted = term;
		cm->cm_paxos_accepted_val = proposed_coordinator;
		accepted = B_TRUE;
	}

	mutex_exit(&cm->cm_lock);
	return (accepted);
}

/*
 * Finalize a Paxos election: update the coordinator.
 * Called when a majority of nodes have accepted.
 */
void
cluster_paxos_commit(cluster_membership_t *cm, cluster_node_id_t new_coord)
{
	cluster_node_t *cn;

	mutex_enter(&cm->cm_lock);

	cm->cm_coordinator = new_coord;
	cm->cm_epoch++;

	/* Update all nodes' roles */
	for (cn = list_head(&cm->cm_nodes); cn != NULL;
	    cn = list_next(&cm->cm_nodes, cn)) {
		if (cn->cn_id == new_coord)
			cn->cn_role = CLUSTER_ROLE_COORDINATOR;
		else if (cn->cn_state == CLUSTER_NODE_ACTIVE)
			cn->cn_role = CLUSTER_ROLE_PARTICIPANT;
	}

	cmn_err(CE_NOTE, "cluster: new coordinator elected: node %u",
	    new_coord);

	mutex_exit(&cm->cm_lock);
}

/* ------------------------------------------------------------------ */
/*  Membership Persistence (disk-backed)                               */
/* ------------------------------------------------------------------ */

/*
 * Write the current membership state to the MOS.
 * This provides ground truth that survives node failures.
 *
 * The membership is stored as a ZAP object:
 *   key "epoch"          → uint64 (current epoch)
 *   key "coordinator"    → uint64 (coordinator node ID)
 *   key "num_nodes"      → uint64 (node count)
 *   key "node_N_id"      → uint64 (node ID)
 *   key "node_N_guid"    → uint64 (node GUID)
 *   key "node_N_state"   → uint64 (node state)
 *   key "node_N_role"    → uint64 (node role)
 *   key "node_N_incarn"  → uint64 (incarnation number)
 *   key "node_N_fenced"  → uint64 (fenced flag)
 */
#define	CLUSTER_MEMBERSHIP_OBJ_NAME	"cluster_membership"

int
cluster_membership_write(spa_t *spa, cluster_membership_t *cm, dmu_tx_t *tx)
{
	dsl_pool_t *dp = spa->spa_dsl_pool;
	objset_t *mos = dp->dp_meta_objset;
	cluster_node_t *cn;
	uint64_t obj;
	int error;

	ASSERT(dmu_tx_is_syncing(tx));

	/* Find or create the membership ZAP object */
	error = zap_lookup(mos, DMU_POOL_DIRECTORY_OBJECT,
	    CLUSTER_MEMBERSHIP_OBJ_NAME, sizeof (uint64_t), 1, &obj);
	if (error == ENOENT) {
		obj = zap_create(mos, DMU_OTN_ZAP_METADATA,
		    DMU_OT_NONE, 0, tx);
		error = zap_add(mos, DMU_POOL_DIRECTORY_OBJECT,
		    CLUSTER_MEMBERSHIP_OBJ_NAME, sizeof (uint64_t), 1,
		    &obj, tx);
	}
	if (error != 0)
		return (error);

	mutex_enter(&cm->cm_lock);

	/* Write global state */
	(void) zap_update(mos, obj, "epoch", sizeof (uint64_t), 1,
	    &cm->cm_epoch, tx);
	(void) zap_update(mos, obj, "coordinator", sizeof (uint64_t), 1,
	    &cm->cm_coordinator, tx);
	(void) zap_update(mos, obj, "num_nodes", sizeof (uint64_t), 1,
	    &cm->cm_total_votes, tx);

	/* Write per-node state */
	uint64_t idx = 0;
	for (cn = list_head(&cm->cm_nodes); cn != NULL;
	    cn = list_next(&cm->cm_nodes, cn), idx++) {
		char key[64];

		(void) snprintf(key, sizeof (key), "node_%llu_id",
		    (u_longlong_t)idx);
		(void) zap_update(mos, obj, key, sizeof (uint64_t), 1,
		    &cn->cn_id, tx);

		(void) snprintf(key, sizeof (key), "node_%llu_guid",
		    (u_longlong_t)idx);
		(void) zap_update(mos, obj, key, sizeof (uint64_t), 1,
		    &cn->cn_guid, tx);

		(void) snprintf(key, sizeof (key), "node_%llu_state",
		    (u_longlong_t)idx);
		uint64_t state = cn->cn_state;
		(void) zap_update(mos, obj, key, sizeof (uint64_t), 1,
		    &state, tx);

		(void) snprintf(key, sizeof (key), "node_%llu_role",
		    (u_longlong_t)idx);
		uint64_t role = cn->cn_role;
		(void) zap_update(mos, obj, key, sizeof (uint64_t), 1,
		    &role, tx);

		(void) snprintf(key, sizeof (key), "node_%llu_incarn",
		    (u_longlong_t)idx);
		(void) zap_update(mos, obj, key, sizeof (uint64_t), 1,
		    &cn->cn_incarnation, tx);

		(void) snprintf(key, sizeof (key), "node_%llu_fenced",
		    (u_longlong_t)idx);
		uint64_t fenced = cn->cn_fenced ? 1 : 0;
		(void) zap_update(mos, obj, key, sizeof (uint64_t), 1,
		    &fenced, tx);
	}

	mutex_exit(&cm->cm_lock);
	return (0);
}

/*
 * Read the membership state from MOS.
 * Called during pool import to recover cluster state.
 */
int
cluster_membership_read(spa_t *spa, cluster_membership_t *cm)
{
	dsl_pool_t *dp = spa->spa_dsl_pool;
	objset_t *mos = dp->dp_meta_objset;
	uint64_t obj, num_nodes;
	int error;

	error = zap_lookup(mos, DMU_POOL_DIRECTORY_OBJECT,
	    CLUSTER_MEMBERSHIP_OBJ_NAME, sizeof (uint64_t), 1, &obj);
	if (error != 0)
		return (error);	/* No cluster config = single-node */

	error = zap_lookup(mos, obj, "epoch", sizeof (uint64_t), 1,
	    &cm->cm_epoch);
	if (error != 0)
		return (error);

	error = zap_lookup(mos, obj, "coordinator", sizeof (uint64_t), 1,
	    &cm->cm_coordinator);
	if (error != 0)
		return (error);

	error = zap_lookup(mos, obj, "num_nodes", sizeof (uint64_t), 1,
	    &num_nodes);
	if (error != 0)
		return (error);

	/* Read per-node state */
	for (uint64_t idx = 0; idx < num_nodes; idx++) {
		char key[64];
		uint64_t id, guid, state, role, incarnation, fenced;

		(void) snprintf(key, sizeof (key), "node_%llu_id",
		    (u_longlong_t)idx);
		error = zap_lookup(mos, obj, key, sizeof (uint64_t), 1, &id);
		if (error != 0)
			break;

		(void) snprintf(key, sizeof (key), "node_%llu_guid",
		    (u_longlong_t)idx);
		error = zap_lookup(mos, obj, key, sizeof (uint64_t), 1, &guid);
		if (error != 0)
			break;

		(void) snprintf(key, sizeof (key), "node_%llu_state",
		    (u_longlong_t)idx);
		(void) zap_lookup(mos, obj, key, sizeof (uint64_t), 1, &state);

		(void) snprintf(key, sizeof (key), "node_%llu_role",
		    (u_longlong_t)idx);
		(void) zap_lookup(mos, obj, key, sizeof (uint64_t), 1, &role);

		(void) snprintf(key, sizeof (key), "node_%llu_incarn",
		    (u_longlong_t)idx);
		(void) zap_lookup(mos, obj, key, sizeof (uint64_t), 1,
		    &incarnation);

		(void) snprintf(key, sizeof (key), "node_%llu_fenced",
		    (u_longlong_t)idx);
		(void) zap_lookup(mos, obj, key, sizeof (uint64_t), 1,
		    &fenced);

		cluster_node_t *cn = cluster_node_alloc(id, guid);
		cn->cn_state = state;
		cn->cn_role = role;
		cn->cn_incarnation = incarnation;
		cn->cn_fenced = (fenced != 0);
		cn->cn_last_heartbeat = 0;

		mutex_enter(&cm->cm_lock);
		/*
		 * Only add if not already present.  cluster_spa_init()
		 * may have already added the local node before we read
		 * back membership from MOS on re-import.  If the local
		 * node already exists, keep its ACTIVE state from init.
		 */
		cluster_node_t *existing = avl_find(&cm->cm_nodes_by_id,
		    cn, NULL);
		if (existing == NULL) {
			list_insert_tail(&cm->cm_nodes, cn);
			avl_add(&cm->cm_nodes_by_id, cn);
		} else {
			/* Update role/incarnation but keep ACTIVE state */
			existing->cn_role = cn->cn_role;
			existing->cn_incarnation = cn->cn_incarnation;
			existing->cn_fenced = cn->cn_fenced;
			cluster_node_free(cn);
		}
		mutex_exit(&cm->cm_lock);
	}

	mutex_enter(&cm->cm_lock);
	cluster_membership_recalc_quorum(cm);
	mutex_exit(&cm->cm_lock);

	return (0);
}

/* ------------------------------------------------------------------ */
/*  Lookup helpers                                                     */
/* ------------------------------------------------------------------ */

cluster_node_t *
cluster_membership_find(cluster_membership_t *cm, cluster_node_id_t id)
{
	cluster_node_t search = { .cn_id = id };
	return (avl_find(&cm->cm_nodes_by_id, &search, NULL));
}

boolean_t
cluster_membership_is_active(cluster_membership_t *cm, cluster_node_id_t id)
{
	cluster_node_t *cn;

	mutex_enter(&cm->cm_lock);
	cn = cluster_membership_find(cm, id);
	boolean_t active = (cn != NULL && cn->cn_state == CLUSTER_NODE_ACTIVE);
	mutex_exit(&cm->cm_lock);
	return (active);
}
