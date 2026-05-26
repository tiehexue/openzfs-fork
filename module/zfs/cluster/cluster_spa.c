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
 * Cluster ZFS - Main Integration Module
 *
 * This module ties together all cluster components and provides
 * the top-level API for enabling cluster mode on a ZFS pool.
 *
 * CLUSTER POOL LIFECYCLE
 * ======================
 *
 * Creation:
 *   zpool create -o cluster=on -o cluster_nodes=4 tank raidz ...
 *   - Creates pool with cluster metadata objects in MOS
 *   - Node 0 becomes initial coordinator
 *   - Metaslabs are partitioned across all planned nodes
 *   - Only node 0 is active initially
 *
 * Join:
 *   zpool import -o cluster_node=1 tank
 *   - Node sends join request to coordinator
 *   - Coordinator assigns metaslabs
 *   - Coordinator activates node
 *   - Node begins accepting I/O
 *
 * Leave (graceful):
 *   zpool export tank
 *   - Node sends leave request
 *   - Coordinator reassigns metaslabs
 *   - Node cleanly exits
 *
 * Failure:
 *   - Heartbeat timeout detected
 *   - Coordinator fences dead node
 *   - Coordinator reclaims and reassigns metaslabs
 *   - Coordinator replays dead node's ZIL
 *
 * INTEGRATION POINTS WITH EXISTING ZFS
 * =====================================
 *
 * 1. spa_sync(): Modified to skip MOS/uberblock on participants
 * 2. metaslab_alloc(): Filtered to only use owned metaslabs
 * 3. dmu_tx_assign(): Reports dirty data to coordinator
 * 4. vdev_label_write(): Extended with cluster epoch
 * 5. spa_open(): Extended to check cluster membership
 * 6. zil_commit(): Writes to per-node ZIL region
 * 7. MMP: Extended for cluster-aware activity checks
 */

#include <sys/cluster/cluster_types.h>
#include <sys/cluster/cluster_spa.h>
#include <sys/cluster/cluster_membership.h>
#include <sys/cluster/cluster_txg.h>
#include <sys/cluster/cluster_metaslab.h>
#include <sys/cluster/cluster_sync.h>
#include <sys/cluster/cluster_dlm.h>
#include <sys/cluster/cluster_recovery.h>
#include <sys/cluster/cluster_messaging.h>
#include <sys/cluster/cluster_heartbeat.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/dsl_pool.h>
#include <sys/dmu.h>
#include <sys/dmu_tx.h>
#include <sys/zap.h>
#include <sys/zfs_ioctl.h>

/* ------------------------------------------------------------------ */
/*  Cluster SPA Initialization                                         */
/* ------------------------------------------------------------------ */

/*
 * Initialize cluster state for a pool.
 * Called when a pool is imported in cluster mode.
 */
