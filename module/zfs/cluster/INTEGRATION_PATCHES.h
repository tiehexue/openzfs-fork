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
 * INTEGRATION PATCHES FOR EXISTING ZFS FILES
 *
 * This file documents the minimal changes needed to existing ZFS source
 * files to integrate the cluster subsystem. Each section shows the
 * original code and the cluster-aware replacement.
 *
 * Changes are designed to be:
 *   - Minimal: only a few lines per file
 *   - Conditional: only active when cluster mode is enabled
 *   - Safe: no impact on single-node operation when cluster=off
 */

/* ================================================================== */
/*  1. include/sys/spa_impl.h                                         */
/* ================================================================== */

/*
 * ADD: Forward declaration and spa_cluster pointer in spa_t
 *
 * Already applied:
 *   - Forward declaration of cluster_spa_t added
 *   - spa_cluster field added to spa_t struct
 *
 * The spa_cluster pointer is NULL for single-node pools and
 * points to cluster_spa_t for cluster-enabled pools.
 */


/* ================================================================== */
/*  2. module/zfs/spa.c — spa_sync()                                  */
/* ================================================================== */

/*
 * MODIFY: spa_sync() to add cluster coordination hooks
 *
 * At the beginning of spa_sync(), add cluster sync enter:
 *
 *   void
 *   spa_sync(spa_t *spa, uint64_t txg)
 *   {
 * +     // Cluster: enter sync coordination
 * +     if (spa->spa_cluster != NULL)
 * +         cluster_spa_sync_enter(spa, txg);
 * +
 *       vdev_t *vd = NULL;
 *       VERIFY(spa_writeable(spa));
 *       ...
 *
 * At the end of spa_sync(), before the final config exit:
 *
 *       spa->spa_ubsync = spa->spa_uberblock;
 *       spa_config_exit(spa, SCL_CONFIG, FTAG);
 *
 * +     // Cluster: signal sync complete
 * +     if (spa->spa_cluster != NULL)
 * +         cluster_spa_sync_exit(spa, txg);
 * +
 *       spa_handle_ignored_writes(spa);
 */


/* ================================================================== */
/*  3. module/zfs/spa.c — spa_sync_iterate_to_convergence()           */
/* ================================================================== */

/*
 * MODIFY: Skip dsl_pool_sync MOS writes on participant nodes
 *
 * In cluster mode, only the coordinator writes the MOS.
 * Participants still flush their data blocks, but skip
 * the MOS sync and space map updates.
 *
 *   static void
 *   spa_sync_iterate_to_convergence(spa_t *spa, dmu_tx_t *tx)
 *   {
 *       ...
 *       do {
 *           ...
 * -         dsl_pool_sync(dp, txg);
 * +         // Cluster: participants skip MOS write, coordinator syncs all
 * +         if (spa->spa_cluster == NULL ||
 * +             cluster_spa_is_coordinator(spa)) {
 * +             dsl_pool_sync(dp, txg);
 * +         } else {
 * +             // Participant: only flush data blocks, skip MOS
 * +             dsl_pool_sync_data_only(dp, txg);
 * +         }
 *           ...
 *       } while (dmu_objset_is_dirty(mos, txg));
 *   }
 *
 * Note: dsl_pool_sync_data_only() is a new function that
 * syncs dirty user data without writing the MOS.
 */


/* ================================================================== */
/*  4. module/zfs/spa.c — spa_sync_rewrite_vdev_config()              */
/* ================================================================== */

/*
 * MODIFY: Skip uberblock write on participant nodes
 *
 * Only the coordinator writes the uberblock. Participants
 * must not update vdev labels.
 *
 *   static void
 *   spa_sync_rewrite_vdev_config(spa_t *spa, dmu_tx_t *tx)
 *   {
 * +     // Cluster: only coordinator writes uberblock
 * +     if (spa->spa_cluster != NULL &&
 * +         !cluster_spa_is_coordinator(spa))
 * +         return;
 * +
 *       vdev_t *rvd = spa->spa_root_vdev;
 *       ...
 */


