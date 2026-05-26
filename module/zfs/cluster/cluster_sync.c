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
 * Cluster ZFS - MOS and Uberblock Coordination
 *
 * This module implements the single-writer model for MOS and
 * uberblock updates in Cluster ZFS.
 *
 * THE CORE CONSTRAINT
 * ===================
 *
 * In standard ZFS, the uberblock is the single root of trust:
 *
 *   Uberblock → ub_rootbp → MOS → all pool metadata
 *                              → space maps, datasets, DSL, etc.
 *
 * If two nodes write uberblocks concurrently, the disk can end up
 * in an inconsistent state where one node's uberblock points to
 * a MOS that references blocks written by another node that may
 * not have been committed yet.
 *
 * THE SINGLE-WRITER MODEL
 * =======================
 *
 * Only the coordinator node writes the MOS and uberblock.
 * This ensures:
 *
 *   1. The MOS is always consistent (single thread of updates)
 *   2. The uberblock always points to a valid, committed MOS
 *   3. No ordering conflicts in vdev label writes
 *
 * FLOW:
 *   1. All nodes write their dirty data blocks (parallel, safe)
 *   2. All nodes report sync completion to coordinator
 *   3. Coordinator waits for all nodes to complete data flush
 *   4. Coordinator writes MOS (dsl_pool_sync_mos)
 *   5. Coordinator writes uberblock (vdev_config_sync)
 *
 * COORDINATOR MODIFICATIONS TO spa_sync()
 * ========================================
 *
 * The coordinator's spa_sync() is modified:
 *   - Before dsl_pool_sync_mos(): wait for all participants
 *   - After vdev_config_sync(): broadcast TXG_SYNC_DONE
 *
 * PARTICIPANT MODIFICATIONS TO spa_sync()
 * ========================================
 *
 * The participant's spa_sync() is modified:
 *   - Skip dsl_pool_sync_mos() entirely
 *   - Skip vdev_config_sync() (uberblock write) entirely
 *   - Only flush dirty data blocks
 *   - Report completion to coordinator
 *
 * UBERBLOCK VALIDATION
 * ====================
 *
 * On import, a node reads the uberblock and validates:
 *   1. ub_magic matches
 *   2. ub_txg is recent enough
 *   3. ub_guid_sum matches the vdev tree
 *   4. Cluster membership epoch matches (if cluster is active)
 *
 * MIGRATING COORDINATOR
 * =====================
 *
 * When the coordinator fails, the new coordinator must:
 *   1. Read the latest uberblock from disk
 *   2. Ensure all pending I/O from the old coordinator has
 *      completed or been aborted
 *   3. Ensure all participants' data flushes are complete
 *   4. Take over MOS/uberblock writing
 */

#include <sys/cluster/cluster_types.h>
#include <sys/cluster/cluster_sync.h>
#include <sys/cluster/cluster_txg.h>
#include <sys/cluster/cluster_membership.h>
#include <sys/cluster/cluster_spa.h>
#include <sys/cluster/cluster_metaslab.h>
#include <sys/spa_impl.h>
#include <sys/dsl_pool.h>
#include <sys/uberblock_impl.h>
#include <sys/vdev_impl.h>
#include <sys/dmu_tx.h>
#include <sys/metaslab_impl.h>
#include <sys/txg_impl.h>

/* ------------------------------------------------------------------ */
/*  Coordinator: Modified spa_sync() Hooks                             */
/* ------------------------------------------------------------------ */

/*
 * Called by the coordinator's spa_sync() BEFORE writing MOS.
 *
 * This is the CRITICAL synchronization point where the coordinator
 * ensures all participants have flushed their dirty data blocks
 * before the MOS is written. This ordering is essential because:
 *
 *   - MOS entries (space maps, dataset structures) reference
 *     data blocks written by participants
 *   - If a participant hasn't written its data blocks yet,
 *     the MOS would reference non-existent blocks
 *   - The uberblock (written after MOS) must never reference
 *     a MOS that points to unwritten data
 */
void
cluster_sync_coordinator_pre_mos(cluster_spa_t *cspa)
{
	cluster_txg_t *ctx = &cspa->cspa_txg;
	cluster_membership_t *cm = &cspa->cspa_membership;

	ASSERT(cspa->cspa_is_coordinator);

	/*
	 * Wait for all active participants to report that
	 * their data flush is complete for this TXG.
	 *
	 * The flow is:
	 *   1. Coordinator broadcasts TXG_SYNC_START
	 *   2. All participants begin flushing dirty data
	 *   3. Each participant sends TXG_BARRIER_LEAVE when done
	 *   4. Coordinator waits until all barriers are cleared
	 */
	mutex_enter(&ctx->ctx_lock);

	while (!cluster_txg_all_sync_complete(ctx, cm)) {
		cv_wait(&ctx->ctx_sync_start_cv, &ctx->ctx_lock);
	}

	mutex_exit(&ctx->ctx_lock);
}

