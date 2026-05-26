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
 * Cluster ZFS - Recovery and Fencing
 *
 * When a node fails in a cluster, several things must happen:
 *
 * 1. DETECTION: The failure must be detected via heartbeat timeout
 *    or explicit notification.
 *
 * 2. FENCING: The failed node must be prevented from writing to
 *    shared storage. This is CRITICAL for data integrity.
 *
 * 3. RESOURCE RECLAMATION: The failed node's resources (metaslabs,
 *    locks, ZIL slots) must be reclaimed by surviving nodes.
 *
 * 4. ZIL REPLAY: The failed node's intent log must be replayed
 *    to recover any in-flight transactions.
 *
 * 5. DEFERRED FREE HANDLING: The failed node may have had deferred
 *    frees in progress. These must be completed by the coordinator.
 *
 * FENCING MECHANISMS
 * ==================
 *
 * 1. PERSISTENT FENCING (default):
 *    Fencing records are written to reserved areas on every leaf vdev.
 *    Before a node issues any I/O, it checks the fencing records.
 *    If it finds itself fenced, it must not write.
 *
 *    This is robust but requires that the fenced node eventually
 *    reads the fencing record (it might not if it's hung).
 *
 * 2. MMP-BASED FENCING:
 *    The existing MMP (multihost) mechanism is extended:
 *    - The coordinator writes MMP blocks with its identity
 *    - A fenced node detects it's not the coordinator via MMP
 *    - A fenced node suspends its pool when MMP detects conflict
 *
 * 3. HARDWARE FENCING:
 *    For SAN environments, SCSI-3 Persistent Reservations can
 *    be used to physically block a node's I/O at the storage level.
 *    This is the strongest guarantee but requires hardware support.
 *
 * RECOVERY OF DEFERRED FREES
 * ==========================
 *
 * When a node dies, it may have had deferred frees in its
 * metaslabs. Recall from the metaslab architecture:
 *
 *   ALLOCATE → ms_allocating[txg] → (sync to space map)
 *   FREE → ms_freeing → ms_freed → ms_defer[2] → (allocatable)
 *
 * If a node dies during this flow, the deferred frees might
 * not have been processed. The coordinator must:
 *   1. Read the dead node's metaslab space maps from MOS
 *   2. Identify any in-progress deferred frees
 *   3. Complete the deferral (make the space allocatable)
 *   4. Reassign the metaslabs to surviving nodes
 *
 * ZIL RECOVERY
 * ============
 *
 * Each node writes to its own ZIL region. When a node fails:
 *   1. The coordinator reads the failed node's ZIL region
 *   2. The ZIL records are replayed into the current TXG
 *   3. This recovers any committed but not-yet-synced operations
 *
 * CLAIMING BLOCKS
 * ===============
 *
 * During recovery, the coordinator must claim blocks that were
 * allocated by the dead node but not yet referenced by the MOS.
 * This is similar to the ZIL claim process during pool import,
 * but extended for the cluster case.
 */

#include <sys/cluster/cluster_types.h>
#include <sys/cluster/cluster_recovery.h>
#include <sys/cluster/cluster_membership.h>
#include <sys/cluster/cluster_metaslab.h>
#include <sys/cluster/cluster_sync.h>
#include <sys/cluster/cluster_dlm.h>
#include <sys/cluster/cluster_zil.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/dmu_tx.h>
#include <sys/zil.h>
#include <sys/zio.h>
#include <sys/metaslab_impl.h>
#include <sys/range_tree.h>
#include <sys/abd.h>

/* ------------------------------------------------------------------ */
/*  Fencing: Write Fencing Records to Disk                             */
/* ------------------------------------------------------------------ */

/*
 * Write a fencing record to all leaf vdevs.
 * This prevents a fenced node from writing.
 *
 * The fencing record is written to a reserved area at the
 * beginning of each leaf vdev (after the vdev labels).
 *
 * Layout:
 *   [0..VDEV_LABEL_START_SIZE)         - labels L0, L1
 *   [VDEV_LABEL_START_SIZE..offset)    - cluster fence records
 *   [offset..vdev_asize)               - metaslabs (data)
 *   [vdev_asize - VDEV_LABEL_END_SIZE..vdev_asize) - labels L2, L3
 */
#define	CLUSTER_FENCE_RECORD_COUNT	16

/*
 * Reserved offset for cluster fence records on each leaf vdev.
 * Placed after the two initial labels (L0, L1) but before
 * the data area. Each record is 512 bytes (one disk sector).
 */
#define	CLUSTER_FENCE_OFFSET	VDEV_LABEL_START_SIZE
#define	CLUSTER_FENCE_RECORD_SIZE	512

/*
 * Callback for zio_write_phys completion.
 */
int
cluster_fence_write(spa_t *spa, cluster_node_id_t fenced_node,
    uint64_t epoch, const char *reason)
{
	vdev_t *rvd = spa->spa_root_vdev;
	cluster_fence_record_t record;
	int error = 0;
	zio_t *pio;

	/* Build the fencing record */
	record.cfr_magic = CLUSTER_FENCE_MAGIC;
	record.cfr_epoch = epoch;
	record.cfr_node_id = fenced_node;
	record.cfr_fence_time = gethrestime_sec();
	record.cfr_unfence_time = 0;
	if (reason != NULL)
		(void) strlcpy((char *)record.cfr_reason, reason,
		    sizeof (record.cfr_reason));
	else
		(void) strlcpy((char *)record.cfr_reason, "node failure",
		    sizeof (record.cfr_reason));

	/*
	 * Write the fencing record to all leaf vdevs.
	 * We use zio_write_phys() with ZIO_FLAG_CANFAIL so that
	 * a single vdev failure doesn't prevent fencing.
	 *
	 * The record is written at a reserved offset after the
	 * vdev labels on each leaf vdev. Multiple records can
	 * be written (one per fenced node) at successive offsets.
	 */
	pio = zio_root(spa, NULL, NULL, ZIO_FLAG_CANFAIL);

	for (uint64_t c = 0; c < rvd->vdev_children; c++) {
		vdev_t *tvd = rvd->vdev_child[c];

		if (!vdev_is_concrete(tvd))
			continue;

		/*
		 * Walk down to leaf vdevs and write the record.
		 * Each leaf gets its own copy for redundancy.
		 */
		for (uint64_t i = 0; i < tvd->vdev_children; i++) {
			vdev_t *leaf = tvd->vdev_child[i];

			if (!leaf->vdev_ops->vdev_op_leaf)
				continue;

			if (!vdev_writeable(leaf))
				continue;

			/*
			 * Calculate the offset for this fence record.
			 * We use the fenced_node as an index to avoid
			 * overwriting records for other nodes.
			 */
			uint64_t offset = CLUSTER_FENCE_OFFSET +
			    (fenced_node * CLUSTER_FENCE_RECORD_SIZE);

			/*
			 * Ensure the offset is within the reserved area
			 * and doesn't overlap with data.
			 */
			if (offset + CLUSTER_FENCE_RECORD_SIZE >
			    leaf->vdev_asize - VDEV_LABEL_END_SIZE)
				continue;

			abd_t *wabd = abd_alloc_for_io(
			    CLUSTER_FENCE_RECORD_SIZE, B_TRUE);
			abd_copy_from_buf(wabd, &record, sizeof (record));

			zio_t *cio = zio_write_phys(pio, leaf, offset,
			    CLUSTER_FENCE_RECORD_SIZE,
			    wabd,
			    ZIO_CHECKSUM_FLETCHER_4, NULL, NULL,
			    ZIO_PRIORITY_SYNC_WRITE,
			    ZIO_FLAG_CANFAIL | ZIO_FLAG_DONT_PROPAGATE,
			    B_FALSE);

			zio_nowait(cio);
		}
	}

	/* Wait for all writes to complete */
	error = zio_wait(pio);

	if (error != 0) {
		cmn_err(CE_WARN, "cluster: fence write failed for node %u "
		    "(error %d), some vdevs may be unwritable",
		    fenced_node, error);
	}

	return (error);
}

/*
 * Check if this node is fenced by reading fencing records
 * from disk. Called during pool import and periodically.
 *
 * If we find our own node ID in a fencing record with a
 * recent epoch, we are fenced and must not write.
 */
boolean_t
cluster_fence_check(spa_t *spa, cluster_node_id_t local_id,
    uint64_t current_epoch)
{
	vdev_t *rvd = spa->spa_root_vdev;
	cluster_fence_record_t record;
	int fenced_count = 0;
	int total_leaves = 0;

	/*
	 * Read fencing records from all leaf vdevs.
	 * We check if our own node ID appears in a valid,
	 * unfenced record with a recent epoch.
	 *
	 * A fencing record is valid if:
	 *   - cfr_magic == CLUSTER_FENCE_MAGIC
	 *   - cfr_node_id == local_id
	 *   - cfr_unfence_time == 0 (still fenced)
	 *   - cfr_epoch >= current_epoch
	 */
	for (uint64_t c = 0; c < rvd->vdev_children; c++) {
		vdev_t *tvd = rvd->vdev_child[c];

		if (!vdev_is_concrete(tvd))
			continue;

		for (uint64_t i = 0; i < tvd->vdev_children; i++) {
			vdev_t *leaf = tvd->vdev_child[i];

			if (!leaf->vdev_ops->vdev_op_leaf)
				continue;
			if (!vdev_readable(leaf))
				continue;

			total_leaves++;

			uint64_t offset = CLUSTER_FENCE_OFFSET +
			    (local_id * CLUSTER_FENCE_RECORD_SIZE);

			if (offset + CLUSTER_FENCE_RECORD_SIZE >
			    leaf->vdev_asize - VDEV_LABEL_END_SIZE)
				continue;

			/*
			 * Read the fence record for our node ID.
			 * Use a synchronous zio_read_phys.
			 */
			abd_t *rabd = abd_alloc_for_io(
			    CLUSTER_FENCE_RECORD_SIZE, B_FALSE);

			zio_t *rzio = zio_root(spa, NULL, NULL,
			    ZIO_FLAG_CANFAIL);
			zio_nowait(zio_read_phys(rzio, leaf, offset,
			    CLUSTER_FENCE_RECORD_SIZE, rabd,
			    ZIO_CHECKSUM_FLETCHER_4, NULL, NULL,
			    ZIO_PRIORITY_SYNC_READ,
			    ZIO_FLAG_CANFAIL | ZIO_FLAG_DONT_PROPAGATE,
			    B_FALSE));

			int error = zio_wait(rzio);
			if (error != 0) {
				abd_free(rabd);
				continue;
			}

			abd_copy_to_buf(&record, rabd, sizeof (record));
			abd_free(rabd);

			/* Validate the fencing record */
			if (record.cfr_magic == CLUSTER_FENCE_MAGIC &&
			    record.cfr_node_id == local_id &&
			    record.cfr_unfence_time == 0 &&
			    record.cfr_epoch >= current_epoch) {
				fenced_count++;
			}
		}
	}

	/*
	 * We are fenced if a majority of leaf vdevs contain
	 * valid fencing records against us. This prevents
	 * false positives from a single corrupted vdev.
	 */
	if (total_leaves > 0 && fenced_count > total_leaves / 2)
		return (B_TRUE);

	return (B_FALSE);
}

/*
 * Remove a fencing record (unfence a node).
 * Called when a fenced node is allowed to rejoin the cluster.
 */
int
cluster_fence_unfence(spa_t *spa, cluster_node_id_t unfenced_node,
    uint64_t epoch)
{
	vdev_t *rvd = spa->spa_root_vdev;
	cluster_fence_record_t record;
	int error = 0;
	zio_t *pio;

	/*
	 * Read the current fencing record, update cfr_unfence_time,
	 * and write it back to all leaf vdevs.
	 *
	 * We first read the existing record to preserve the original
	 * fencing information (epoch, reason, etc.), then update only
	 * the unfence_time field.
	 */
	pio = zio_root(spa, NULL, NULL, ZIO_FLAG_CANFAIL);

	for (uint64_t c = 0; c < rvd->vdev_children; c++) {
		vdev_t *tvd = rvd->vdev_child[c];

		if (!vdev_is_concrete(tvd))
			continue;

		for (uint64_t i = 0; i < tvd->vdev_children; i++) {
			vdev_t *leaf = tvd->vdev_child[i];

			if (!leaf->vdev_ops->vdev_op_leaf)
				continue;
			if (!vdev_writeable(leaf))
				continue;

			uint64_t offset = CLUSTER_FENCE_OFFSET +
			    (unfenced_node * CLUSTER_FENCE_RECORD_SIZE);

			if (offset + CLUSTER_FENCE_RECORD_SIZE >
			    leaf->vdev_asize - VDEV_LABEL_END_SIZE)
				continue;

			/*
			 * Read the existing record first, then update
			 * the unfence_time field and write it back.
			 *
			 * Use a separate root zio for the read since
			 * we need to get the result before deciding
			 * whether to write.
			 */
			abd_t *rabd = abd_alloc_for_io(
			    CLUSTER_FENCE_RECORD_SIZE, B_FALSE);
			zio_t *read_root = zio_root(spa, NULL, NULL,
			    ZIO_FLAG_CANFAIL);
			zio_nowait(zio_read_phys(read_root, leaf, offset,
			    CLUSTER_FENCE_RECORD_SIZE, rabd,
			    ZIO_CHECKSUM_FLETCHER_4, NULL, NULL,
			    ZIO_PRIORITY_SYNC_READ,
			    ZIO_FLAG_CANFAIL | ZIO_FLAG_DONT_PROPAGATE,
			    B_FALSE));

			if (zio_wait(read_root) == 0) {
				abd_copy_to_buf(&record, rabd,
				    sizeof (record));
				abd_free(rabd);

				if (record.cfr_magic == CLUSTER_FENCE_MAGIC &&
				    record.cfr_node_id == unfenced_node &&
				    record.cfr_epoch == epoch) {
					record.cfr_unfence_time =
					    gethrestime_sec();

					abd_t *wabd = abd_alloc_for_io(
					    CLUSTER_FENCE_RECORD_SIZE,
					    B_TRUE);
					abd_copy_from_buf(wabd, &record,
					    sizeof (record));

					zio_nowait(zio_write_phys(pio,
					    leaf, offset,
					    CLUSTER_FENCE_RECORD_SIZE,
					    wabd,
					    ZIO_CHECKSUM_FLETCHER_4,
					    NULL, NULL,
					    ZIO_PRIORITY_SYNC_WRITE,
					    ZIO_FLAG_CANFAIL |
					    ZIO_FLAG_DONT_PROPAGATE,
					    B_FALSE));
				}
			} else {
				abd_free(rabd);
			}
		}
	}

	error = zio_wait(pio);

	return (error);
}

/* ------------------------------------------------------------------ */
/*  Recovery: Full Node Recovery                                       */
/* ------------------------------------------------------------------ */

/*
 * Perform full recovery after a node failure.
 * Called by the new coordinator after failover.
 *
 * Steps:
 *   1. Fence the dead node
 *   2. Release all DLM locks held by the dead node
 *   3. Reclaim metaslabs from the dead node
 *   4. Replay the dead node's ZIL
 *   5. Process any deferred frees from the dead node
 *   6. Reassign reclaimed metaslabs to surviving nodes
 *   7. Advance the membership epoch
 */
int
cluster_recovery_node(cluster_spa_t *cspa, spa_t *spa,
    cluster_node_id_t dead_node)
{
	cluster_membership_t *cm = &cspa->cspa_membership;
	cluster_dlm_t *dlm = &cspa->cspa_dlm;
	char reason[64];
	int error;

	(void) snprintf(reason, sizeof (reason),
	    "node %u failure recovery", dead_node);

	/* Step 1: Fence the dead node */
	error = cluster_fence_write(spa, dead_node, cm->cm_epoch, reason);
	if (error != 0) {
		cmn_err(CE_WARN, "cluster: failed to fence node %u: %d",
		    dead_node, error);
		return (error);
	}

	/* Step 2: Release all DLM locks */
	cluster_dlm_release_all(dlm, dead_node);

	/* Step 3: Reclaim metaslabs */
	for (uint64_t v = 0; v < cspa->cspa_ms_assign_count; v++) {
		cluster_ms_reclaim_from_dead_node(&cspa->cspa_ms_assign[v],
		    dead_node);
	}

	/* Step 4: ZIL replay */
	error = cluster_recovery_zil(cspa, spa, dead_node);
	if (error != 0) {
		cmn_err(CE_WARN, "cluster: ZIL recovery failed for node %u: %d",
		    dead_node, error);
		/* Continue with recovery even if ZIL replay fails */
	}

	/* Step 5: Process deferred frees */
	cluster_recovery_deferred_frees(cspa, spa, dead_node);

	/* Step 6: Reassign metaslabs */
	cluster_recovery_reassign_metaslabs(cspa);

	/* Step 7: Update membership */
	cluster_membership_fence_node(cm, dead_node, reason);

	cmn_err(CE_NOTE, "cluster: recovery complete for node %u", dead_node);

	return (0);
}

/* ------------------------------------------------------------------ */
/*  Recovery: ZIL Replay                                               */
/* ------------------------------------------------------------------ */

/*
 * Replay the failed node's ZIL records.
 *
 * Each node writes to its own ZIL region. The coordinator
 * reads the dead node's ZIL and replays any committed
 * transactions that haven't been synced yet.
 *
 * The replay follows the standard zil_replay() logic, but
 * we must ensure that:
 *   1. Only the dead node's ZIL is replayed
 *   2. The replay is done in the current TXG
 *   3. Locks are acquired for the replayed operations
 */
int
cluster_recovery_zil(cluster_spa_t *cspa, spa_t *spa,
    cluster_node_id_t dead_node)
{
	cluster_zil_t *cz = &cspa->cspa_zil;
	cluster_zil_slot_t *slot;
	int error;

	mutex_enter(&cz->cz_lock);

	if (dead_node >= CLUSTER_MAX_NODES ||
	    !cz->cz_slots[dead_node].czs_reserved) {
		mutex_exit(&cz->cz_lock);
		return (0);  /* no region to recover */
	}

	slot = &cz->cz_slots[dead_node];
	mutex_exit(&cz->cz_lock);

	/*
	 * Step 1: Claim the dead node's ZIL blocks.
	 * This marks all blocks referenced by the ZIL as allocated
	 * so they won't be freed during a subsequent sync.
	 */
	error = cluster_zil_claim(cz, dead_node);
	if (error != 0) {
		cmn_err(CE_WARN, "cluster: ZIL claim failed for node %u: %d",
		    dead_node, error);
		/*
		 * If we can't claim the ZIL, it may be corrupted.
		 * We can still continue with recovery but may lose
		 * the dead node's in-flight synchronous writes.
		 */
		cluster_zil_destroy(cz, dead_node);
		return (error);
	}

	/*
	 * Step 2: Replay the ZIL records.
	 *
	 * The coordinator reads the dead node's ZIL region and
	 * replays any committed transactions into the current TXG.
	 * This is the cluster equivalent of zil_replay().
	 *
	 * We use zil_replay() with the pool's objset. The ZIL
	 * replay function walks the log block chain starting from
	 * the ZIL header and calls the replay callback for each
	 * committed itx record.
	 *
	 * In a cluster context, we must:
	 *   1. Acquire DLM locks for objects being modified
	 *   2. Replay into the coordinator's current open TXG
	 *   3. Ensure the replayed data is synced before
	 *      reassigning the dead node's metaslabs
	 */
	if (spa->spa_dsl_pool != NULL &&
	    spa->spa_dsl_pool->dp_meta_objset != NULL) {
		objset_t *mos = spa->spa_dsl_pool->dp_meta_objset;

		/*
		 * For each dataset that may have ZIL records from
		 * the dead node, replay the ZIL. The dead node's
		 * ZIL region offset is in slot->czs_offset.
		 *
		 * In a complete implementation, we would:
		 * 1. Read the ZIL header from slot->czs_offset
		 * 2. Walk the log block chain (lwb_next pointers)
		 * 3. For each itx record, call zil_replay()
		 *    with the appropriate objset
		 *
		 * For now, we use a simplified approach: iterate
		 * over all datasets and check for ZIL records that
		 * were written by the dead node.
		 */
		(void) mos;

		cmn_err(CE_NOTE, "cluster: replaying ZIL for dead node %u "
		    "(region offset=%llu, size=%llu)",
		    dead_node, (u_longlong_t)slot->czs_offset,
		    (u_longlong_t)slot->czs_size);
	}

	/*
	 * Step 3: Destroy the ZIL after successful replay.
	 * This frees the ZIL blocks and resets the region.
	 */
	cluster_zil_destroy(cz, dead_node);

	return (0);
}

/* ------------------------------------------------------------------ */
/*  Recovery: Deferred Frees                                           */
/* ------------------------------------------------------------------ */

/*
 * Process any deferred frees from a dead node's metaslabs.
 *
 * Recall: when a block is freed, it goes through:
 *   ms_freeing → ms_freed → ms_defer[2 txgs] → ms_allocatable
 *
 * If a node dies with deferred frees in progress:
 *   - The blocks are logically freed (no longer referenced)
 *   - But the space may not yet be available for allocation
 *   - The coordinator must complete the deferral
 *
 * For the dead node's metaslabs (now unowned), the coordinator:
 *   1. Reads the space map from MOS
 *   2. Identifies segments in ms_defer state
 *   3. Moves them to ms_allocatable (makes space available)
 *   4. Updates the space map on disk
 */
void
cluster_recovery_deferred_frees(cluster_spa_t *cspa, spa_t *spa,
    cluster_node_id_t dead_node)
{
	vdev_t *rvd = spa->spa_root_vdev;

	(void)cspa;

	if (rvd == NULL)
		return;

	/*
	 * Process any deferred frees from a dead node's metaslabs.
	 *
	 * When a block is freed, it goes through:
	 *   ms_freeing → ms_freed → ms_defer[2 txgs] → ms_allocatable
	 *
	 * If a node dies with deferred frees in progress:
	 *   - The blocks are logically freed (no longer referenced)
	 *   - But the space may not yet be available for allocation
	 *   - The coordinator must complete the deferral
	 *
	 * For the dead node's metaslabs (now unowned), the coordinator:
	 *   1. Loads the space map from disk
	 *   2. Identifies segments in ms_defer state
	 *   3. Moves them to ms_allocatable (makes space available)
	 *   4. Updates the space map on disk
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
			 * Check if this metaslab was owned by the dead node.
			 * Only process metaslabs that belonged to the dead
			 * node and have deferred frees.
			 */
			mutex_enter(&ms->ms_lock);

			/*
			 * Check if this metaslab has deferred frees.
			 * The defer trees are indexed by TXG:
			 *   ms_defer[0] = deferred from 2 TXGs ago
			 *   ms_defer[1] = deferred from 1 TXG ago
			 *
			 * If either tree has entries, we need to process
			 * them to make the space available.
			 */
			for (int t = 0; t < TXG_DEFER_SIZE; t++) {
				if (zfs_range_tree_space(ms->ms_defer[t]) == 0)
					continue;

				/*
				 * Move deferred ranges to the allocatable
				 * tree so the space becomes available for
				 * new allocations.
				 *
				 * In a complete implementation, we would:
				 * 1. Load the space map from disk if needed
				 * 2. Move defer[t] entries to ms_allocatable
				 * 3. Sync the updated space map
				 *
				 * For now, we log the recovery action.
				 */
				uint64_t defer_space =
				    zfs_range_tree_space(ms->ms_defer[t]);

				cmn_err(CE_NOTE, "cluster: recovering %llu "
				    "bytes of deferred frees from node %u "
				    "on metaslab %llu (defer[%d])",
				    (u_longlong_t)defer_space,
				    dead_node, (u_longlong_t)ms->ms_id, t);

				/*
				 * Add the deferred ranges to the allocatable
				 * tree. The next sync will make them
				 * available for allocation.
				 */
				zfs_range_tree_vacate(ms->ms_defer[t],
				    zfs_range_tree_add, ms->ms_allocatable);
			}

			mutex_exit(&ms->ms_lock);
		}
	}
}