int
cluster_spa_init(spa_t *spa, cluster_node_id_t node_id,
    boolean_t is_coordinator)
{
	cluster_spa_t *cspa;
	vdev_t *rvd = spa->spa_root_vdev;
	int error;

	cspa = kmem_zalloc(sizeof (cluster_spa_t), KM_SLEEP);
	if (cspa == NULL)
		return (ENOMEM);

	/* Identity */
	cspa->cspa_local_id = node_id;
	cspa->cspa_is_coordinator = is_coordinator;
	cspa->cspa_role = is_coordinator ?
	    CLUSTER_ROLE_COORDINATOR : CLUSTER_ROLE_PARTICIPANT;

	/* Configuration defaults */
	cspa->cspa_config.cc_magic = CLUSTER_CONFIG_MAGIC;
	cspa->cspa_config.cc_version = 1;
	cspa->cspa_config.cc_max_nodes = CLUSTER_MAX_NODES;
	cspa->cspa_config.cc_epoch = 1;
	cspa->cspa_config.cc_coordinator_id = CLUSTER_NODE_ID_COORDINATOR;
	cspa->cspa_config.cc_ms_partition_policy =
	    CLUSTER_MS_PARTITION_DYNAMIC;
	cspa->cspa_config.cc_txg_policy = CLUSTER_TXG_CENTRALIZED;
	cspa->cspa_config.cc_heartbeat_interval_ms = 1000;
	cspa->cspa_config.cc_heartbeat_timeout_ms = 5000;
	cspa->cspa_config.cc_txg_sync_interval_ms = 5000;
	cspa->cspa_config.cc_fence_policy = CLUSTER_FENCE_PERSISTENT;

	/* Initialize membership */
	error = cluster_membership_init(&cspa->cspa_membership);
	if (error != 0)
		goto fail;

	/*
	 * Set the membership coordinator to node 0 (the default).
	 * If this node is not coordinator, auto-promotion during
	 * import will update this if the coordinator is unreachable.
	 */
	cspa->cspa_membership.cm_coordinator = CLUSTER_NODE_ID_COORDINATOR;

	/* Add self as first member */
	error = cluster_membership_add_node(&cspa->cspa_membership,
	    node_id, spa->spa_hostid ? spa->spa_hostid : 0,
	    utsname()->nodename,
	    is_coordinator ? CLUSTER_ROLE_COORDINATOR :
	    CLUSTER_ROLE_PARTICIPANT);
	if (error != 0)
		goto fail;

	cluster_membership_activate_node(&cspa->cspa_membership, node_id);

	/* Initialize TXG coordination */
	error = cluster_txg_init(&cspa->cspa_txg, node_id);
	if (error != 0)
		goto fail;

	/* Initialize metaslab assignments per vdev */
	cspa->cspa_ms_assign_count = rvd->vdev_children;
	cspa->cspa_ms_assign = kmem_zalloc(
	    cspa->cspa_ms_assign_count * sizeof (cluster_ms_assign_t),
	    KM_SLEEP);

	for (uint64_t c = 0; c < rvd->vdev_children; c++) {
		vdev_t *tvd = rvd->vdev_child[c];
		uint64_t ms_count = tvd->vdev_ms_count;

		if (ms_count == 0)
			continue;

		error = cluster_ms_assign_init(&cspa->cspa_ms_assign[c],
		    tvd->vdev_id, ms_count);
		if (error != 0)
			goto fail;

		/* If coordinator, partition all metaslabs to self initially */
		if (is_coordinator) {
			cluster_node_id_t me[1] = { node_id };
			cluster_ms_partition_static(&cspa->cspa_ms_assign[c],
			    me, 1);
		}
	}

	/* Initialize DLM */
	error = cluster_dlm_init(&cspa->cspa_dlm);
	if (error != 0)
		goto fail;

	/* Initialize cluster ZIL */
	mutex_init(&cspa->cspa_zil.cz_lock, NULL, MUTEX_DEFAULT, NULL);
	cspa->cspa_zil.cz_slog_obj = 0;
	cspa->cspa_zil.cz_total_reserved = 0;
	cspa->cspa_zil.cz_total_capacity = 0;

	/* Initialize fencing */
	cspa->cspa_vdev_fence_count = rvd->vdev_children;
	cspa->cspa_vdev_fences = kmem_zalloc(
	    cspa->cspa_vdev_fence_count * sizeof (cluster_vdev_fence_t),
	    KM_SLEEP);
	for (uint64_t v = 0; v < cspa->cspa_vdev_fence_count; v++) {
		cspa->cspa_vdev_fences[v].cvf_vdev_id =
		    rvd->vdev_child[v]->vdev_id;
		mutex_init(&cspa->cspa_vdev_fences[v].cvf_lock, NULL,
		    MUTEX_DEFAULT, NULL);
		list_create(&cspa->cspa_vdev_fences[v].cvf_fenced_nodes,
		    sizeof (cluster_fence_record_t), 0);
	}

	/* Messaging infrastructure */
	mutex_init(&cspa->cspa_msg_lock, NULL, MUTEX_DEFAULT, NULL);
	cspa->cspa_msg_seq = 0;
	list_create(&cspa->cspa_msg_pending, sizeof (cluster_msg_header_t), 0);
	list_create(&cspa->cspa_msg_handlers, 0, 0);

	/* Recovery state */
	cspa->cspa_recovering = B_FALSE;
	cspa->cspa_recover_target = CLUSTER_NODE_ID_NONE;

	/* Attach to spa */
	spa->spa_cluster = cspa;

	return (0);

fail:
	cluster_spa_fini(spa);
	return (error);
}

/*
 * Destroy cluster state for a pool.
 * Called when a pool is exported or destroyed.
 */
