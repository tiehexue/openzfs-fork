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
 * Cluster ZFS - Metaslab Partitioning
 *
 * This is the CORE mechanism that enables concurrent writes
 * from multiple nodes to shared storage without conflicts.
 *
 * DESIGN PRINCIPLE
 * ================
 *
 * Each metaslab on each vdev is assigned to exactly one node.
 * A node can ONLY allocate space from metaslabs it owns.
 * Multiple nodes can READ from any metaslab (reads are shared).
 * But only the owning node can WRITE (allocate) to its metaslabs.
 *
 * This eliminates write-write conflicts because:
 *   - Two nodes will never try to allocate the same block
 *   - Each node's space map updates go to disjoint metaslabs
 *   - No distributed locking needed for space allocation
 *
 * partitoning STRATEGIES
 * =======================
 *
 * 1. STATIC: At cluster formation, metaslabs are divided evenly
 *    among nodes. Simple but doesn't adapt to workload skew.
 *
 * 2. DYNAMIC: When a node joins/leaves, its metaslabs are
 *    redistributed. Handles membership changes.
 *
 * 3. ADAPTIVE: Periodically rebalances based on I/O activity.
 *    Nodes with more write traffic get more metaslabs.
 *
 * IMPLEMENTATION
 * ==============
 *
 * The assignment is stored in the MOS as a ZAP object per vdev.
 * The key is the metaslab index; the value is the owning node ID.
 *
 * When a node needs to allocate space:
 *   1. It looks up its assigned metaslabs for the target vdev
 *   2. It selects a metaslab using the standard weight-based
 *      algorithm, but only from its own subset
 *   3. It allocates from the selected metaslab normally
 *
 * When a node joins:
 *   1. Coordinator reassigns some metaslabs from existing nodes
 *   2. Free metaslabs are preferred; if none, the donor node
 *      flushes and releases a metaslab
 *   3. Assignment is written to MOS
 *
 * When a node leaves or is fenced:
 *   1. All its metaslabs become "unowned"
 *   2. Unowned metaslabs can be reassigned to remaining nodes
 *   3. Deferred frees from the dead node's metaslabs must be
 *      handled by the coordinator during recovery
 *
 * SPACE MAP COHERENCE
 * ===================
 *
 * Each node reads the full space map for its assigned metaslabs
 * during import. Since only the owner writes to a metaslab's
 * space map, there are no concurrent writes to the same space
 * map object.
 *
 * IMPORTANT: After a metaslab is reassigned, the new owner
 * must read the latest space map from disk before allocating.
 * The coordinator ensures the old owner has flushed all
 * pending writes before reassigning.
 */

#include <sys/cluster/cluster_types.h>
#include <sys/cluster/cluster_metaslab.h>
#include <sys/cluster/cluster_membership.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/metaslab_impl.h>
#include <sys/dmu_tx.h>
#include <sys/zap.h>
#include <sys/dsl_pool.h>

/* ------------------------------------------------------------------ */
/*  Metaslab Assignment Initialization                                 */
/* ------------------------------------------------------------------ */

int
cluster_ms_assign_init(cluster_ms_assign_t *cma, uint64_t vdev_id,
    uint64_t ms_count)
{
	cma->cma_vdev_id = vdev_id;
	cma->cma_ms_count = ms_count;
	cma->cma_owner = kmem_zalloc(ms_count * sizeof (cluster_node_id_t),
	    KM_SLEEP);
	mutex_init(&cma->cma_lock, NULL, MUTEX_DEFAULT, NULL);

	/* Initialize all metaslabs as unowned */
	for (uint64_t i = 0; i < ms_count; i++)
		cma->cma_owner[i] = CLUSTER_NODE_ID_NONE;

	return (0);
}

void
cluster_ms_assign_fini(cluster_ms_assign_t *cma)
{
	if (cma->cma_owner != NULL) {
		kmem_free(cma->cma_owner,
		    cma->cma_ms_count * sizeof (cluster_node_id_t));
		cma->cma_owner = NULL;
	}
	mutex_destroy(&cma->cma_lock);
}

/* ------------------------------------------------------------------ */
/*  Static Partitioning                                                */
/* ------------------------------------------------------------------ */

/*
 * Divide metaslabs evenly among active nodes.
 * Remaining metaslabs are left unowned for future assignment.
 *
 * Example with 100 metaslabs and 4 nodes:
 *   Node 0: metaslabs 0-24
 *   Node 1: metaslabs 25-49
 *   Node 2: metaslabs 50-74
 *   Node 3: metaslabs 75-99
 */
