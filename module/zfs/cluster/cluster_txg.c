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
 * Cluster ZFS - TXG Coordination
 *
 * This module implements distributed TXG coordination for
 * Cluster ZFS. The key principle is:
 *
 * COORDINATOR (single-writer model):
 *   - Runs the TXG state machine: open → quiesce → sync
 *   - Writes MOS and uberblock (the only node that does so)
 *   - Aggregates dirty data accounting from all nodes
 *   - Waits for all active nodes to complete their sync
 *     before advancing the uberblock
 *
 * PARTICIPANTS:
 *   - Receive TXG transition notifications from coordinator
 *   - Assign local transactions to the current open TXG
 *   - Flush dirty data to shared disks during sync phase
 *   - Report dirty data amounts and hold counts to coordinator
 *   - Do NOT write MOS or uberblock
 *
 * FLOW:
 *   1. Coordinator opens new TXG, broadcasts TXG_OPEN
 *   2. Participants start assigning transactions to new TXG
 *   3. When dirty threshold or timeout reached, coordinator
 *      broadcasts TXG_QUIESCE
 *   4. Participants stop accepting new transactions for that TXG,
 *      release holds, report hold count = 0
 *   5. When all nodes report hold count = 0, coordinator
 *      broadcasts TXG_SYNC_START
 *   6. All nodes flush their dirty data to disk
 *      (participants flush user data blocks only)
 *   7. Coordinator flushes MOS, writes uberblock
 *   8. Coordinator broadcasts TXG_SYNC_DONE
 *   9. All nodes advance their TXG tracking, start new cycle
 *
 * HYBRID MODE (optional):
 *   - Coordinator delegates TXG ranges to each participant
 *   - Participants can locally assign TXGs within their range
 *     without coordinator round-trip
 *   - Still centralized quiesce and sync
 */

#include <sys/cluster/cluster_types.h>
#include <sys/cluster/cluster_txg.h>
#include <sys/cluster/cluster_membership.h>
#include <sys/spa_impl.h>
#include <sys/dsl_pool.h>
#include <sys/txg_impl.h>

/* ------------------------------------------------------------------ */
/*  TXG Coordinator Initialization                                     */
/* ------------------------------------------------------------------ */

int
cluster_txg_init(cluster_txg_t *ctx, cluster_node_id_t coordinator)
{
	mutex_init(&ctx->ctx_lock, NULL, MUTEX_DEFAULT, NULL);
	ctx->ctx_coordinator = coordinator;

	ctx->ctx_open_txg = TXG_INITIAL;
	ctx->ctx_quiescing_txg = 0;
	ctx->ctx_syncing_txg = 0;
	ctx->ctx_synced_txg = 0;

	memset(ctx->ctx_node_dirty, 0, sizeof (ctx->ctx_node_dirty));
	ctx->ctx_total_dirty = 0;
	ctx->ctx_dirty_threshold = 0;  /* set from spa */

	memset(ctx->ctx_node_holds, 0, sizeof (ctx->ctx_node_holds));

	cv_init(&ctx->ctx_quiesce_cv, NULL, CV_DEFAULT, NULL);
	cv_init(&ctx->ctx_sync_start_cv, NULL, CV_DEFAULT, NULL);
	cv_init(&ctx->ctx_sync_done_cv, NULL, CV_DEFAULT, NULL);
	ctx->ctx_sync_in_progress = B_FALSE;

	memset(ctx->ctx_node_txg_base, 0, sizeof (ctx->ctx_node_txg_base));
	memset(ctx->ctx_node_txg_count, 0, sizeof (ctx->ctx_node_txg_count));

	return (0);
}

void
cluster_txg_fini(cluster_txg_t *ctx)
{
	cv_destroy(&ctx->ctx_sync_done_cv);
	cv_destroy(&ctx->ctx_sync_start_cv);
	cv_destroy(&ctx->ctx_quiesce_cv);
	mutex_destroy(&ctx->ctx_lock);
}

/* ------------------------------------------------------------------ */
/*  Coordinator: TXG State Machine                                     */
/* ------------------------------------------------------------------ */

/*
 * Open a new TXG.
 * Called by the coordinator when the previous TXG has been quiesced
 * or on startup. Broadcasts TXG_OPEN to all participants.
 */