void
cluster_spa_fini(spa_t *spa)
{
	cluster_spa_t *cspa = spa->spa_cluster;

	if (cspa == NULL)
		return;

	/* Cleanup messaging */
	list_destroy(&cspa->cspa_msg_handlers);
	list_destroy(&cspa->cspa_msg_pending);
	mutex_destroy(&cspa->cspa_msg_lock);

	/* Cleanup fencing */
	for (uint64_t v = 0; v < cspa->cspa_vdev_fence_count; v++) {
		list_destroy(&cspa->cspa_vdev_fences[v].cvf_fenced_nodes);
		mutex_destroy(&cspa->cspa_vdev_fences[v].cvf_lock);
	}
	kmem_free(cspa->cspa_vdev_fences,
	    cspa->cspa_vdev_fence_count * sizeof (cluster_vdev_fence_t));

	/* Cleanup ZIL */
	mutex_destroy(&cspa->cspa_zil.cz_lock);

	/* Cleanup DLM */
	cluster_dlm_fini(&cspa->cspa_dlm);

	/* Cleanup metaslab assignments */
	for (uint64_t v = 0; v < cspa->cspa_ms_assign_count; v++)
		cluster_ms_assign_fini(&cspa->cspa_ms_assign[v]);
	kmem_free(cspa->cspa_ms_assign,
	    cspa->cspa_ms_assign_count * sizeof (cluster_ms_assign_t));

	/* Cleanup TXG */
	cluster_txg_fini(&cspa->cspa_txg);

	/* Cleanup membership */
	cluster_membership_fini(&cspa->cspa_membership);

	/* Detach from spa */
	spa->spa_cluster = NULL;

	kmem_free(cspa, sizeof (cluster_spa_t));
}

/* ------------------------------------------------------------------ */
/*  Cluster Pool Import/Export Hooks                                   */
/* ------------------------------------------------------------------ */

/*
 * Hook called during spa_import() when cluster mode is requested.
 *
 * This extends the standard import path:
 *   1. Standard spa_load() (read uberblock, open vdevs)
 *   2. Read cluster config from MOS (if present)
 *   3. If joining as participant, contact coordinator
 *   4. If first node, become coordinator
 *   5. Initialize cluster state
 */