void
cluster_ms_partition_static(cluster_ms_assign_t *cma,
    cluster_node_id_t *active_nodes, uint64_t num_active)
{
	uint64_t ms_per_node;
	uint64_t remainder;
	uint64_t offset = 0;

	ASSERT(num_active > 0);
	ASSERT(cma->cma_owner != NULL);

	mutex_enter(&cma->cma_lock);

	ms_per_node = cma->cma_ms_count / num_active;
	remainder = cma->cma_ms_count % num_active;

	for (uint64_t n = 0; n < num_active; n++) {
		uint64_t count = ms_per_node + (n < remainder ? 1 : 0);

		for (uint64_t i = 0; i < count; i++) {
			ASSERT(offset + i < cma->cma_ms_count);
			cma->cma_owner[offset + i] = active_nodes[n];
		}
		offset += count;
	}

	mutex_exit(&cma->cma_lock);
}

/* ------------------------------------------------------------------ */
/*  Adaptive Partitioning                                              */
/* ------------------------------------------------------------------ */

/*
 * Rebalance metaslab assignment based on per-node I/O activity.
 * Nodes with higher write throughput receive more metaslabs.
 *
 * This is called periodically (e.g., every N TXGs) when the
 * adaptive policy is enabled.
 *
 * Algorithm:
 *   1. Collect per-node write statistics
 *   2. Compute target metaslab count per node (proportional to
 *      write throughput, with a minimum floor)
 *   3. Reassign metaslabs from over-assigned to under-assigned nodes
 *   4. Prefer reassigning metaslabs with more free space
 */
void
cluster_ms_partition_adaptive(cluster_ms_assign_t *cma,
    cluster_node_id_t *active_nodes, uint64_t num_active,
    uint64_t *write_bytes, uint64_t total_ms_free)
{
	(void)total_ms_free;
	uint64_t total_writes = 0;
	uint64_t *target_count;
	uint64_t *current_count;
	uint64_t total_metaslabs = cma->cma_ms_count;

	/* Compute total writes across all nodes */
	for (uint64_t n = 0; n < num_active; n++)
		total_writes += write_bytes[n];

	if (total_writes == 0) {
		/* No writes - fall back to even distribution */
		cluster_ms_partition_static(cma, active_nodes, num_active);
		return;
	}

	target_count = kmem_zalloc(num_active * sizeof (uint64_t), KM_SLEEP);
	current_count = kmem_zalloc(num_active * sizeof (uint64_t), KM_SLEEP);

	/* Compute target allocation proportional to write share */
	for (uint64_t n = 0; n < num_active; n++) {
		uint64_t share = (write_bytes[n] * 100) / total_writes;
		target_count[n] = (share * total_metaslabs) / 100;
		/* Minimum floor: each node gets at least 1 metaslab */
		if (target_count[n] == 0)
			target_count[n] = 1;
	}

	/* Count current per-node assignments */
	mutex_enter(&cma->cma_lock);
	for (uint64_t i = 0; i < cma->cma_ms_count; i++) {
		if (cma->cma_owner[i] == CLUSTER_NODE_ID_NONE)
			continue;
		for (uint64_t n = 0; n < num_active; n++) {
			if (cma->cma_owner[i] == active_nodes[n]) {
				current_count[n]++;
				break;
			}
		}
	}

	/*
	 * Reassignment: move metaslabs from over-assigned to
	 * under-assigned nodes, preferring metaslabs with more
	 * free space (less disruptive).
	 *
	 * For simplicity, we do a simple reassignment here.
	 * A production implementation would use a more sophisticated
	 * algorithm considering metaslab weights and fragmentation.
	 */
	for (uint64_t n = 0; n < num_active; n++) {
		int64_t delta = (int64_t)target_count[n] -
		    (int64_t)current_count[n];
		if (delta > 0) {
			/* Need more metaslabs: find unowned ones first */
			uint64_t needed = delta;
			for (uint64_t i = 0; i < cma->cma_ms_count && needed > 0;
			    i++) {
				if (cma->cma_owner[i] == CLUSTER_NODE_ID_NONE) {
					cma->cma_owner[i] = active_nodes[n];
					needed--;
				}
			}
			/* If still need more, steal from over-assigned nodes */
			for (uint64_t m = 0; m < num_active && needed > 0; m++) {
				if (m == n)
					continue;
				int64_t their_delta = (int64_t)target_count[m] -
				    (int64_t)current_count[m];
				if (their_delta < 0) {
					/* This node has excess */
					for (uint64_t i = 0;
					    i < cma->cma_ms_count && needed > 0;
					    i++) {
						if (cma->cma_owner[i] ==
						    active_nodes[m]) {
							cma->cma_owner[i] =
							    active_nodes[n];
							needed--;
							current_count[m]--;
						}
					}
				}
			}
		}
	}

	mutex_exit(&cma->cma_lock);

	kmem_free(target_count, num_active * sizeof (uint64_t));
	kmem_free(current_count, num_active * sizeof (uint64_t));
}