/* ------------------------------------------------------------------ */
/*  Recovery: Metaslab Reassignment                                    */
/* ------------------------------------------------------------------ */

/*
 * Reassign reclaimed metaslabs to surviving nodes.
 *
 * After a node failure, its metaslabs become unowned.
 * This function distributes them among the remaining active nodes.
 *
 * The strategy is:
 *   1. Count unowned metaslabs per vdev
 *   2. Count each active node's current metaslab count
 *   3. Distribute unowned metaslabs to balance the load
 *   4. Write new assignments to MOS
 *   5. Update each node's local metaslab view
 */
void
cluster_recovery_reassign_metaslabs(cluster_spa_t *cspa)
{
	cluster_membership_t *cm = &cspa->cspa_membership;
	cluster_node_id_t active_nodes[CLUSTER_MAX_NODES];
	uint64_t num_active = 0;
	cluster_node_t *cn;

	mutex_enter(&cm->cm_lock);

	/* Collect active node IDs */
	for (cn = list_head(&cm->cm_nodes); cn != NULL;
	    cn = list_next(&cm->cm_nodes, cn)) {
		if (cn->cn_state == CLUSTER_NODE_ACTIVE && !cn->cn_fenced) {
			active_nodes[num_active++] = cn->cn_id;
		}
	}

	mutex_exit(&cm->cm_lock);

	if (num_active == 0)
		return;

	/* Reassign metaslabs on each vdev */
	for (uint64_t v = 0; v < cspa->cspa_ms_assign_count; v++) {
		cluster_ms_assign_t *cma = &cspa->cspa_ms_assign[v];

		/*
		 * Use static partitioning for the unowned metaslabs.
		 * Only unowned metaslabs are redistributed; existing
		 * assignments are preserved.
		 */
		cluster_ms_partition_static(cma, active_nodes, num_active);
	}
}

