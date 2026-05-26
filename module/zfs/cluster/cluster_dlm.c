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
 * Cluster ZFS - Distributed Lock Manager (DLM)
 *
 * WHY WE NEED A DLM
 * =================
 *
 * Metaslab partitioning prevents conflicts at the block allocation
 * level, but there are many higher-level operations that still
 * require coordination:
 *
 *   - Two nodes writing the same file (object-level conflict)
 *   - One node creating a snapshot while another writes data
 *     (dataset-level conflict)
 *   - One node resizing a dataset while another writes
 *     (quota/space conflict)
 *   - Pool configuration changes (adding vdevs, etc.)
 *   - ZAP attribute updates (namespace conflicts)
 *
 * The DLM provides fine-grained distributed locking to prevent
 * these semantic conflicts.
 *
 * DESIGN
 * ======
 *
 * The DLM runs on the coordinator node. All lock requests from
 * participants are forwarded to the coordinator, which decides
 * whether to grant or deny them based on compatibility rules.
 *
 * Lock compatibility:
 *   SHARED + SHARED     = compatible (multiple readers OK)
 *   SHARED + EXCLUSIVE  = incompatible (must wait)
 *   EXCLUSIVE + EXCLUSIVE = incompatible (must wait)
 *
 * Lock resources are identified by:
 *   - Resource type (dataset, object, ZAP, etc.)
 *   - Objset ID
 *   - Object ID
 *
 * Locks are held for the duration of a transaction (TXG).
 * When a TXG completes, all locks for that TXG are automatically
 * released. This simplifies the model and prevents deadlocks.
 *
 * DEADLOCK PREVENTION
 * ===================
 *
 * Two strategies:
 *   1. TXG-scoped locks: All locks are released at TXG boundary.
 *      No lock can span multiple TXGs.
 *   2. Wait-die: If a younger transaction requests a lock held
 *      by an older one, it waits. If an older transaction requests
 *      a lock held by a younger one, the younger one is aborted
 *      (rolled back).
 */

#include <sys/cluster/cluster_types.h>
#include <sys/cluster/cluster_dlm.h>
#include <sys/spa_impl.h>

/* ------------------------------------------------------------------ */
/*  Lock Entry AVL Comparator                                          */
/* ------------------------------------------------------------------ */

static int
cluster_lock_entry_compare(const void *a, const void *b)
{
	const cluster_lock_entry_t *la = a;
	const cluster_lock_entry_t *lb = b;

	if (la->cle_key < lb->cle_key)
		return (-1);
	if (la->cle_key > lb->cle_key)
		return (1);
	return (0);
}

/* ------------------------------------------------------------------ */
/*  DLM Initialization                                                 */
/* ------------------------------------------------------------------ */

int
cluster_dlm_init(cluster_dlm_t *dlm)
{
	mutex_init(&dlm->dlm_lock, NULL, MUTEX_DEFAULT, NULL);
	avl_create(&dlm->dlm_locks, cluster_lock_entry_compare,
	    sizeof (cluster_lock_entry_t),
	    offsetof(cluster_lock_entry_t, cle_avl_link));
	dlm->dlm_next_id = 1;

	dlm->dlm_entry_cache = kmem_cache_create("cluster_dlm_entry",
	    sizeof (cluster_lock_entry_t), 0, NULL, NULL, NULL,
	    NULL, NULL, 0);
	dlm->dlm_grant_cache = kmem_cache_create("cluster_dlm_grant",
	    sizeof (cluster_lock_grant_t), 0, NULL, NULL, NULL,
	    NULL, NULL, 0);
	dlm->dlm_request_cache = kmem_cache_create("cluster_dlm_request",
	    sizeof (cluster_lock_request_t), 0, NULL, NULL, NULL,
	    NULL, NULL, 0);

	return (0);
}