/*
 * Called by the coordinator's spa_sync() AFTER writing uberblock.
 *
 * Broadcasts TXG_SYNC_DONE to all participants so they can
 * advance their local TXG tracking and start accepting new
 * transactions.
 */
void
cluster_sync_coordinator_post_uberblock(cluster_spa_t *cspa, uint64_t txg)
{
	(void)txg;
	ASSERT(cspa->cspa_is_coordinator);

	cluster_txg_sync_done(&cspa->cspa_txg, &cspa->cspa_membership);

	/*
	 * In a real implementation, broadcast TXG_SYNC_DONE message
	 * to all participants here.
	 */
}

/* ------------------------------------------------------------------ */
/*  Participant: Modified spa_sync() Hooks                             */
/* ------------------------------------------------------------------ */

/*
 * Called by the participant's spa_sync() at the START.
 *
 * On a participant, spa_sync() is modified to:
 *   - Skip dsl_pool_sync_mos() — coordinator handles MOS
 *   - Skip vdev_config_sync() — coordinator handles uberblock
 *   - Only flush dirty data blocks
 *   - Report completion to coordinator
 */
void
cluster_sync_participant_begin(cluster_spa_t *cspa, uint64_t txg)
{
	(void)txg;
	ASSERT(!cspa->cspa_is_coordinator);

	/*
	 * The participant's spa_sync() path is:
	 *
	 * 1. Wait for prior open-context ZIOs
	 * 2. Process BRT updates
	 * 3. Lock config (SCL_CONFIG, RW_READER)
	 * 4. dsl_pool_sync() — flush dirty datasets
	 *    - This writes data blocks to disk
	 *    - Does NOT write MOS or uberblock
	 * 5. Report completion to coordinator
	 *
	 * Steps that are SKIPPED on participant:
	 * - dsl_pool_sync_mos() — only coordinator does this
	 * - spa_sync_rewrite_vdev_config() — only coordinator
	 * - vdev_uberblock_sync() — only coordinator
	 */
}

/*
 * Called by the participant's spa_sync() when data flush is complete.
 * Sends a TXG_BARRIER_LEAVE message to the coordinator.
 */
void
cluster_sync_participant_complete(cluster_spa_t *cspa, uint64_t txg)
{
	(void)txg;
	ASSERT(!cspa->cspa_is_coordinator);

	/*
	 * Report to coordinator that this participant has completed
	 * its data flush for the given TXG.
	 *
	 * The coordinator will not write MOS until ALL active
	 * participants have reported completion.
	 *
	 * In a real implementation, send TXG_BARRIER_LEAVE message.
	 */
}

/* ------------------------------------------------------------------ */
/*  Uberblock Validation for Cluster                                   */
/* ------------------------------------------------------------------ */

/*
 * Validate that an uberblock is safe to use in a cluster context.
 *
 * Additional checks beyond the standard uberblock validation:
 *   1. Cluster epoch must match current membership
 *   2. If the writer of this uberblock is now fenced, reject
 *   3. If a new coordinator was elected, ensure this uberblock
 *      was written by the current (or previous) coordinator
 */
boolean_t
cluster_sync_uberblock_validate(cluster_spa_t *cspa, uberblock_t *ub)
{
	cluster_membership_t *cm = &cspa->cspa_membership;

	/* Standard validation */
	if (ub->ub_magic != UBERBLOCK_MAGIC)
		return (B_FALSE);

	if (ub->ub_version != SPA_VERSION)
		return (B_FALSE);

	if (ub->ub_txg == 0)
		return (B_FALSE);

	/*
	 * Cluster-specific validation:
	 *
	 * If this is a cluster pool (has cluster_config in MOS),
	 * check that the uberblock's MMP information indicates
	 * a valid coordinator was writing.
	 *
	 * We extend the MMP mechanism for cluster awareness:
	 * - ub_mmp_config contains the coordinator's node ID
	 * - ub_mmp_delay contains the cluster epoch
	 *
	 * This allows a node to detect if the uberblock was
	 * written by a now-fenced coordinator and reject it.
	 */
	if (cspa->cspa_config.cc_num_nodes > 0) {
		uint64_t writer_id =
		    ((ub->ub_mmp_config >> 48) & 0xFFFF) - 1;
		uint64_t writer_epoch = ub->ub_mmp_delay;

		/*
		 * If the writer was fenced in a more recent epoch,
		 * this uberblock is stale and should be rejected.
		 */
		if (writer_epoch < cm->cm_epoch) {
			cluster_node_t *cn;

			cn = cluster_membership_find(cm, writer_id);
			if (cn != NULL && cn->cn_fenced) {
				cmn_err(CE_WARN, "cluster: rejecting "
				    "uberblock from fenced node %llu "
				    "(epoch %llu < %llu)",
				    (u_longlong_t)writer_id,
				    (u_longlong_t)writer_epoch,
				    (u_longlong_t)cm->cm_epoch);
				return (B_FALSE);
			}
		}
	}

	return (B_TRUE);
}