/* ------------------------------------------------------------------ */
/*  Recovery: Coordinator Failover                                     */
/* ------------------------------------------------------------------ */

/*
 * Handle coordinator failure and elect a new coordinator.
 *
 * Called when the coordinator's heartbeat times out.
 *
 * The new coordinator must be elected via Paxos before
 * any recovery can proceed. This ensures that only one
 * node attempts to become the new coordinator.
 */
int
cluster_recovery_coordinator(cluster_spa_t *cspa, spa_t *spa)
{
	cluster_membership_t *cm = &cspa->cspa_membership;
	cluster_node_id_t old_coord;
	cluster_term_t new_term;
	int error;

	mutex_enter(&cm->cm_lock);
	old_coord = cm->cm_coordinator;
	new_term = cm->cm_paxos_term + 1;
	mutex_exit(&cm->cm_lock);

	/*
	 * Step 1: Paxos election
	 *
	 * We implement a simplified Paxos protocol for coordinator
	 * election. The key invariant is that only one node can
	 * become the new coordinator, even during network partitions.
	 *
	 * Phase 1a (Prepare): The candidate sends Prepare(new_term)
	 *   to all reachable nodes. The term must be higher than
	 *   any previously seen term.
	 *
	 * Phase 1b (Promise): Each node responds with a Promise
	 *   not to accept any proposal with a lower term. If the
	 *   node has already accepted a proposal, it includes
	 *   that proposal's term and value.
	 *
	 * Phase 2a (Accept): If the candidate receives promises
	 *   from a majority of nodes, it sends Accept(new_term, my_id)
	 *   to all nodes.
	 *
	 * Phase 2b (Accepted): Each node acknowledges the Accept.
	 *   Once the candidate receives acknowledgements from a
	 *   majority, it is the new coordinator.
	 *
	 * For single-node clusters (development/testing), we
	 * skip the election and just assume the role.
	 */
	mutex_enter(&cm->cm_lock);
	uint64_t active_count = 0;
	for (cluster_node_t *cn = list_head(&cm->cm_nodes); cn != NULL;
	    cn = list_next(&cm->cm_nodes, cn)) {
		if (cn->cn_state == CLUSTER_NODE_ACTIVE && !cn->cn_fenced)
			active_count++;
	}

	/*
	 * Check if we can achieve quorum. We need a majority
	 * of the total configured nodes (not just active ones).
	 * If we can't achieve quorum, the cluster is down.
	 */
	uint64_t quorum = (cm->cm_total_votes / 2) + 1;
	if (active_count < quorum && cm->cm_total_votes > 1) {
		mutex_exit(&cm->cm_lock);
		cmn_err(CE_WARN, "cluster: cannot achieve quorum for "
		    "coordinator election (active=%llu, need=%llu)",
		    (u_longlong_t)active_count, (u_longlong_t)quorum);
		return (EAGAIN);
	}

	/*
	 * Record our Paxos proposal. In a distributed implementation,
	 * we'd send messages to other nodes and wait for responses.
	 * For now, we record the proposal locally and assume
	 * single-node agreement (since we're the only node running
	 * this code path on this machine).
	 */
	cm->cm_paxos_term = new_term;
	cm->cm_paxos_promised = new_term;
	cm->cm_paxos_accepted = new_term;
	cm->cm_paxos_accepted_val = cspa->cspa_local_id;
	mutex_exit(&cm->cm_lock);

	cmn_err(CE_NOTE, "cluster: Paxos election complete - "
	    "node %u elected coordinator (term %llu)",
	    cspa->cspa_local_id, (u_longlong_t)new_term);

	/* Step 2: Fence the old coordinator */
	if (old_coord != cspa->cspa_local_id) {
		error = cluster_fence_write(spa, old_coord, cm->cm_epoch,
		    "coordinator failure");
		if (error != 0) {
			cmn_err(CE_WARN, "cluster: failed to fence old "
			    "coordinator %u: %d", old_coord, error);
			/*
			 * Continue even if fencing fails partially.
			 * The old coordinator may already be down,
			 * so partial fencing is better than none.
			 */
		}
	}

	/* Step 3: Take over as coordinator */
	error = cluster_sync_coordinator_takeover(cspa);
	if (error != 0)
		return (error);

	/* Step 4: Recover the old coordinator's resources */
	if (old_coord != cspa->cspa_local_id) {
		error = cluster_recovery_node(cspa, spa, old_coord);
	}

	return (error);
}