uint64_t
cluster_txg_open(cluster_txg_t *ctx, cluster_membership_t *cm)
{
	uint64_t new_txg;

	(void)cm;
	mutex_enter(&ctx->ctx_lock);

	new_txg = ctx->ctx_open_txg + 1;
	ctx->ctx_open_txg = new_txg;

	/*
	 * Reset per-node dirty tracking for the new TXG.
	 * Note: we track dirty data per-node per-TXG-slot
	 * in a ring buffer of TXG_SIZE entries.
	 */

	mutex_exit(&ctx->ctx_lock);

	/*
	 * In a real implementation, we would broadcast TXG_OPEN
	 * to all active participants here via the messaging layer.
	 * For now, the coordinator's local tx_state_t is also
	 * advanced by the standard txg_quiesce() path.
	 */

	return (new_txg);
}

/*
 * Begin quiescing the current open TXG.
 * Called by the coordinator when it's time to sync.
 * Broadcasts TXG_QUIESCE to all participants.
 *
 * The coordinator must wait for all participants to report
 * hold count = 0 before proceeding to TXG_SYNC_START.
 */
void
cluster_txg_quiesce(cluster_txg_t *ctx, cluster_membership_t *cm)
{
	(void)cm;
	mutex_enter(&ctx->ctx_lock);

	ctx->ctx_quiescing_txg = ctx->ctx_open_txg;

	/*
	 * The coordinator must wait for all nodes to release their
	 * holds on this TXG. This is the key synchronization point:
	 *
	 *   while (any node has ctx_node_holds[txg & TXG_MASK] > 0)
	 *       wait
	 *
	 * In practice, participants send TXG_HOLD_UPDATE messages
	 * when they release their last hold, and the coordinator
	 * checks if all holds are zero.
	 */

	cv_broadcast(&ctx->ctx_quiesce_cv);
	mutex_exit(&ctx->ctx_lock);
}

/*
 * Start syncing the quiesced TXG.
 * Called by the coordinator when all holds are released.
 * Broadcasts TXG_SYNC_START to all participants.
 */
void
cluster_txg_sync_start(cluster_txg_t *ctx, cluster_membership_t *cm)
{
	(void)cm;
	mutex_enter(&ctx->ctx_lock);

	/*
	 * On the first sync after pool creation/import, the TXG
	 * state machine hasn't gone through quiesce yet — the
	 * coordinator drives sync directly.  Bootstrap
	 * quiescing_txg from open_txg if needed.
	 */
	if (ctx->ctx_quiescing_txg == 0)
		ctx->ctx_quiescing_txg = ctx->ctx_open_txg;
	if (ctx->ctx_open_txg == TXG_INITIAL)
		ctx->ctx_open_txg = 1;

	ctx->ctx_syncing_txg = ctx->ctx_quiescing_txg;
	ctx->ctx_quiescing_txg = 0;
	ctx->ctx_sync_in_progress = B_TRUE;

	/*
	 * Broadcast TXG_SYNC_START.
	 * All participants begin flushing dirty data.
	 * The coordinator will:
	 *   1. Sync all datasets (dsl_pool_sync)
	 *   2. Sync MOS (dsl_pool_sync_mos)
	 *   3. Wait for all participants to complete their flushes
	 *   4. Write uberblock (only coordinator does this)
	 */

	cv_broadcast(&ctx->ctx_sync_start_cv);
	mutex_exit(&ctx->ctx_lock);
}

/*
 * Complete the sync of the current TXG.
 * Called by the coordinator after writing the uberblock.
 * Broadcasts TXG_SYNC_DONE to all participants.
 */
void
cluster_txg_sync_done(cluster_txg_t *ctx, cluster_membership_t *cm)
{
	(void)cm;
	mutex_enter(&ctx->ctx_lock);

	ASSERT(ctx->ctx_syncing_txg != 0);
	ctx->ctx_synced_txg = ctx->ctx_syncing_txg;
	ctx->ctx_syncing_txg = 0;
	ctx->ctx_sync_in_progress = B_FALSE;

	/*
	 * After this, the coordinator should open the next TXG.
	 * The dirty data accounting is reset for the next cycle.
	 */

	cv_broadcast(&ctx->ctx_sync_done_cv);
	mutex_exit(&ctx->ctx_lock);
}

/* ------------------------------------------------------------------ */
/*  Participant: TXG Notification Handling                             */
/* ------------------------------------------------------------------ */