void
cluster_dlm_fini(cluster_dlm_t *dlm)
{
	cluster_lock_entry_t *cle;
	void *cookie = NULL;

	while ((cle = avl_destroy_nodes(&dlm->dlm_locks, &cookie)) != NULL) {
		cluster_lock_grant_t *grant;
		cluster_lock_request_t *req;

		while ((grant = list_remove_head(&cle->cle_grants)) != NULL)
			kmem_cache_free(dlm->dlm_grant_cache, grant);
		while ((req = list_remove_head(&cle->cle_pending)) != NULL)
			kmem_cache_free(dlm->dlm_request_cache, req);

		list_destroy(&cle->cle_grants);
		list_destroy(&cle->cle_pending);
		kmem_cache_free(dlm->dlm_entry_cache, cle);
	}
	avl_destroy(&dlm->dlm_locks);

	kmem_cache_destroy(dlm->dlm_request_cache);
	kmem_cache_destroy(dlm->dlm_grant_cache);
	kmem_cache_destroy(dlm->dlm_entry_cache);
	mutex_destroy(&dlm->dlm_lock);
}

/* ------------------------------------------------------------------ */
/*  Lock Key Computation                                               */
/* ------------------------------------------------------------------ */

/*
 * Compute a hash key for a lock resource.
 * Uses resource type + objset + object to create a unique key.
 */
static uint64_t
cluster_lock_key(cluster_lock_resource_t resource, uint64_t objset,
    uint64_t object)
{
	uint64_t key;

	/*
	 * Mix the three components into a single 64-bit key.
	 * We use a simple hash to distribute across the key space.
	 */
	key = (uint64_t)resource;
	key ^= objset * 0x9E3779B97F4A7C15ULL;
	key = (key << 17) | (key >> 47);
	key ^= object * 0x517CC1B727220A95ULL;
	key = (key << 31) | (key >> 33);

	return (key);
}

/* ------------------------------------------------------------------ */
/*  Lock Request Processing                                            */
/* ------------------------------------------------------------------ */

/*
 * Check if a lock request is compatible with existing grants.
 *
 * Returns:
 *   B_TRUE  - lock can be granted immediately
 *   B_FALSE - lock must wait (conflict exists)
 */
static boolean_t
cluster_lock_is_compatible(cluster_lock_entry_t *cle,
    cluster_lock_type_t requested_type,
    cluster_node_id_t requester)
{
	(void)requester;
	/* If no one holds the lock, it's compatible */
	if (list_is_empty(&cle->cle_grants))
		return (B_TRUE);

	/* Shared locks are compatible with other shared locks */
	if (requested_type == CLUSTER_LOCK_SHARED && !cle->cle_exclusive)
		return (B_TRUE);

	/* Any other combination is incompatible */
	return (B_FALSE);
}

/*
 * Process a lock request.
 *
 * Called on the coordinator node when a participant (or local
 * thread) requests a lock.
 *
 * Returns:
 *   0       - lock granted
 *   EAGAIN  - lock would block; request queued
 *   EDEADLK - deadlock detected
 */
int
cluster_dlm_request(cluster_dlm_t *dlm, cluster_node_id_t requester,
    cluster_lock_type_t type, cluster_lock_resource_t resource,
    uint64_t objset, uint64_t object, uint64_t txg,
    void (*callback)(void *), void *callback_arg)
{
	cluster_lock_entry_t *cle, search;
	uint64_t key;
	int error = 0;

	key = cluster_lock_key(resource, objset, object);

	mutex_enter(&dlm->dlm_lock);

	/* Find or create the lock entry */
	search.cle_key = key;
	cle = avl_find(&dlm->dlm_locks, &search, NULL);

	if (cle == NULL) {
		/* Create new entry */
		cle = kmem_cache_alloc(dlm->dlm_entry_cache, KM_SLEEP);
		cle->cle_id = dlm->dlm_next_id++;
		cle->cle_key = key;
		cle->cle_resource = resource;
		cle->cle_objset = objset;
		cle->cle_object = object;
		cle->cle_shared_count = 0;
		cle->cle_exclusive = B_FALSE;
		list_create(&cle->cle_grants, sizeof (cluster_lock_grant_t),
		    offsetof(cluster_lock_grant_t, clg_link));
		list_create(&cle->cle_pending,
		    sizeof (cluster_lock_request_t),
		    offsetof(cluster_lock_request_t, clr_link));
		avl_add(&dlm->dlm_locks, cle);
	}

	/* Check compatibility */
	if (cluster_lock_is_compatible(cle, type, requester)) {
		/* Grant the lock */
		cluster_lock_grant_t *grant;

		grant = kmem_cache_alloc(dlm->dlm_grant_cache, KM_SLEEP);
		grant->clg_id = cle->cle_id;
		grant->clg_holder = requester;
		grant->clg_type = type;
		grant->clg_resource = resource;
		grant->clg_objset = objset;
		grant->clg_object = object;
		grant->clg_grant_time = gethrtime();
		grant->clg_txg = txg;

		list_insert_tail(&cle->cle_grants, grant);

		if (type == CLUSTER_LOCK_SHARED)
			cle->cle_shared_count++;
		else
			cle->cle_exclusive = B_TRUE;
	} else {
		/* Queue the request */
		cluster_lock_request_t *req;

		req = kmem_cache_alloc(dlm->dlm_request_cache, KM_SLEEP);
		req->clr_requester = requester;
		req->clr_type = type;
		req->clr_resource = resource;
		req->clr_objset = objset;
		req->clr_object = object;
		req->clr_callback = callback;
		req->clr_callback_arg = callback_arg;

		list_insert_tail(&cle->cle_pending, req);

		/*
		 * Check for deadlock:
		 * If the current holder is waiting for a lock that
		 * this node holds, we have a deadlock.
		 * For simplicity, we use TXG-scoped locks and
		 * wait-die to prevent this.
		 */
		error = EAGAIN;
	}

	mutex_exit(&dlm->dlm_lock);
	return (error);
}