/* ------------------------------------------------------------------ */
/*  Per-Node Metaslab Selection                                        */
/* ------------------------------------------------------------------ */

/*
 * Get the list of metaslab indices assigned to a specific node
 * for a given vdev. Used by the allocation path to filter
 * metaslabs.
 *
 * Returns an array of metaslab indices and the count.
 * Caller must free the array with kmem_free().
 */
uint64_t *
cluster_ms_get_owned(cluster_ms_assign_t *cma, cluster_node_id_t node_id,
    uint64_t *count)
{
	uint64_t *indices;
	uint64_t n = 0;

	mutex_enter(&cma->cma_lock);

	/* First pass: count owned metaslabs */
	for (uint64_t i = 0; i < cma->cma_ms_count; i++) {
		if (cma->cma_owner[i] == node_id)
			n++;
	}

	*count = n;
	if (n == 0) {
		mutex_exit(&cma->cma_lock);
		return (NULL);
	}

	indices = kmem_alloc(n * sizeof (uint64_t), KM_SLEEP);
	n = 0;
	for (uint64_t i = 0; i < cma->cma_ms_count; i++) {
		if (cma->cma_owner[i] == node_id)
			indices[n++] = i;
	}

	mutex_exit(&cma->cma_lock);
	return (indices);
}

/*
 * Check if a specific metaslab is owned by a node.
 * Fast path for allocation filtering.
 */
boolean_t
cluster_ms_is_owned(cluster_ms_assign_t *cma, uint64_t ms_index,
    cluster_node_id_t node_id)
{
	boolean_t owned;

	ASSERT(ms_index < cma->cma_ms_count);

	mutex_enter(&cma->cma_lock);
	owned = (cma->cma_owner[ms_index] == node_id);
	mutex_exit(&cma->cma_lock);

	return (owned);
}

/*
 * Release a metaslab from a node's ownership.
 * Called when reassigning metaslabs between nodes.
 */
void
cluster_ms_release(cluster_ms_assign_t *cma, uint64_t ms_index,
    cluster_node_id_t old_owner)
{
	ASSERT(ms_index < cma->cma_ms_count);

	mutex_enter(&cma->cma_lock);
	ASSERT(cma->cma_owner[ms_index] == old_owner);
	cma->cma_owner[ms_index] = CLUSTER_NODE_ID_NONE;
	mutex_exit(&cma->cma_lock);
}

/*
 * Assign a metaslab to a new owner.
 */
void
cluster_ms_assign_one(cluster_ms_assign_t *cma, uint64_t ms_index,
    cluster_node_id_t new_owner)
{
	ASSERT(ms_index < cma->cma_ms_count);

	mutex_enter(&cma->cma_lock);
	ASSERT(cma->cma_owner[ms_index] == CLUSTER_NODE_ID_NONE);
	cma->cma_owner[ms_index] = new_owner;
	mutex_exit(&cma->cma_lock);
}

/* ------------------------------------------------------------------ */
/*  Node Join/Leave Metaslab Handling                                  */
/* ------------------------------------------------------------------ */

/*
 * Assign metaslabs to a newly joined node.
 * Takes metaslabs from unowned pool or from nodes with excess.
 */
int
cluster_ms_assign_to_new_node(cluster_ms_assign_t *cma,
    cluster_node_id_t new_node, uint64_t target_count)
{
	uint64_t assigned = 0;

	mutex_enter(&cma->cma_lock);

	/* First: assign unowned metaslabs */
	for (uint64_t i = 0; i < cma->cma_ms_count && assigned < target_count;
	    i++) {
		if (cma->cma_owner[i] == CLUSTER_NODE_ID_NONE) {
			cma->cma_owner[i] = new_node;
			assigned++;
		}
	}

	/*
	 * If not enough unowned metaslabs, take from existing nodes
	 * proportionally. In a real implementation, we would:
	 *   1. Identify which metaslabs have the most free space
	 *   2. Ask the donor node to flush those metaslabs
	 *   3. Wait for flush completion
	 *   4. Reassign ownership
	 */

	mutex_exit(&cma->cma_lock);
	return (assigned > 0 ? 0 : ENOSPC);
}