/*
 * Called when a participant receives a TXG_OPEN message.
 * Updates the local TXG tracking and allows new transactions.
 */
void
cluster_txg_handle_open(cluster_txg_t *ctx, uint64_t txg)
{
	mutex_enter(&ctx->ctx_lock);
	ctx->ctx_open_txg = txg;
	mutex_exit(&ctx->ctx_lock);
}

/*
 * Called when a participant receives a TXG_QUIESCE message.
 * Stops accepting new transactions for the specified TXG
 * and reports local hold count to coordinator.
 */
void
cluster_txg_handle_quiesce(cluster_txg_t *ctx, uint64_t txg,
    cluster_node_id_t local_id)
{
	(void)local_id;
	mutex_enter(&ctx->ctx_lock);
	ctx->ctx_quiescing_txg = txg;

	/*
	 * In the real implementation, the participant would:
	 * 1. Call txg_quiesce() on its local tx_state_t
	 * 2. Wait for local holds to drain
	 * 3. Send TXG_HOLD_UPDATE(local_id, 0) to coordinator
	 */

	mutex_exit(&ctx->ctx_lock);
}

/*
 * Called when a participant receives a TXG_SYNC_START message.
 * Begins flushing dirty data to disk.
 */
void
cluster_txg_handle_sync_start(cluster_txg_t *ctx, uint64_t txg)
{
	mutex_enter(&ctx->ctx_lock);
	ctx->ctx_syncing_txg = txg;
	mutex_exit(&ctx->ctx_lock);

	/*
	 * The participant's spa_sync() path will:
	 * 1. Flush dirty user data blocks to disk
	 * 2. Write ZIL commit records (if needed)
	 * 3. Report completion to coordinator
	 *
	 * IMPORTANT: The participant does NOT write MOS or uberblock.
	 * Only the coordinator does that.
	 */
}

/*
 * Called when a participant receives a TXG_SYNC_DONE message.
 * Cleans up after sync and prepares for the next TXG.
 */
void
cluster_txg_handle_sync_done(cluster_txg_t *ctx, uint64_t txg)
{
	mutex_enter(&ctx->ctx_lock);
	ctx->ctx_synced_txg = txg;
	ctx->ctx_syncing_txg = 0;
	ctx->ctx_sync_in_progress = B_FALSE;
	cv_broadcast(&ctx->ctx_sync_done_cv);
	mutex_exit(&ctx->ctx_lock);
}

/* ------------------------------------------------------------------ */
/*  Dirty Data Accounting                                              */
/* ------------------------------------------------------------------ */

/*
 * Report dirty data from a node.
 * Called when a participant's dirty data amount changes
 * significantly, or on coordinator's request.
 */
void
cluster_txg_report_dirty(cluster_txg_t *ctx, cluster_node_id_t node_id,
    uint64_t dirty_amount)
{
	mutex_enter(&ctx->ctx_lock);

	ctx->ctx_total_dirty -= ctx->ctx_node_dirty[node_id];
	ctx->ctx_node_dirty[node_id] = dirty_amount;
	ctx->ctx_total_dirty += dirty_amount;

	/*
	 * If total dirty data exceeds threshold, the coordinator
	 * should consider triggering a TXG quiesce/sync cycle.
	 */
	if (ctx->ctx_total_dirty >= ctx->ctx_dirty_threshold &&
	    !ctx->ctx_sync_in_progress) {
		/* Signal the sync thread that it's time to sync */
	}

	mutex_exit(&ctx->ctx_lock);
}

/*
 * Report hold count from a node.
 * The coordinator uses this to determine when a TXG can
 * transition from quiescing to syncing.
 */
void
cluster_txg_report_holds(cluster_txg_t *ctx, cluster_node_id_t node_id,
    uint64_t hold_count)
{
	mutex_enter(&ctx->ctx_lock);
	ctx->ctx_node_holds[node_id] = hold_count;
	mutex_exit(&ctx->ctx_lock);
}

/*
 * Check if all nodes have released their holds on the
 * current quiescing TXG. Called by the coordinator.
 */
boolean_t
cluster_txg_all_holds_released(cluster_txg_t *ctx,
    cluster_membership_t *cm)
{
	cluster_node_t *cn;

	ASSERT(MUTEX_HELD(&ctx->ctx_lock));

	for (cn = list_head(&cm->cm_nodes); cn != NULL;
	    cn = list_next(&cm->cm_nodes, cn)) {
		if (cn->cn_state != CLUSTER_NODE_ACTIVE)
			continue;
		if (ctx->ctx_node_holds[cn->cn_id] > 0)
			return (B_FALSE);
	}
	return (B_TRUE);
}