/* ================================================================== */
/*  5. module/zfs/metaslab.c — allocation filter                      */
/* ================================================================== */

/*
 * ADD: Cluster metaslab ownership check in allocation path
 *
 * The key integration point is in the metaslab allocation
 * selection. We need to filter out metaslabs not owned by
 * this node during allocation.
 *
 * In metaslab_class_allocatable() or the allocation rotor
 * (metaslab_group_alloc_normal()), add:
 *
 * +   // Cluster: skip metaslabs not owned by this node
 * +   if (spa->spa_cluster != NULL &&
 * +       !cluster_metaslab_owns(spa, ms)) {
 * +       continue;
 * +   }
 *
 * This can also be implemented by disabling non-owned metaslabs
 * via the existing ms_disabled mechanism at membership change time.
 * The cluster_metaslab_group_update() function handles this.
 */


/* ================================================================== */
/*  6. module/zfs/txg.c — TXG coordination                            */
/* ================================================================== */

/*
 * MODIFY: Integrate cluster TXG coordination
 *
 * The existing txg_sync_thread() and txg_quiesce_thread()
 * are modified to coordinate with the cluster TXG state machine.
 *
 * In txg_sync_thread():
 *
 *   static void
 *   txg_sync_thread(void *arg)
 *   {
 *       ...
 *       while (!txg_stopped(dp)) {
 *           ...
 * +         // Cluster: wait for coordinator's TXG_SYNC_START
 * +         if (spa->spa_cluster != NULL)
 * +             cluster_txg_wait_sync_start(spa->spa_cluster, txg);
 * +
 *           spa_sync(spa, txg);
 *           ...
 *       }
 *   }
 *
 * In txg_quiesce_thread():
 *
 *   static void
 *   txg_quiesce_thread(void *arg)
 *   {
 *       ...
 *       while (!txg_stopped(dp)) {
 *           ...
 * +         // Cluster: wait for coordinator's TXG_QUIESCE
 * +         if (spa->spa_cluster != NULL)
 * +             cluster_txg_wait_quiesce(spa->spa_cluster, txg);
 * +
 *           txg_quiesce(dp, txg);
 *           ...
 *       }
 *   }
 *
 * For the coordinator node, the TXG transitions are initiated
 * locally (same as single-node). For participant nodes, they
 * wait for broadcast messages from the coordinator.
 */


/* ================================================================== */
/*  7. module/zfs/dmu_tx.c — dirty data reporting                     */
/* ================================================================== */

/*
 * ADD: Report dirty data to cluster TXG coordinator
 *
 * When a transaction is assigned to a TXG, the local dirty
 * data accounting must be reported to the coordinator.
 *
 * In dmu_tx_assign():
 *
 *   int
 *   dmu_tx_assign(dmu_tx_t *tx, txg_how_t th)
 *   {
 *       ...
 *       tx->tx_txg = txg_hold_open(tx->tx_pool, tx);
 * +     // Cluster: report dirty data to coordinator
 * +     if (tx->tx_pool->dp_spa->spa_cluster != NULL)
 * +         cluster_txg_dirty_add(tx->tx_pool->dp_spa->spa_cluster,
 * +             tx->tx_txg, tx->tx_space_written);
 *       ...
 *   }
 */


/* ================================================================== */
/*  8. module/zfs/vdev_label.c — uberblock extension                   */
/* ================================================================== */

/*
 * MODIFY: Extend uberblock write with cluster epoch
 *
 * The cluster uses spare bits in the MMP fields of the uberblock
 * to store the coordinator node ID and cluster epoch.
 *
 * In vdev_label_write_ub():
 *
 *   // Before writing the uberblock to the label:
 * + if (spa->spa_cluster != NULL) {
 * +     cluster_sync_encode_ub(spa, ub);
 * + }
 *
 * In uberblock_verify():
 *
 *   // After reading and verifying the uberblock:
 * + if (spa->spa_cluster != NULL) {
 * +     error = cluster_sync_validate_ub(spa, ub);
 * +     if (error != 0)
 * +         return (error);
 * + }
 *
 * The encoding uses:
 *   ub_mmp_config bits 48-63 → coordinator node ID
 *   ub_mmp_delay          → cluster epoch number
 */