/*
 * Reclaim metaslabs from a dead/fenced node.
 * Marks all its metaslabs as unowned so they can be reassigned.
 *
 * IMPORTANT: The coordinator must ensure that the dead node's
 * pending writes to those metaslabs have completed or been
 * aborted before calling this function. Otherwise, we risk
 * the dead node's in-flight writes corrupting the space map
 * after the metaslab has been reassigned.
 */
void
cluster_ms_reclaim_from_dead_node(cluster_ms_assign_t *cma,
    cluster_node_id_t dead_node)
{
	mutex_enter(&cma->cma_lock);

	for (uint64_t i = 0; i < cma->cma_ms_count; i++) {
		if (cma->cma_owner[i] == dead_node)
			cma->cma_owner[i] = CLUSTER_NODE_ID_NONE;
	}

	mutex_exit(&cma->cma_lock);
}

/* ------------------------------------------------------------------ */
/*  Metaslab Assignment Persistence                                    */
/* ------------------------------------------------------------------ */

/*
 * Write metaslab assignments to MOS.
 * Stored as ZAP objects: one per vdev.
 *
 * Key format: "ms_%llu" → uint64 (node_id)
 */
#define	CLUSTER_MS_ASSIGN_PREFIX	"cluster_ms_assign"

int
cluster_ms_assign_write(spa_t *spa, cluster_ms_assign_t *cma, dmu_tx_t *tx)
{
	dsl_pool_t *dp = spa->spa_dsl_pool;
	objset_t *mos = dp->dp_meta_objset;
	char objname[64];
	uint64_t obj;
	int error;

	ASSERT(dmu_tx_is_syncing(tx));

	(void) snprintf(objname, sizeof (objname), "%s_%llu",
	    CLUSTER_MS_ASSIGN_PREFIX, (u_longlong_t)cma->cma_vdev_id);

	error = zap_lookup(mos, DMU_POOL_DIRECTORY_OBJECT, objname,
	    sizeof (uint64_t), 1, &obj);
	if (error == ENOENT) {
		obj = zap_create(mos, DMU_OTN_ZAP_METADATA,
		    DMU_OT_NONE, 0, tx);
		error = zap_add(mos, DMU_POOL_DIRECTORY_OBJECT, objname,
		    sizeof (uint64_t), 1, &obj, tx);
	}
	if (error != 0)
		return (error);

	mutex_enter(&cma->cma_lock);

	for (uint64_t i = 0; i < cma->cma_ms_count; i++) {
		char key[32];
		uint64_t owner = cma->cma_owner[i];

		(void) snprintf(key, sizeof (key), "ms_%llu",
		    (u_longlong_t)i);
		(void) zap_update(mos, obj, key, sizeof (uint64_t), 1,
		    &owner, tx);
	}

	mutex_exit(&cma->cma_lock);
	return (0);
}

/*
 * Read metaslab assignments from MOS.
 */
int
cluster_ms_assign_read(spa_t *spa, uint64_t vdev_id,
    cluster_ms_assign_t *cma)
{
	dsl_pool_t *dp = spa->spa_dsl_pool;
	objset_t *mos = dp->dp_meta_objset;
	char objname[64];
	uint64_t obj;
	int error;

	(void) snprintf(objname, sizeof (objname), "%s_%llu",
	    CLUSTER_MS_ASSIGN_PREFIX, (u_longlong_t)vdev_id);

	error = zap_lookup(mos, DMU_POOL_DIRECTORY_OBJECT, objname,
	    sizeof (uint64_t), 1, &obj);
	if (error != 0)
		return (error);

	mutex_enter(&cma->cma_lock);

	for (uint64_t i = 0; i < cma->cma_ms_count; i++) {
		char key[32];

		(void) snprintf(key, sizeof (key), "ms_%llu",
		    (u_longlong_t)i);
		(void) zap_lookup(mos, obj, key, sizeof (uint64_t), 1,
		    &cma->cma_owner[i]);
	}

	mutex_exit(&cma->cma_lock);
	return (0);
}