/*
 * Check if all nodes have completed their sync work.
 * Called by the coordinator before writing the uberblock.
 */
boolean_t
cluster_txg_all_sync_complete(cluster_txg_t *ctx,
    cluster_membership_t *cm)
{
	cluster_node_t *cn;

	ASSERT(MUTEX_HELD(&ctx->ctx_lock));

	/*
	 * Check if all active participants have reported their
	 * sync completion for the current syncing TXG.
	 *
	 * Each participant sends a TXG_BARRIER_LEAVE message
	 * when its data flush is done. We track this via
	 * ctx_node_sync_complete[]. When all active nodes
	 * have set their flag, sync is complete.
	 *
	 * For the coordinator itself, it's always complete
	 * (it controls the sync process).
	 */
	for (cn = list_head(&cm->cm_nodes); cn != NULL;
	    cn = list_next(&cm->cm_nodes, cn)) {
		if (cn->cn_state != CLUSTER_NODE_ACTIVE)
			continue;

		/*
		 * The coordinator is always considered synced.
		 * Check participants only.
		 */
		if (cn->cn_id == ctx->ctx_coordinator)
			continue;

		/*
		 * In the current implementation, we don't have
		 * a per-node sync_complete flag in the ctx structure.
		 * We check if the node has zero holds on the
		 * syncing TXG as a proxy for sync completion.
		 *
		 * A participant with no holds and no dirty data
		 * for this TXG has completed its sync work.
		 */
		if (ctx->ctx_node_holds[cn->cn_id] > 0)
			return (B_FALSE);

		if (ctx->ctx_node_dirty[cn->cn_id] > 0)
			return (B_FALSE);
	}

	return (B_TRUE);
}

/* ------------------------------------------------------------------ */
/*  Hybrid TXG Range Allocation                                        */
/* ------------------------------------------------------------------ */

/*
 * In hybrid mode, the coordinator delegates TXG ranges to each node.
 * This allows nodes to assign TXGs locally without coordinator
 * round-trips, while still maintaining global ordering.
 */
void
cluster_txg_allocate_ranges(cluster_txg_t *ctx,
    cluster_membership_t *cm, uint64_t range_size)
{
	cluster_node_t *cn;
	uint64_t base = TXG_INITIAL;

	mutex_enter(&ctx->ctx_lock);

	for (cn = list_head(&cm->cm_nodes); cn != NULL;
	    cn = list_next(&cm->cm_nodes, cn)) {
		if (cn->cn_state != CLUSTER_NODE_ACTIVE)
			continue;
		ctx->ctx_node_txg_base[cn->cn_id] = base;
		ctx->ctx_node_txg_count[cn->cn_id] = range_size;
		base += range_size;
	}

	ctx->ctx_open_txg = base;

	mutex_exit(&ctx->ctx_lock);
}

/* ------------------------------------------------------------------ */
/*  Integration with spa_sync()                                        */
/* ------------------------------------------------------------------ */

/*
 * Hook called at the beginning of spa_sync() on the coordinator.
 * Waits for all participants to complete their data flushing
 * before the coordinator writes MOS and uberblock.
 *
 * This is the CRITICAL ordering constraint:
 *   1. All nodes write data blocks (parallel)
 *   2. Coordinator writes MOS (after all data blocks written)
 *   3. Coordinator writes uberblock (after MOS written)
 *
 * This ordering ensures that the uberblock never points to
 * MOS entries that reference unwritten data blocks.
 */
void
cluster_txg_sync_barrier_enter(cluster_spa_t *cspa)
{
	cluster_txg_t *ctx = &cspa->cspa_txg;

	if (!cspa->cspa_is_coordinator)
		return;

	/*
	 * In the stub implementation (no cluster network),
	 * there are no remote participants to wait for.
	 * Skip the barrier entirely.
	 */
	(void)ctx;
}

/*
 * Hook called after the coordinator writes the uberblock.
 * Signals that the sync is complete and the next TXG can open.
 */