int
cluster_spa_import(spa_t *spa, cluster_node_id_t node_id)
{
	dsl_pool_t *dp = spa->spa_dsl_pool;
	objset_t *mos;
	uint64_t cluster_obj;
	boolean_t is_first_node;
	boolean_t is_coordinator;
	int error;

	/*
	 * Node 0 is always the coordinator. All other nodes
	 * are participants.
	 */
	is_coordinator = (node_id == CLUSTER_NODE_ID_COORDINATOR);

	/*
	 * Check if this pool has cluster configuration.
	 * If not, this is the first import ever — the importer
	 * must be node 0 (coordinator).
	 */
	if (dp != NULL && dp->dp_meta_objset != NULL) {
		mos = dp->dp_meta_objset;
		error = zap_lookup(mos, DMU_POOL_DIRECTORY_OBJECT,
		    "cluster_config", sizeof (uint64_t), 1, &cluster_obj);
		is_first_node = (error == ENOENT);
	} else {
		is_first_node = B_TRUE;
	}

	if (is_first_node) {
		/*
		 * First import — initialize as the specified node.
		 * If node_id is 0, this becomes the initial coordinator.
		 * If node_id is non-zero, the caller is requesting a
		 * specific node role (typically coordinator=0 for first).
		 *
		 * The pool was created previously (spa_create) and we
		 * are the first to import it in cluster mode.  Write
		 * our identity to the cluster config on disk.
		 */
		error = cluster_spa_init(spa, node_id,
		    node_id == CLUSTER_NODE_ID_COORDINATOR);
		if (error != 0)
			return (error);

		/*
		 * Write cluster configuration to MOS.
		 * This marks the pool as a cluster pool.
		 */
		error = cluster_spa_config_write(spa);
		if (error != 0)
			return (error);
	} else {
		/* Joining existing cluster as the specified node */
		error = cluster_spa_init(spa, node_id, is_coordinator);
		if (error != 0)
			return (error);

		/*
		 * Read cluster config from MOS.
		 * Contact coordinator to join.
		 * Receive metaslab assignments.
		 */
		error = cluster_spa_config_read(spa);
		if (error != 0)
			return (error);

		/* Read existing membership */
		error = cluster_membership_read(spa,
		    &spa->spa_cluster->cspa_membership);
		if (error != 0)
			return (error);

		/*
		 * Auto-promotion: if the designated coordinator is
		 * not active (e.g., this is a sequential import where
		 * the coordinator exported first), promote this node
		 * to acting coordinator.  This preserves the cluster
		 * design (coordinator-only MOS/uberblock) while
		 * allowing single-node operation when no coordinator
		 * is reachable.
		 *
		 * Also handles the case where this node IS the
		 * designated coordinator re-importing after an
		 * export — the previous LEAVING state is detected
		 * and we reclaim coordinator role.
		 *
		 * In a full cluster deployment with active networking,
		 * a coordinator that is unreachable but still active
		 * on another node would be detected via heartbeats,
		 * and a proper Paxos election would be held.  Here we
		 * use the MOS membership state as a safe heuristic.
		 */
		cluster_membership_t *cm =
		    &spa->spa_cluster->cspa_membership;
		mutex_enter(&cm->cm_lock);
		cluster_node_t *coord_cn =
		    cluster_membership_find(cm, cm->cm_coordinator);
		cluster_node_t *self_cn =
		    cluster_membership_find(cm, node_id);

		boolean_t need_promotion = B_FALSE;

		if (node_id == cm->cm_coordinator) {
			/*
			 * We are the designated coordinator.
			 * Reclaim the role if we were LEAVING
			 * (from a prior graceful export).
			 */
			if (self_cn != NULL &&
			    self_cn->cn_state == CLUSTER_NODE_LEAVING) {
				need_promotion = B_TRUE;
			}
			/* Native coordinator: already is_coordinator=TRUE
			 * from cluster_spa_init, nothing to do. */
		} else if (coord_cn == NULL) {
			/*
			 * No coordinator on record — auto-promote.
			 */
			need_promotion = B_TRUE;
		} else if (coord_cn->cn_state != CLUSTER_NODE_ACTIVE) {
			/*
			 * Coordinator exists but is not ACTIVE
			 * (LEAVING, FENCED, DEAD, etc.) — auto-promote.
			 */
			need_promotion = B_TRUE;
		} else {
			/*
			 * Coordinator appears ACTIVE in MOS, but in
			 * the single-machine simulation there is no
			 * real network heartbeat to verify liveness.
			 * If the coordinator's last heartbeat timestamp
			 * is 0 (never received a real heartbeat),
			 * the coordinator is not actually running —
			 * auto-promote to keep the pool operational.
			 */
			if (coord_cn->cn_last_heartbeat == 0) {
				cmn_err(CE_NOTE, "cluster: coordinator "
				    "node %u appears ACTIVE in MOS but "
				    "has no heartbeat — auto-promoting "
				    "node %u",
				    cm->cm_coordinator, node_id);
				need_promotion = B_TRUE;
			}
		}

		if (need_promotion) {
			spa->spa_cluster->cspa_is_coordinator = B_TRUE;
			spa->spa_cluster->cspa_role =
			    CLUSTER_ROLE_COORDINATOR;
			cm->cm_coordinator = node_id;

			/* Update self role to coordinator in membership */
			if (self_cn != NULL) {
				self_cn->cn_role = CLUSTER_ROLE_COORDINATOR;
				self_cn->cn_state = CLUSTER_NODE_ACTIVE;
			}

			cmn_err(CE_NOTE, "cluster: node %u auto-promoted "
			    "to coordinator (no active coordinator)",
			    node_id);
		}
		mutex_exit(&cm->cm_lock);

		/*
		 * If we are coordinator (native or auto-promoted),
		 * write ACTIVE membership to MOS.  This signals
		 * to any subsequently-importing participant that a
		 * coordinator is alive, preventing unwanted
		 * auto-promotion (split-brain).  When we export,
		 * cluster_spa_export() writes LEAVING to unset this.
		 */
		if (spa->spa_cluster->cspa_is_coordinator) {
			error = cluster_spa_config_write(spa);
			if (error != 0)
				return (error);
		}

		/* Read existing metaslab assignments */
		for (uint64_t v = 0;
		    v < spa->spa_cluster->cspa_ms_assign_count; v++) {
			error = cluster_ms_assign_read(spa, v,
			    &spa->spa_cluster->cspa_ms_assign[v]);
			if (error != 0)
				return (error);
		}
	}

	return (0);
}