/* ================================================================== */
/*  9. module/zfs/zil.c — per-node ZIL regions                        */
/* ================================================================== */

/*
 * MODIFY: ZIL commit path to use cluster regions
 *
 * In zil_lwb_write_start() (or zil_commit_writer()):
 *
 * + if (zilog->zl_spa->spa_cluster != NULL) {
 * +     cluster_zil_t *cz = &zilog->zl_spa->spa_cluster->cspa_zil;
 * +     cluster_node_id_t nid = zilog->zl_spa->spa_cluster->cspa_local_id;
 * +     // Allocate LWB from cluster ZIL region
 * +     lwb->lwb_blk = cluster_zil_alloc_block(cz, nid, lwb_size);
 * + } else {
 *      // Normal ZIL allocation path
 *      ...
 * + }
 *
 * In zil_sync() (ZIL cleanup after TXG sync):
 *
 * + if (zilog->zl_spa->spa_cluster != NULL) {
 * +     cluster_zil_free_block(cz, nid, offset, size);
 * + } else {
 *      // Normal ZIL free path
 *      ...
 * + }
 */


/* ================================================================== */
/*  10. module/zfs/mmp.c — cluster-aware MMP                          */
/* ================================================================== */

/*
 * MODIFY: Extend MMP for cluster awareness
 *
 * The MMP (Multihost Protection) mechanism is extended to:
 *   1. Allow multiple nodes to write MMP blocks (cluster mode)
 *   2. Include cluster epoch in MMP writes
 *   3. Detect fenced nodes via MMP
 *
 * In mmp_thread():
 *
 * + if (spa->spa_cluster != NULL) {
 * +     // Cluster mode: include node ID and epoch in MMP write
 * +     mmp_seq = cluster_sync_mmp_encode(spa, mmp_seq);
 * + }
 *
 * In mmp_check_uniqueness():
 *
 * + if (spa->spa_cluster != NULL) {
 * +     // Allow MMP from other cluster nodes
 * +     if (cluster_sync_mmp_validate(spa, ub))
 * +         return (B_TRUE);  // valid cluster MMP
 * + }
 */


/* ================================================================== */
/*  11. module/zfs/dsl_pool.c — data-only sync                         */
/* ================================================================== */

/*
 * ADD: dsl_pool_sync_data_only() for participant nodes
 *
 * This is a new function that syncs dirty user data blocks
 * without writing the MOS. Participant nodes call this
 * instead of dsl_pool_sync().
 *
 *   void
 *   dsl_pool_sync_data_only(dsl_pool_t *dp, uint64_t txg)
 *   {
 *       // Sync dirty datasets (user data blocks only)
 *       dsl_dataset_t *ds;
 *       while ((ds = txg_list_remove(&dp->dp_dirty_datasets, txg)) != NULL) {
 *           dsl_dataset_sync(ds, txg);
 *       }
 *
 *       // Do NOT sync the MOS, space maps, or pool config
 *       // The coordinator handles those
 *   }
 */


/* ================================================================== */
/*  12. module/zfs/spa.c — pool import/export                          */
/* ================================================================== */

/*
 * MODIFY: spa_open() and spa_import() for cluster mode
 *
 * In spa_import():
 *
 * + if (cluster_mode_requested) {
 * +     error = cluster_spa_import(spa, node_id);
 * +     if (error != 0)
 * +         goto fail;
 * + }
 *
 * In spa_export():
 *
 * + if (spa->spa_cluster != NULL) {
 * +     error = cluster_spa_export(spa);
 * +     if (error != 0)
 * +         return (error);
 * + }
 */