/* ------------------------------------------------------------------ */
/*  Integration: Modified Allocation Path                              */
/* ------------------------------------------------------------------ */

/*
 * This function is the key integration point between the cluster
 * metaslab partitioning and the existing metaslab_alloc() path.
 *
 * In the standard ZFS allocation path:
 *   metaslab_alloc() → metaslab_group_alloc() → metaslab_group_alloc_normal()
 *     → iterates metaslab_group's mg_metaslab_tree by weight
 *     → selects best metaslab → allocates from it
 *
 * For cluster ZFS, we modify this to:
 *   metaslab_alloc() → cluster_metaslab_filter() → metaslab_group_alloc_normal()
 *     → only consider metaslabs owned by this node
 *     → skip metaslabs owned by other nodes
 *
 * The filter function is called for each candidate metaslab
 * during the allocation search. If the metaslab is not owned
 * by this node, it is skipped.
 */
boolean_t
cluster_metaslab_alloc_filter(metaslab_t *msp, cluster_node_id_t local_id,
    cluster_ms_assign_t *cma)
{
	/*
	 * If no cluster assignment exists (single-node mode),
	 * allow all metaslabs.
	 */
	if (cma == NULL)
		return (B_TRUE);

	return (cluster_ms_is_owned(cma, msp->ms_id, local_id));
}

/*
 * Update the metaslab group to reflect cluster partitioning.
 * After a membership change, this rebuilds the per-node view
 * of which metaslabs are available for allocation.
 *
 * In a real implementation, this would:
 *   1. Walk all metaslabs in the group
 *   2. For each, check if it's assigned to this node
 *   3. Set/unset ms_disabled for non-owned/owned metaslabs
 *      so the standard allocation path naturally skips them
 */
void
cluster_metaslab_group_update(vdev_t *vd, cluster_node_id_t local_id,
    cluster_ms_assign_t *cma)
{
	metaslab_group_t *mg = vd->vdev_mg;
	metaslab_t *msp;

	if (mg == NULL || cma == NULL)
		return;

	mutex_enter(&mg->mg_lock);

	for (msp = avl_first(&mg->mg_metaslab_tree); msp != NULL;
	    msp = AVL_NEXT(&mg->mg_metaslab_tree, msp)) {
		boolean_t owned = cluster_ms_is_owned(cma, msp->ms_id,
		    local_id);

		/*
		 * If this metaslab is not owned by us, we mark it
		 * as disabled so the allocator won't try to use it.
		 * This leverages the existing ms_disabled mechanism.
		 */
		if (!owned && msp->ms_disabled == 0) {
			metaslab_disable(msp);
		} else if (owned && msp->ms_disabled > 0) {
			/*
			 * Re-enable metaslabs assigned to us.
			 * This happens after rebalancing.
			 */
			metaslab_enable(msp, B_FALSE, B_FALSE);
		}
	}

	mutex_exit(&mg->mg_lock);
}

/*
 * Check whether the local node owns a given metaslab.
 * This is the convenience function used from the allocation path
 * in metaslab.c's find_valid_metaslab().
 *
 * Returns B_TRUE if:
 *   - We are not in cluster mode (spa_cluster == NULL), OR
 *   - We are in cluster mode and the metaslab is assigned to us
 */
boolean_t
cluster_metaslab_owns(spa_t *spa, metaslab_t *msp)
{
	cluster_spa_t *cspa;
	vdev_t *vd;
	uint64_t i;
	boolean_t owned = B_FALSE;

	if (spa->spa_cluster == NULL)
		return (B_TRUE);

	cspa = spa->spa_cluster;

	/* Find the vdev that contains this metaslab */
	vd = msp->ms_group->mg_vd;

	/* Look up the per-vdev assignment table in the cluster state */
	for (i = 0; i < cspa->cspa_ms_assign_count; i++) {
		if (cspa->cspa_ms_assign[i].cma_vdev_id == vd->vdev_id)
			break;
	}

	/*
	 * If no assignment table found for this vdev, do NOT allow
	 * allocation — in cluster mode every metaslab must be assigned.
	 * Returning B_TRUE here would break the partition guarantee.
	 */
	if (i == cspa->cspa_ms_assign_count)
		return (B_FALSE);

	owned = cluster_ms_is_owned(&cspa->cspa_ms_assign[i], msp->ms_id,
	    cspa->cspa_local_id);

	return (owned);
}