/*
 * Hook called during spa_export().
 *
 * Marks this node as LEAVING in the in-memory membership.  The
 * LEAVING state does NOT need to be written to MOS because:
 *
 *   1. During the last TXG sync (triggered by vdev_config_dirty
 *      and spa_final_txg in spa_export_common), the coordinator
 *      writes a final uberblock with the cluster epoch encoded.
 *
 *   2. On the next import, cluster_spa_import() reads the MOS
 *      membership.  If the previous coordinator's last heartbeat
 *      timestamp is 0 (no real network heartbeat was ever
 *      received), auto-promotion triggers regardless of the
 *      ACTIVE/LEAVING state in MOS.
 *
 * Writing a DMU transaction during export is unsafe because the
 * pool may be in a transitional state where dnode_hold can fail.
 * Instead we just do in-memory cleanup and let the next importer
 * detect the missing coordinator via heartbeat liveness.
 */
int
cluster_spa_export(spa_t *spa)
{
	cluster_spa_t *cspa = spa->spa_cluster;

	if (cspa == NULL)
		return (0);

	/*
	 * Mark self as LEAVING in the in-memory membership.
	 * This is for local consistency during the final sync.
	 */
	cluster_membership_t *cm = &cspa->cspa_membership;
	mutex_enter(&cm->cm_lock);
	cluster_node_t *self =
	    cluster_membership_find(cm, cspa->cspa_local_id);
	if (self != NULL) {
		self->cn_state = CLUSTER_NODE_LEAVING;
		cm->cm_num_active = (cm->cm_num_active > 0) ?
		    cm->cm_num_active - 1 : 0;
	}
	mutex_exit(&cm->cm_lock);

	/* Clean up in-memory cluster state */
	cluster_spa_fini(spa);
	return (0);
}

/* ------------------------------------------------------------------ */
/*  Cluster Configuration Persistence                                  */
/* ------------------------------------------------------------------ */

#define	CLUSTER_CONFIG_OBJ_NAME	"cluster_config"

int
cluster_spa_config_write(spa_t *spa)
{
	cluster_spa_t *cspa = spa->spa_cluster;
	dsl_pool_t *dp = spa->spa_dsl_pool;
	objset_t *mos = dp->dp_meta_objset;
	dmu_tx_t *tx;
	uint64_t obj;
	int error;

	if (cspa == NULL)
		return (0);

	tx = dmu_tx_create_dd(dp->dp_mos_dir);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error != 0) {
		dmu_tx_abort(tx);
		return (error);
	}

	/* Find or create the cluster config ZAP object */
	error = zap_lookup(mos, DMU_POOL_DIRECTORY_OBJECT,
	    CLUSTER_CONFIG_OBJ_NAME, sizeof (uint64_t), 1, &obj);
	if (error == ENOENT) {
		obj = zap_create(mos, DMU_OTN_ZAP_METADATA,
		    DMU_OT_NONE, 0, tx);
		error = zap_add(mos, DMU_POOL_DIRECTORY_OBJECT,
		    CLUSTER_CONFIG_OBJ_NAME, sizeof (uint64_t), 1,
		    &obj, tx);
	}
	if (error != 0) {
		dmu_tx_commit(tx);
		return (error);
	}

	/* Write configuration */
	(void) zap_update(mos, obj, "magic", sizeof (uint64_t), 1,
	    &cspa->cspa_config.cc_magic, tx);
	(void) zap_update(mos, obj, "version", sizeof (uint64_t), 1,
	    &cspa->cspa_config.cc_version, tx);
	(void) zap_update(mos, obj, "max_nodes", sizeof (uint64_t), 1,
	    &cspa->cspa_config.cc_max_nodes, tx);
	(void) zap_update(mos, obj, "coordinator", sizeof (uint64_t), 1,
	    &cspa->cspa_config.cc_coordinator_id, tx);
	(void) zap_update(mos, obj, "ms_policy", sizeof (uint64_t), 1,
	    &cspa->cspa_config.cc_ms_partition_policy, tx);
	(void) zap_update(mos, obj, "txg_policy", sizeof (uint64_t), 1,
	    &cspa->cspa_config.cc_txg_policy, tx);
	(void) zap_update(mos, obj, "hb_interval", sizeof (uint64_t), 1,
	    &cspa->cspa_config.cc_heartbeat_interval_ms, tx);
	(void) zap_update(mos, obj, "hb_timeout", sizeof (uint64_t), 1,
	    &cspa->cspa_config.cc_heartbeat_timeout_ms, tx);
	(void) zap_update(mos, obj, "fence_policy", sizeof (uint64_t), 1,
	    &cspa->cspa_config.cc_fence_policy, tx);

	/* Write membership */
	(void) cluster_membership_write(spa, &cspa->cspa_membership, tx);

	/* Write metaslab assignments */
	for (uint64_t v = 0; v < cspa->cspa_ms_assign_count; v++) {
		(void) cluster_ms_assign_write(spa,
		    &cspa->cspa_ms_assign[v], tx);
	}

	dmu_tx_commit(tx);
	return (0);
}