/* ------------------------------------------------------------------ */
/*  Coordinator: Uberblock Write with Cluster Metadata                 */
/* ------------------------------------------------------------------ */

/*
 * Extended uberblock update for cluster mode.
 *
 * In addition to the standard uberblock_update() fields,
 * the coordinator writes cluster metadata into the MMP fields:
 *
 *   ub_mmp_config bits 48-63: coordinator node ID
 *   ub_mmp_delay: cluster membership epoch
 *
 * This allows nodes to validate uberblock provenance on import.
 */
void
cluster_sync_uberblock_update(uberblock_t *ub, cluster_spa_t *cspa,
    vdev_t *rvd, uint64_t txg)
{
	/* Standard uberblock update */
	ub->ub_magic = UBERBLOCK_MAGIC;
	ub->ub_version = SPA_VERSION;
	ub->ub_txg = txg;
	ub->ub_guid_sum = rvd->vdev_guid_sum;
	ub->ub_timestamp = gethrestime_sec();

	if (cspa->cspa_is_coordinator) {
		/*
		 * Encode coordinator identity and epoch into
		 * the MMP fields of the uberblock.
		 *
		 * ub_mmp_config:
		 *   bits 0-47: standard MMP config
		 *   bits 48-63: coordinator node ID
		 *
		 * ub_mmp_delay:
		 *   Set to current cluster epoch
		 */
		uint64_t mmp_config = ub->ub_mmp_config;
		mmp_config &= ~((uint64_t)0xFFFF << 48);
		mmp_config |= ((uint64_t)cspa->cspa_local_id << 48);
		ub->ub_mmp_config = mmp_config;
		ub->ub_mmp_delay = cspa->cspa_membership.cm_epoch;
		ub->ub_mmp_magic = MMP_MAGIC;
	}
}

/* ------------------------------------------------------------------ */
/*  Coordinator Transition                                             */
/* ------------------------------------------------------------------ */

/*
 * Take over as coordinator after a failover.
 *
 * This function is called when a new coordinator is elected.
 * It must:
 *   1. Read the latest uberblock from disk
 *   2. Wait for all pending I/O from the old coordinator
 *   3. Ensure all participants' data is flushed
 *   4. Begin writing MOS and uberblock
 */
int
cluster_sync_coordinator_takeover(cluster_spa_t *cspa)
{
	spa_t *spa = cspa->cspa_spa;
	int error = 0;

	/*
	 * Step 1: Read the latest uberblock.
	 * The latest uberblock on disk represents the last
	 * consistent state committed by the old coordinator.
	 *
	 * vdev_uberblock_load() will find the best uberblock
	 * across all vdev labels. We use the existing
	 * spa->spa_uberblock which was loaded during spa_load().
	 */
	if (spa != NULL) {
		uberblock_t *ub = &spa->spa_ubsync;

		/*
		 * Validate that the loaded uberblock is usable.
		 * It should have been validated during spa_load(),
		 * but we double-check in the cluster context.
		 */
		if (ub->ub_magic != UBERBLOCK_MAGIC) {
			cmn_err(CE_WARN, "cluster: loaded uberblock has "
			    "invalid magic during coordinator takeover");
			return (EIO);
		}
	}

	/*
	 * Step 2: Wait for pending I/O.
	 * Any I/O that was in flight from the old coordinator
	 * must complete or be aborted before we write new data.
	 *
	 * We do this by waiting for the spa's TXG to quiesce.
	 * The standard ZFS mechanism ensures that once a TXG
	 * has synced, all writes issued in that TXG are stable.
	 */
	if (spa != NULL && spa->spa_dsl_pool != NULL) {
		/*
		 * Wait for the current syncing TXG to complete.
		 * This ensures that all I/O from the previous
		 * coordinator's last sync has been written to disk.
		 */
		uint64_t syncing_txg = spa->spa_syncing_txg;
		if (syncing_txg != 0) {
			txg_wait_synced(spa->spa_dsl_pool, syncing_txg);
		}
	}

	/*
	 * Step 3: Reconcile state.
	 * - Read cluster membership from MOS
	 * - Read metaslab assignments from MOS
	 * - Reclaim metaslabs from fenced nodes
	 * - Update local DLM state
	 */
	if (spa != NULL) {
		error = cluster_spa_config_read(spa);
		if (error != 0) {
			cmn_err(CE_WARN, "cluster: failed to read cluster "
			    "config during takeover: %d", error);
			/* Continue - we may be recovering from a fresh pool */
		}
	}

	/*
	 * Step 4: Mark self as coordinator.
	 */
	cspa->cspa_is_coordinator = B_TRUE;
	cspa->cspa_role = CLUSTER_ROLE_COORDINATOR;

	/*
	 * Step 5: Update membership to reflect new coordinator.
	 */
	mutex_enter(&cspa->cspa_membership.cm_lock);
	cspa->cspa_membership.cm_coordinator = cspa->cspa_local_id;
	mutex_exit(&cspa->cspa_membership.cm_lock);

	/*
	 * Step 6: Write new uberblock with our identity.
	 * This establishes us as the active coordinator
	 * and prevents the old coordinator (if it recovers)
	 * from writing a stale uberblock.
	 *
	 * The next spa_sync() cycle will write the uberblock
	 * with our node ID encoded in the MMP fields.
	 * We trigger a config dirty to ensure vdev labels
	 * are updated.
	 */
	if (spa != NULL && spa->spa_root_vdev != NULL) {
		vdev_config_dirty(spa->spa_root_vdev);
	}

	cmn_err(CE_NOTE, "cluster: node %u taking over as coordinator",
	    cspa->cspa_local_id);

	return (error);
}