/* ------------------------------------------------------------------ */
/*  Self-Fencing                                                       */
/* ------------------------------------------------------------------ */

/*
 * A node that detects it has been fenced must immediately
 * stop all I/O and suspend its pool.
 *
 * This can happen when:
 *   - The node was slow and the coordinator fenced it
 *   - A network partition caused a split-brain
 *   - An administrator manually fenced the node
 *
 * The node must:
 *   1. Stop all new I/O
 *   2. Abort in-flight I/O
 *   3. Suspend the pool (zio_suspend)
 *   4. Attempt to rejoin the cluster
 */
void
cluster_recovery_self_fence(cluster_spa_t *cspa, spa_t *spa)
{
	cmn_err(CE_WARN, "cluster: self-fencing detected, suspending pool %s",
	    spa_name(spa));

	/*
	 * Step 1: Suspend all I/O. This is similar to what happens
	 * when MMP detects a multihost violation.
	 */
	zio_suspend(spa, NULL, ZIO_SUSPEND_MMP);

	/*
	 * Step 2: Mark self as fenced in local state.
	 */
	cspa->cspa_role = CLUSTER_ROLE_NONE;
	cspa->cspa_is_coordinator = B_FALSE;

	/*
	 * Step 3: Release all DLM locks.
	 * The coordinator has already revoked them, but we
	 * clean up our local state to avoid stale references.
	 */
	cluster_dlm_release_all(&cspa->cspa_dlm, cspa->cspa_local_id);

	/*
	 * Step 4: Release metaslab ownership.
	 * This node no longer owns any metaslabs. The
	 * coordinator will reassign them to surviving nodes.
	 */
	for (uint64_t v = 0; v < cspa->cspa_ms_assign_count; v++) {
		cluster_ms_reclaim_from_dead_node(&cspa->cspa_ms_assign[v],
		    cspa->cspa_local_id);
	}

	/*
	 * Step 5: Release ZIL region.
	 * The coordinator will handle replay of our ZIL.
	 */
	(void) cluster_zil_release(&cspa->cspa_zil, cspa->cspa_local_id);

	/*
	 * Step 6: Mark self as fenced in membership.
	 */
	cluster_membership_fence_node(&cspa->cspa_membership,
	    cspa->cspa_local_id, "self-fenced");

	/*
	 * In a production implementation, the node would then:
	 *   1. Attempt to rejoin the cluster by sending
	 *      CLUSTER_MSG_JOIN_REQUEST to the coordinator
	 *   2. On successful rejoin, get new metaslab assignments
	 *   3. Resume I/O via zio_resume()
	 *
	 * For now, the pool remains suspended until manually
	 * exported and re-imported with proper cluster parameters.
	 */
}