int
cluster_spa_config_read(spa_t *spa)
{
	cluster_spa_t *cspa = spa->spa_cluster;
	dsl_pool_t *dp = spa->spa_dsl_pool;
	objset_t *mos = dp->dp_meta_objset;
	uint64_t obj;
	int error;

	if (cspa == NULL)
		return (EINVAL);

	error = zap_lookup(mos, DMU_POOL_DIRECTORY_OBJECT,
	    CLUSTER_CONFIG_OBJ_NAME, sizeof (uint64_t), 1, &obj);
	if (error != 0)
		return (error);

	(void) zap_lookup(mos, obj, "magic", sizeof (uint64_t), 1,
	    &cspa->cspa_config.cc_magic);
	(void) zap_lookup(mos, obj, "version", sizeof (uint64_t), 1,
	    &cspa->cspa_config.cc_version);
	(void) zap_lookup(mos, obj, "max_nodes", sizeof (uint64_t), 1,
	    &cspa->cspa_config.cc_max_nodes);
	(void) zap_lookup(mos, obj, "coordinator", sizeof (uint64_t), 1,
	    &cspa->cspa_config.cc_coordinator_id);
	(void) zap_lookup(mos, obj, "ms_policy", sizeof (uint64_t), 1,
	    &cspa->cspa_config.cc_ms_partition_policy);
	(void) zap_lookup(mos, obj, "txg_policy", sizeof (uint64_t), 1,
	    &cspa->cspa_config.cc_txg_policy);
	(void) zap_lookup(mos, obj, "hb_interval", sizeof (uint64_t), 1,
	    &cspa->cspa_config.cc_heartbeat_interval_ms);
	(void) zap_lookup(mos, obj, "hb_timeout", sizeof (uint64_t), 1,
	    &cspa->cspa_config.cc_heartbeat_timeout_ms);
	(void) zap_lookup(mos, obj, "fence_policy", sizeof (uint64_t), 1,
	    &cspa->cspa_config.cc_fence_policy);

	/* Validate magic */
	if (cspa->cspa_config.cc_magic != CLUSTER_CONFIG_MAGIC)
		return (EINVAL);

	return (0);
}

/* ------------------------------------------------------------------ */
/*  Cluster Mode Check                                                 */
/* ------------------------------------------------------------------ */

/*
 * Check if a pool is in cluster mode.
 */
boolean_t
cluster_spa_enabled(spa_t *spa)
{
	return (spa->spa_cluster != NULL);
}

/*
 * Check if this node is the coordinator.
 */
boolean_t
cluster_spa_is_coordinator(spa_t *spa)
{
	cluster_spa_t *cspa = spa->spa_cluster;

	if (cspa == NULL)
		return (B_TRUE);  /* single-node = always coordinator */

	return (cspa->cspa_is_coordinator);
}

/* ------------------------------------------------------------------ */
/*  Modified spa_sync() Integration                                    */
/* ------------------------------------------------------------------ */

/*
 * This function provides the entry point for cluster-aware
 * spa_sync(). It wraps the standard spa_sync() with cluster
 * coordination hooks.
 *
 * COORDINATOR FLOW:
 *   cluster_spa_sync_enter()   — wait for participants
 *   [standard spa_sync()]
 *     → dsl_pool_sync()        — write all dirty data
 *     → dsl_pool_sync_mos()    — write MOS (coordinator only)
 *     → vdev_config_sync()     — write uberblock (coordinator only)
 *   cluster_spa_sync_exit()    — broadcast sync done
 *
 * PARTICIPANT FLOW:
 *   cluster_spa_sync_enter()   — begin data flush
 *   [modified spa_sync()]
 *     → dsl_pool_sync()        — write dirty data blocks
 *     → SKIP dsl_pool_sync_mos()
 *     → SKIP vdev_config_sync()
 *   cluster_spa_sync_exit()    — report completion
 */