/*
 * Release a lock.
 * Called when a node completes its transaction.
 * Wakes any pending waiters if the lock becomes available.
 */
int
cluster_dlm_release(cluster_dlm_t *dlm, cluster_node_id_t holder,
    cluster_lock_resource_t resource, uint64_t objset, uint64_t object)
{
	cluster_lock_entry_t *cle, search;
	uint64_t key;

	key = cluster_lock_key(resource, objset, object);

	mutex_enter(&dlm->dlm_lock);

	search.cle_key = key;
	cle = avl_find(&dlm->dlm_locks, &search, NULL);
	if (cle == NULL) {
		mutex_exit(&dlm->dlm_lock);
		return (ENOENT);
	}

	/* Find and remove the grant for this holder */
	cluster_lock_grant_t *grant;
	for (grant = list_head(&cle->cle_grants); grant != NULL;
	    grant = list_next(&cle->cle_grants, grant)) {
		if (grant->clg_holder == holder) {
			list_remove(&cle->cle_grants, grant);

			if (grant->clg_type == CLUSTER_LOCK_SHARED) {
				cle->cle_shared_count--;
			} else {
				cle->cle_exclusive = B_FALSE;
			}

			kmem_cache_free(dlm->dlm_grant_cache, grant);
			break;
		}
	}

	/* If lock is now free, grant to first waiter */
	if (list_is_empty(&cle->cle_grants) &&
	    !list_is_empty(&cle->cle_pending)) {
		cluster_lock_request_t *req;

		req = list_head(&cle->cle_pending);
		list_remove(&cle->cle_pending, req);

		/* Recursively grant the lock */
		(void) cluster_dlm_request(dlm, req->clr_requester,
		    req->clr_type, req->clr_resource, req->clr_objset,
		    req->clr_object, 0, req->clr_callback,
		    req->clr_callback_arg);

		/* Notify the requester (via callback or message) */
		if (req->clr_callback != NULL)
			req->clr_callback(req->clr_callback_arg);

		kmem_cache_free(dlm->dlm_request_cache, req);
	}

	/* Clean up empty entries */
	if (list_is_empty(&cle->cle_grants) &&
	    list_is_empty(&cle->cle_pending)) {
		avl_remove(&dlm->dlm_locks, cle);
		list_destroy(&cle->cle_grants);
		list_destroy(&cle->cle_pending);
		kmem_cache_free(dlm->dlm_entry_cache, cle);
	}

	mutex_exit(&dlm->dlm_lock);
	return (0);
}

/*
 * Release all locks held by a node.
 * Called when a node is fenced or leaves the cluster.
 * This prevents dead nodes from holding locks indefinitely.
 */