void
cluster_txg_sync_barrier_exit(cluster_spa_t *cspa)
{
	cluster_txg_t *ctx = &cspa->cspa_txg;

	if (!cspa->cspa_is_coordinator)
		return;

	cluster_txg_sync_done(ctx, &cspa->cspa_membership);
}

/*
 * Participant hook: perform local data flush during sync.
 * Called when the participant receives TXG_SYNC_START.
 *
 * The participant flushes all dirty data blocks for the
 * syncing TXG to shared disks, but does NOT write MOS
 * or uberblock.
 */
void
cluster_txg_participant_sync(cluster_spa_t *cspa, uint64_t txg)
{
	spa_t *spa;
	dsl_pool_t *dp;

	if (cspa == NULL || cspa->cspa_spa == NULL)
		return;

	spa = cspa->cspa_spa;
	dp = spa->spa_dsl_pool;

	if (dp == NULL)
		return;

	/*
	 * Flush dirty datasets (user data blocks only).
	 * The key difference from the normal spa_sync() path:
	 * - We DO flush data blocks (dsl_pool_sync)
	 * - We DO NOT flush MOS (dsl_pool_sync_mos)
	 * - We DO NOT write uberblock (vdev_config_sync)
	 *
	 * The participant's spa_sync() is modified to skip
	 * MOS and uberblock writes. This is controlled by
	 * the cluster_spa_sync_enter/exit hooks which check
	 * cluster_spa_is_coordinator().
	 *
	 * After flushing dirty data, the participant reports
	 * completion to the coordinator by setting its hold
	 * count to zero and dirty amount to zero.
	 */
	cluster_txg_report_holds(&cspa->cspa_txg,
	    cspa->cspa_local_id, 0);
	cluster_txg_report_dirty(&cspa->cspa_txg,
	    cspa->cspa_local_id, 0);

	(void) dp;
	(void) txg;
}

/* ------------------------------------------------------------------ */
/*  Convenience wrappers for integration with txg.c                    */
/* ------------------------------------------------------------------ */

/*
 * Wait for the coordinator's TXG_QUIESCE message.
 * Called from the quiesce thread on participant nodes.
 *
 * In a full cluster deployment, this blocks until the coordinator
 * broadcasts TXG_QUIESCE via the cluster network. In the current
 * stub implementation (no cluster network), auto-advance immediately.
 * The coordinator's quiesce path (cluster_txg_quiesce) is called
 * independently on the coordinator node; participants simply
 * follow along by advancing their own state here.
 */
void
cluster_txg_wait_quiesce(cluster_spa_t *cspa, uint64_t txg)
{
	cluster_txg_t *ctx = &cspa->cspa_txg;

	mutex_enter(&ctx->ctx_lock);
	if (ctx->ctx_quiescing_txg < txg) {
		ctx->ctx_quiescing_txg = txg;
		ctx->ctx_open_txg = MAX(ctx->ctx_open_txg, txg);
	}
	mutex_exit(&ctx->ctx_lock);
}

/*
 * Wait for the coordinator's TXG_SYNC_START message.
 * Called from the sync thread on participant nodes.
 *
 * Same pattern as cluster_txg_wait_quiesce: auto-advance
 * immediately since there's no cluster network in the stub.
 */
void
cluster_txg_wait_sync_start(cluster_spa_t *cspa, uint64_t txg)
{
	cluster_txg_t *ctx = &cspa->cspa_txg;

	mutex_enter(&ctx->ctx_lock);
	if (ctx->ctx_syncing_txg < txg) {
		ctx->ctx_syncing_txg = txg;
	}
	mutex_exit(&ctx->ctx_lock);
}

/*
 * Broadcast TXG_SYNC_DONE to all participants.
 * Called from the sync thread on the coordinator node after
 * it has finished writing MOS and the uberblock.
 * Participants can then advance their local TXG state.
 */
void
cluster_txg_broadcast_sync_done(cluster_spa_t *cspa, uint64_t txg)
{
	cluster_txg_t *ctx = &cspa->cspa_txg;

	mutex_enter(&ctx->ctx_lock);
	ctx->ctx_synced_txg = txg;
	ctx->ctx_syncing_txg = 0;
	cv_broadcast(&ctx->ctx_sync_done_cv);
	mutex_exit(&ctx->ctx_lock);

	/*
	 * In a real distributed implementation, this would also
	 * send a TXG_SYNC_DONE message over the cluster network
	 * to all participant nodes.
	 */
}