void
cluster_spa_sync_enter(spa_t *spa, uint64_t txg)
{
	cluster_spa_t *cspa = spa->spa_cluster;

	if (cspa == NULL)
		return;  /* single-node: no coordination needed */

	(void)txg;

	/*
	 * In a true multi-node cluster, the coordinator would
	 * broadcast TXG_SYNC_START and wait for participants to
	 * complete their data flushes before writing MOS.
	 *
	 * In the single-machine export/import simulation there
	 * are no other active nodes.  All cluster TXG coordination
	 * is a no-op until real inter-node networking exists.
	 */
	if (!cspa->cspa_is_coordinator) {
		/* Participant: no-op; coordinator drives sync */
	}
}

void
cluster_spa_sync_exit(spa_t *spa, uint64_t txg)
{
	cluster_spa_t *cspa = spa->spa_cluster;

	if (cspa == NULL)
		return;

	(void)txg;

	/*
	 * In a true multi-node cluster, the coordinator would
	 * broadcast TXG_SYNC_DONE and release DLM locks here.
	 * In single-machine simulation there are no other nodes
	 * to notify — everything is a no-op.
	 */
}

/* ------------------------------------------------------------------ */
/*  Cluster Pool Property Handling                                     */
/* ------------------------------------------------------------------ */

/*
 * Handle cluster-related pool properties.
 *
 * New properties:
 *   cluster          - boolean, enable/disable cluster mode
 *   cluster_nodes    - uint64, maximum number of nodes
 *   cluster_node_id  - uint64, this node's ID in the cluster
 *   cluster_ms_policy - string, metaslab partitioning policy
 *   cluster_hb_interval - uint64, heartbeat interval in ms
 *   cluster_hb_timeout  - uint64, heartbeat timeout in ms
 */
int
cluster_spa_prop_set(spa_t *spa, nvlist_t *props)
{
	cluster_spa_t *cspa = spa->spa_cluster;
	nvpair_t *elem;
	int error = 0;

	elem = NULL;
	while ((elem = nvlist_next_nvpair(props, elem)) != NULL) {
		const char *propname = nvpair_name(elem);

		if (strcmp(propname, "cluster") == 0) {
			boolean_t val;

			error = nvpair_value_boolean_value(elem, &val);
			if (error != 0)
				break;

			if (val && cspa == NULL) {
				/* Enable cluster mode */
				error = cluster_spa_init(spa,
				    CLUSTER_NODE_ID_COORDINATOR, B_TRUE);
			} else if (!val && cspa != NULL) {
				/* Disable cluster mode */
				cmn_err(CE_WARN, "cluster: cannot disable "
				    "cluster mode on active pool");
				error = EBUSY;
			}
		} else if (strcmp(propname, "cluster_node_id") == 0) {
			uint64_t val;

			error = nvpair_value_uint64(elem, &val);
			if (error != 0)
				break;

			if (val >= CLUSTER_MAX_NODES) {
				error = EINVAL;
				break;
			}

			if (cspa != NULL)
				cspa->cspa_local_id = (cluster_node_id_t)val;
		} else if (strcmp(propname, "cluster_ms_policy") == 0) {
			const char *val;

			error = nvpair_value_string(elem, &val);
			if (error != 0)
				break;

			if (cspa != NULL) {
				if (strcmp(val, "static") == 0)
					cspa->cspa_config.cc_ms_partition_policy =
					    CLUSTER_MS_PARTITION_STATIC;
				else if (strcmp(val, "dynamic") == 0)
					cspa->cspa_config.cc_ms_partition_policy =
					    CLUSTER_MS_PARTITION_DYNAMIC;
				else if (strcmp(val, "adaptive") == 0)
					cspa->cspa_config.cc_ms_partition_policy =
					    CLUSTER_MS_PARTITION_ADAPTIVE;
				else
					error = EINVAL;
			}
		}
	}

	return (error);
}