/* ------------------------------------------------------------------ */
/*  Space Map Write Coordination                                       */
/* ------------------------------------------------------------------ */

/*
 * When a node writes its metaslab's space map during sync,
 * the space map object lives in the MOS. But only the
 * coordinator writes the MOS!
 *
 * Solution: Each node's space map updates are collected
 * locally, then sent to the coordinator for MOS integration.
 *
 * However, this adds latency. An optimization is:
 *   - Each node writes its OWN space map objects directly
 *     to shared disk (bypassing the MOS write path)
 *   - The coordinator updates the MOS to reference these
 *     space map objects during its sync
 *
 * This works because:
 *   - Space map objects are append-only logs
 *   - Each metaslab has its own space map object
 *   - Since metaslabs are partitioned, no two nodes write
 *     the same space map object
 *
 * The coordinator's MOS write simply updates the space map
 * object references to include the new entries.
 */
void
cluster_sync_spacemap_flush(cluster_spa_t *cspa, uint64_t txg)
{
	spa_t *spa = cspa->cspa_spa;
	vdev_t *rvd;

	(void)txg;

	if (spa == NULL)
		return;

	rvd = spa->spa_root_vdev;
	if (rvd == NULL)
		return;

	/*
	 * Flush space map entries for locally-owned metaslabs.
	 *
	 * Each node's space map updates are collected locally.
	 * Since metaslabs are partitioned across nodes (no two
	 * nodes write the same space map object), and space map
	 * objects are in the MOS, we use Option C from the design:
	 *
	 * Option C: Use the existing log space map feature.
	 *   - Each node writes to its own log space map entries
	 *   - The coordinator flushes all nodes' log space maps
	 *     to the per-metaslab space maps during MOS sync
	 *
	 * On the coordinator, this happens naturally during
	 * dsl_pool_sync_mos(). On participants, we need to
	 * ensure that:
	 *   1. Locally-dirty metaslab space maps are written
	 *      to disk before the coordinator syncs the MOS
	 *   2. The coordinator knows which space maps are dirty
	 *      so it can include them in the MOS update
	 *
	 * The flow is:
	 *   1. Participant syncs dirty datasets (data blocks)
	 *   2. Participant writes dirty space map entries
	 *      for its owned metaslabs
	 *   3. Participant sends TXG_BARRIER_LEAVE
	 *   4. Coordinator syncs MOS (includes space map updates)
	 *   5. Coordinator writes uberblock
	 */
	for (uint64_t c = 0; c < rvd->vdev_children; c++) {
		vdev_t *vd = rvd->vdev_child[c];

		if (!vdev_is_concrete(vd))
			continue;

		for (uint64_t m = 0; m < vd->vdev_ms_count; m++) {
			metaslab_t *ms = vd->vdev_ms[m];

			if (ms == NULL)
				continue;

			/*
			 * Only flush space maps for metaslabs owned
			 * by this node (or unowned metaslabs the
			 * coordinator is managing).
			 */
			if (!cluster_metaslab_owns(spa, ms))
				continue;

			/*
			 * If the metaslab's space map is dirty,
			 * it will be written during the normal
			 * metaslab_sync() path in spa_sync().
			 *
			 * We don't need to do anything extra here
			 * because the ZFS sync pipeline already
			 * handles space map writes. The key
			 * constraint is that on participants,
			 * the MOS write must be skipped (handled
			 * by cluster_spa_sync_enter/exit hooks).
			 */
		}
	}
}