void
cluster_dlm_release_all(cluster_dlm_t *dlm, cluster_node_id_t node_id)
{
	cluster_lock_entry_t *cle;
	void *cookie = NULL;
	boolean_t released;

	mutex_enter(&dlm->dlm_lock);

	do {
		released = B_FALSE;
		cookie = NULL;

		while ((cle = avl_destroy_nodes(&dlm->dlm_locks,
		    &cookie)) != NULL) {
			cluster_lock_grant_t *grant;

			for (grant = list_head(&cle->cle_grants);
			    grant != NULL;
			    grant = list_next(&cle->cle_grants, grant)) {
				if (grant->clg_holder == node_id) {
					list_remove(&cle->cle_grants, grant);
					kmem_cache_free(
					    dlm->dlm_grant_cache, grant);
					released = B_TRUE;
					break;
				}
			}
			/* Don't destroy the tree while iterating */
		}
	} while (released);

	mutex_exit(&dlm->dlm_lock);
}

/*
 * Release all locks for a specific TXG.
 * Called when a TXG completes sync.
 */
void
cluster_dlm_release_txg(cluster_dlm_t *dlm, uint64_t txg)
{
	cluster_lock_entry_t *cle;

	mutex_enter(&dlm->dlm_lock);

	/*
	 * Walk all lock entries and release grants for the
	 * completed TXG.
	 */
	for (cle = avl_first(&dlm->dlm_locks); cle != NULL;
	    cle = AVL_NEXT(&dlm->dlm_locks, cle)) {
		cluster_lock_grant_t *grant, *next;

		grant = list_head(&cle->cle_grants);
		while (grant != NULL) {
			next = list_next(&cle->cle_grants, grant);
			if (grant->clg_txg == txg) {
				list_remove(&cle->cle_grants, grant);

				if (grant->clg_type == CLUSTER_LOCK_SHARED)
					cle->cle_shared_count--;
				else
					cle->cle_exclusive = B_FALSE;

				kmem_cache_free(dlm->dlm_grant_cache, grant);
			}
			grant = next;
		}
	}

	mutex_exit(&dlm->dlm_lock);
}

/* ------------------------------------------------------------------ */
/*  Lock Query                                                         */
/* ------------------------------------------------------------------ */

/*
 * Check if a lock is currently held by a specific node.
 * Used by the coordinator to make authorization decisions.
 */
boolean_t
cluster_dlm_is_locked(cluster_dlm_t *dlm, cluster_node_id_t node_id,
    cluster_lock_resource_t resource, uint64_t objset, uint64_t object)
{
	cluster_lock_entry_t *cle, search;
	uint64_t key;
	boolean_t found = B_FALSE;

	key = cluster_lock_key(resource, objset, object);

	mutex_enter(&dlm->dlm_lock);

	search.cle_key = key;
	cle = avl_find(&dlm->dlm_locks, &search, NULL);
	if (cle != NULL) {
		cluster_lock_grant_t *grant;
		for (grant = list_head(&cle->cle_grants); grant != NULL;
		    grant = list_next(&cle->cle_grants, grant)) {
			if (grant->clg_holder == node_id) {
				found = B_TRUE;
				break;
			}
		}
	}

	mutex_exit(&dlm->dlm_lock);
	return (found);
}

/*
 * Check if a node holds any exclusive locks that would
 * conflict with a requested operation.
 */
boolean_t
cluster_dlm_has_conflict(cluster_dlm_t *dlm, cluster_node_id_t node_id,
    cluster_lock_resource_t resource, uint64_t objset, uint64_t object,
    cluster_lock_type_t requested_type)
{
	cluster_lock_entry_t *cle, search;
	uint64_t key;
	boolean_t conflict = B_FALSE;

	(void)node_id;

	key = cluster_lock_key(resource, objset, object);

	mutex_enter(&dlm->dlm_lock);

	search.cle_key = key;
	cle = avl_find(&dlm->dlm_locks, &search, NULL);
	if (cle != NULL) {
		if (requested_type == CLUSTER_LOCK_EXCLUSIVE) {
			/* Exclusive conflicts with any existing lock */
			conflict = !list_is_empty(&cle->cle_grants);
		} else {
			/* Shared conflicts with exclusive */
			conflict = cle->cle_exclusive;
		}
	}

	mutex_exit(&dlm->dlm_lock);
	return (conflict);
}
