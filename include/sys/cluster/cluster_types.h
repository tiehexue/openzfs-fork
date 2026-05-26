/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2026 Cluster ZFS Project. All rights reserved.
 */

/*
 * Cluster ZFS - Core Type Definitions
 *
 * This header defines the fundamental types and constants for
 * transforming ZFS from a single-node filesystem into a
 * cluster-coordinated shared-disk filesystem.
 *
 * ARCHITECTURE OVERVIEW
 * =====================
 *
 * Cluster ZFS enables multiple nodes to share a single zpool on
 * shared storage (e.g., NVMe-oF, shared SAS, FC SAN) with:
 *
 *   1. Metaslab partitioning: each node owns a disjoint subset of
 *      metaslabs per vdev, eliminating write-write conflicts on
 *      space allocation.
 *
 *   2. Centralized TXG generation: a designated coordinator node
 *      drives the TXG state machine (open → quiesce → sync),
 *      ensuring globally consistent transaction ordering.
 *
 *   3. Single MOS/uberblock writer: only the coordinator node
 *      updates the MOS and writes uberblocks, preventing
 *      on-disk structural corruption.
 *
 *   4. Distributed lock manager: fine-grained locking on
 *      datasets, directories, and objects prevents semantic
 *      conflicts (two nodes writing the same file, etc.).
 *
 *   5. Cluster ZIL: a shared intent log with per-node
 *      reservation slots, providing crash consistency across
 *      the cluster.
 *
 *   6. Membership and fencing: Paxos-based membership with
 *      disk-backed fencing ensures split-brain safety.
 */

#ifndef _SYS_CLUSTER_CLUSTER_TYPES_H
#define	_SYS_CLUSTER_CLUSTER_TYPES_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <sys/param.h>
#include <sys/list.h>
#include <sys/avl.h>
#include <sys/zfs_context.h>

/* ------------------------------------------------------------------ */
/*  Node Identity                                                      */
/* ------------------------------------------------------------------ */

#define	CLUSTER_MAX_NODES		32
#define	CLUSTER_NODE_ID_NONE		((cluster_node_id_t)-1)
#define	CLUSTER_NODE_ID_COORDINATOR	0	/* coordinator is always node 0 */

typedef uint32_t	cluster_node_id_t;
typedef uint64_t	cluster_epoch_t;
typedef uint64_t	cluster_term_t;	/* Paxos term */

/*
 * Node states in the cluster membership.
 */
typedef enum cluster_node_state {
	CLUSTER_NODE_NONE = 0,		/* uninitialized */
	CLUSTER_NODE_JOINING,		/* requesting admission */
	CLUSTER_NODE_ACTIVE,		/* fully participating */
	CLUSTER_NODE_LEAVING,		/* graceful departure */
	CLUSTER_NODE_SUSPECT,		/* possibly failed, under watch */
	CLUSTER_NODE_FENCED,		/* fenced out, I/O blocked */
	CLUSTER_NODE_DEAD		/* confirmed dead, resources reclaimed */
} cluster_node_state_t;

/*
 * Node role in the cluster.
 */
typedef enum cluster_node_role {
	CLUSTER_ROLE_NONE = 0,
	CLUSTER_ROLE_COORDINATOR,	/* drives TXG, writes MOS/uberblock */
	CLUSTER_ROLE_PARTICIPANT,	/* normal I/O node */
	CLUSTER_ROLE_STANDBY		/* spare, can become coordinator */
} cluster_node_role_t;

/*
 * Node descriptor - represents a cluster member.
 */
typedef struct cluster_node {
	cluster_node_id_t	cn_id;		/* unique node ID */
	cluster_node_state_t	cn_state;	/* current state */
	cluster_node_role_t	cn_role;	/* current role */
	uint64_t		cn_guid;	/* stable GUID (derived from hostid) */
	char			cn_hostname[MAXHOSTNAMELEN];
	uint64_t		cn_incarnation;	/* monotonically increasing */
	hrtime_t		cn_last_heartbeat; /* last heartbeat received */
	hrtime_t		cn_heartbeat_timeout; /* timeout interval (ns) */

	/* Metaslab ownership bitmap: which metaslabs this node owns */
	uint64_t		*cn_metaslab_bm;	/* per-vdev bitmap */
	uint64_t		cn_metaslab_bm_size;	/* bitmap size in uint64_t */

	/* TXG state mirror for this node */
	uint64_t		cn_open_txg;	/* last TXG this node was active in */

	/* Fencing state */
	boolean_t		cn_fenced;	/* I/O blocked */
	uint64_t		cn_fence_token; /* fencing epoch */

	list_node_t		cn_link;	/* membership list link */
	avl_node_t		cn_avl_link;	/* AVL tree link by node ID */
} cluster_node_t;

/* ------------------------------------------------------------------ */
/*  Cluster Configuration                                              */
/* ------------------------------------------------------------------ */

/*
 * On-disk cluster configuration stored in MOS.
 * Object: DMU_POOL_CLUSTER_CONFIG (new pool directory object)
 */
typedef struct cluster_config {
	uint64_t	cc_magic;		/* CLUSTER_CONFIG_MAGIC */
#define	CLUSTER_CONFIG_MAGIC	0x2C5CC0F1ULL
	uint64_t	cc_version;		/* 1 */
	uint64_t	cc_num_nodes;		/* current member count */
	uint64_t	cc_max_nodes;		/* CLUSTER_MAX_NODES */
	uint64_t	cc_epoch;		/* membership epoch */
	uint64_t	cc_coordinator_id;	/* current coordinator node ID */
	uint64_t	cc_flags;		/* feature flags */

	/* Metaslab partitioning policy */
	uint64_t	cc_ms_partition_policy;
#define	CLUSTER_MS_PARTITION_STATIC	0	/* fixed at import */
#define	CLUSTER_MS_PARTITION_DYNAMIC	1	/* rebalance on membership change */
#define	CLUSTER_MS_PARTITION_ADAPTIVE	2	/* I/O-driven rebalancing */

	/* TXG coordination policy */
	uint64_t	cc_txg_policy;
#define	CLUSTER_TXG_CENTRALIZED	0	/* coordinator generates all TXGs */
#define	CLUSTER_TXG_HYBRID		1	/* coordinator delegates ranges */

	/* Synchronization intervals (milliseconds) */
	uint64_t	cc_heartbeat_interval_ms;	/* default: 1000 */
	uint64_t	cc_heartbeat_timeout_ms;	/* default: 5000 */
	uint64_t	cc_txg_sync_interval_ms;	/* default: 5000 */

	/* Fencing policy */
	uint64_t	cc_fence_policy;
#define	CLUSTER_FENCE_PERSISTENT	0	/* disk-backed persistent fence */
#define	CLUSTER_FENCE_MMP		1	/* MMP-based activity check */
} cluster_config_t;

/* ------------------------------------------------------------------ */
/*  Cluster Membership                                                 */
/* ------------------------------------------------------------------ */

/*
 * Membership epoch: incremented on every membership change.
 * All nodes must agree on the current epoch for correctness.
 */
typedef struct cluster_membership {
	kmutex_t		cm_lock;
	cluster_epoch_t		cm_epoch;	/* current epoch */
	cluster_node_id_t	cm_coordinator; /* coordinator node ID */
	uint64_t		cm_num_active;	/* active node count */
	list_t			cm_nodes;	/* list of cluster_node_t */
	avl_tree_t		cm_nodes_by_id;	/* AVL index by node ID */

	/* Quorum tracking */
	boolean_t		cm_has_quorum;	/* majority alive? */
	uint64_t		cm_total_votes;	/* total voting members */
	uint64_t		cm_live_votes;	/* live voting members */

	/* Paxos state for membership decisions */
	cluster_term_t		cm_paxos_term;
	uint64_t		cm_paxos_promised;
	uint64_t		cm_paxos_accepted;
	cluster_node_id_t	cm_paxos_accepted_val;
} cluster_membership_t;

/* ------------------------------------------------------------------ */
/*  Metaslab Partitioning                                              */
/* ------------------------------------------------------------------ */

/*
 * Per-node metaslab ownership record.
 * Stored in MOS as a ZAP object per vdev.
 *
 * Key: metaslab index (uint64_t)
 * Value: cluster_node_id_t (uint64_t)
 *
 * A metaslab can be:
 *   - Owned by a specific node (for writes)
 *   - Unowned/available (new space or released by dead node)
 *   - Shared-read (multiple nodes can read, only owner writes)
 */
typedef struct cluster_ms_assign {
	uint64_t	cma_vdev_id;		/* vdev index */
	uint64_t	cma_ms_count;		/* total metaslabs in vdev */
	cluster_node_id_t	*cma_owner;	/* per-metaslab owner array */
	kmutex_t	cma_lock;		/* protects reassignment */
} cluster_ms_assign_t;

/*
 * Per-vdev metaslab partition state, attached to vdev_t.
 */
typedef struct cluster_vdev_partition {
	uint64_t		cvp_vdev_id;
	cluster_ms_assign_t	*cvp_assign;	/* ownership table */
	uint64_t		cvp_local_start;	/* first local metaslab */
	uint64_t		cvp_local_count;	/* count of local metaslabs */
	uint64_t		cvp_local_cursor;	/* allocation cursor */
	kmutex_t		cvp_lock;
} cluster_vdev_partition_t;

/* ------------------------------------------------------------------ */
/*  Cluster TXG Coordination                                           */
/* ------------------------------------------------------------------ */

/*
 * Distributed TXG state.
 *
 * In centralized mode:
 *   - Coordinator runs the full TXG state machine (open, quiesce, sync)
 *   - Participants receive TXG transitions via messages
 *   - Participants flush dirty data to shared disk at sync time
 *   - Coordinator writes MOS and uberblock
 *
 * In hybrid mode:
 *   - Coordinator delegates TXG ranges to participants
 *   - Each participant can locally assign TXGs within its range
 *   - Coordinator still drives quiesce and sync
 */
typedef struct cluster_txg {
	kmutex_t		ctx_lock;
	cluster_node_id_t	ctx_coordinator;

	/* Global TXG state (coordinator's view) */
	uint64_t		ctx_open_txg;
	uint64_t		ctx_quiescing_txg;
	uint64_t		ctx_syncing_txg;
	uint64_t		ctx_synced_txg;

	/* Per-node dirty accounting */
	uint64_t		ctx_node_dirty[CLUSTER_MAX_NODES];
	uint64_t		ctx_total_dirty;
	uint64_t		ctx_dirty_threshold;	/* kick sync threshold */

	/* Per-node hold counts (for quiesce) */
	uint64_t		ctx_node_holds[CLUSTER_MAX_NODES];

	/* TXG sync barrier: coordinator waits for all nodes */
	kcondvar_t		ctx_quiesce_cv;
	kcondvar_t		ctx_sync_start_cv;
	kcondvar_t		ctx_sync_done_cv;
	boolean_t		ctx_sync_in_progress;

	/* Hybrid mode: per-node TXG ranges */
	uint64_t		ctx_node_txg_base[CLUSTER_MAX_NODES];
	uint64_t		ctx_node_txg_count[CLUSTER_MAX_NODES];
} cluster_txg_t;

/*
 * TXG transition message types.
 */
typedef enum cluster_txg_msg_type {
	CLUSTER_TXG_OPEN = 1,		/* new TXG opened */
	CLUSTER_TXG_QUIESCE,		/* begin quiescing */
	CLUSTER_TXG_SYNC_START,	/* begin syncing */
	CLUSTER_TXG_SYNC_DONE,		/* sync complete */
	CLUSTER_TXG_DIRTY_UPDATE,	/* node reports dirty data */
	CLUSTER_TXG_HOLD_UPDATE,	/* node reports hold count */
	CLUSTER_TXG_BARRIER_ENTER,	/* node entering sync barrier */
	CLUSTER_TXG_BARRIER_LEAVE,	/* node leaving sync barrier */
} cluster_txg_msg_type_t;

/* ------------------------------------------------------------------ */
/*  Cluster Lock Manager (DLM)                                         */
/* ------------------------------------------------------------------ */

/*
 * Lock types for fine-grained concurrency control.
 */
typedef enum cluster_lock_type {
	CLUSTER_LOCK_SHARED = 0,	/* shared/read lock */
	CLUSTER_LOCK_EXCLUSIVE = 1,	/* exclusive/write lock */
} cluster_lock_type_t;

typedef enum cluster_lock_resource {
	CLUSTER_LOCK_DATASET = 1,	/* dsl_dataset/dsl_dir */
	CLUSTER_LOCK_OBJECT = 2,	/* individual dnode/object */
	CLUSTER_LOCK_ZAP = 3,		/* ZAP attribute namespace */
	CLUSTER_LOCK_SPACE = 4,	/* metaslab/space allocation */
	CLUSTER_LOCK_POOL_CONFIG = 5,	/* pool configuration changes */
	CLUSTER_LOCK_SNAPSHOT = 6,	/* snapshot/clone operations */
} cluster_lock_resource_t;

typedef uint64_t	cluster_lock_id_t;

/*
 * A granted lock request.
 */
typedef struct cluster_lock_grant {
	cluster_lock_id_t	clg_id;
	cluster_node_id_t	clg_holder;
	cluster_lock_type_t	clg_type;
	cluster_lock_resource_t	clg_resource;
	uint64_t		clg_objset;	/* objset ID */
	uint64_t		clg_object;	/* object ID */
	hrtime_t		clg_grant_time;
	uint64_t		clg_txg;	/* TXG when granted */
	list_node_t		clg_link;
} cluster_lock_grant_t;

/*
 * A pending lock request.
 */
typedef struct cluster_lock_request {
	cluster_node_id_t	clr_requester;
	cluster_lock_type_t	clr_type;
	cluster_lock_resource_t	clr_resource;
	uint64_t		clr_objset;
	uint64_t		clr_object;
	void			(*clr_callback)(void *);
	void			*clr_callback_arg;
	list_node_t		clr_link;
} cluster_lock_request_t;

/*
 * Per-resource lock state.
 */
typedef struct cluster_lock_entry {
	cluster_lock_id_t	cle_id;
	uint64_t		cle_key;	/* hash key */
	cluster_lock_resource_t	cle_resource;
	uint64_t		cle_objset;
	uint64_t		cle_object;
	list_t			cle_grants;	/* granted locks */
	list_t			cle_pending;	/* waiting requests */
	uint64_t		cle_shared_count;
	boolean_t		cle_exclusive;
	avl_node_t		cle_avl_link;
} cluster_lock_entry_t;

/*
 * Distributed Lock Manager state.
 * Runs on the coordinator; participants forward lock requests.
 */
typedef struct cluster_dlm {
	kmutex_t		dlm_lock;
	avl_tree_t		dlm_locks;	/* all active lock entries */
	uint64_t		dlm_next_id;	/* lock ID counter */
	kmem_cache_t		*dlm_entry_cache;
	kmem_cache_t		*dlm_grant_cache;
	kmem_cache_t		*dlm_request_cache;
} cluster_dlm_t;

/* ------------------------------------------------------------------ */
/*  Cluster ZIL (Intent Log)                                           */
/* ------------------------------------------------------------------ */

/*
 * Per-node ZIL reservation in the shared SLOG.
 * Each node gets dedicated log blocks to avoid write conflicts.
 *
 * The cluster ZIL uses a ring-buffer approach:
 *   - Each node writes to its own region of the SLOG
 *   - On recovery, all nodes' regions are replayed
 *   - Coordinator sequences the replay in TXG order
 */
#define	CLUSTER_ZIL_SLOTS_PER_NODE	4
#define	CLUSTER_ZIL_BLOCK_SIZE		(128 * 1024)  /* 128KB */

typedef struct cluster_zil_slot {
	uint64_t		czs_node_id;
	boolean_t		czs_reserved;	/* slot is reserved for a node */
	boolean_t		czs_in_use;	/* slot is actively being used */
	uint64_t		czs_size;	/* region size for this slot */
	uint64_t		czs_write_offset; /* current write offset in region */
	uint64_t		czs_txg;	/* current TXG being logged */
	uint64_t		czs_seq;	/* sequence number */
	uint64_t		czs_offset;	/* base offset in SLOG */
	uint64_t		czs_blocks;	/* blocks reserved */
	uint64_t		czs_blocks_used;
	uint64_t		czs_lwb_count;	/* outstanding write blocks */
	struct zilog		*czs_zilog;	/* per-node zilog handle */
	kmutex_t		czs_lock;
} cluster_zil_slot_t;

typedef struct cluster_zil {
	kmutex_t		cz_lock;
	struct spa		*cz_spa;	/* back-pointer to spa */
	uint64_t		cz_region_size; /* size of each node's ZIL region */
	uint64_t		cz_slot_count;	/* total number of slots */
	uint64_t		cz_slog_obj;	/* SLOG object in MOS */
	cluster_zil_slot_t	cz_slots[CLUSTER_MAX_NODES];
	uint64_t		cz_total_reserved; /* total reserved blocks */
	uint64_t		cz_total_capacity; /* total SLOG capacity */
} cluster_zil_t;

/* ------------------------------------------------------------------ */
/*  Cluster Fencing                                                    */
/* ------------------------------------------------------------------ */

/*
 * Disk-backed fencing records.
 * Written to reserved areas on every leaf vdev.
 * A fenced node cannot complete I/O because the coordinator
 * will reject its space allocations and TXG participation.
 */
typedef struct cluster_fence_record {
	uint64_t	cfr_magic;
#define	CLUSTER_FENCE_MAGIC	0xFE7CEFE7CEULL
	uint64_t	cfr_epoch;		/* membership epoch */
	cluster_node_id_t	cfr_node_id;
	uint64_t	cfr_fence_time;	/* time when fenced */
	uint64_t	cfr_unfence_time;	/* 0 if still fenced */
	uint8_t		cfr_reason[64];	/* human-readable reason */
} cluster_fence_record_t;

/*
 * Per-vdev fencing state.
 */
typedef struct cluster_vdev_fence {
	uint64_t		cvf_vdev_id;
	uint64_t		cvf_fence_epoch;
	list_t			cvf_fenced_nodes;	/* fenced node records */
	kmutex_t		cvf_lock;
} cluster_vdev_fence_t;

/* ------------------------------------------------------------------ */
/*  Cluster Messaging                                                  */
/* ------------------------------------------------------------------ */

/*
 * Inter-node message types.
 */
typedef enum cluster_msg_type {
	/* Membership */
	CLUSTER_MSG_JOIN_REQUEST = 1,
	CLUSTER_MSG_JOIN_RESPONSE,
	CLUSTER_MSG_LEAVE_REQUEST,
	CLUSTER_MSG_HEARTBEAT,
	CLUSTER_MSG_MEMBERSHIP_CHANGE,

	/* TXG coordination */
	CLUSTER_MSG_TXG_OPEN,
	CLUSTER_MSG_TXG_QUIESCE,
	CLUSTER_MSG_TXG_SYNC_START,
	CLUSTER_MSG_TXG_SYNC_DONE,
	CLUSTER_MSG_TXG_DIRTY_REPORT,
	CLUSTER_MSG_TXG_HOLD_REPORT,

	/* Lock management */
	CLUSTER_MSG_LOCK_REQUEST,
	CLUSTER_MSG_LOCK_GRANT,
	CLUSTER_MSG_LOCK_RELEASE,
	CLUSTER_MSG_LOCK_DENIED,

	/* Metaslab management */
	CLUSTER_MSG_MS_ASSIGN,
	CLUSTER_MSG_MS_RELEASE,
	CLUSTER_MSG_MS_REASSIGN,

	/* ZIL */
	CLUSTER_MSG_ZIL_RESERVE,
	CLUSTER_MSG_ZIL_COMMITTED,

	/* Recovery */
	CLUSTER_MSG_FENCE_REQUEST,
	CLUSTER_MSG_FENCE_ACK,
	CLUSTER_MSG_RECOVERY_START,
	CLUSTER_MSG_RECOVERY_DONE,

	/* Coordinator failover */
	CLUSTER_MSG_COORD_CHALLENGE,
	CLUSTER_MSG_COORD_ACK,
	CLUSTER_MSG_COORD_ANNOUNCE,
} cluster_msg_type_t;

/*
 * Message header for all cluster messages.
 */
typedef struct cluster_msg_header {
	uint64_t		cmh_magic;
#define	CLUSTER_MSG_MAGIC	0x2C5C0D6ULL
	uint64_t		cmh_version;
	cluster_msg_type_t	cmh_type;
	cluster_node_id_t	cmh_src;
	cluster_node_id_t	cmh_dst;	/* CLUSTER_NODE_ID_NONE = broadcast */
	uint64_t		cmh_epoch;
	uint64_t		cmh_seq;	/* message sequence number */
	uint64_t		cmh_timestamp;	/* sender's clock */
	uint32_t		cmh_payload_len;
	uint32_t		cmh_flags;
#define	CLUSTER_MSG_FLAG_URGENT	(1 << 0)
#define	CLUSTER_MSG_FLAG_ACK_REQUIRED	(1 << 1)
} cluster_msg_header_t;

/* ------------------------------------------------------------------ */
/*  Cluster SPA Extension                                              */
/* ------------------------------------------------------------------ */

/*
 * Extended SPA state for clustering.
 * This is attached to spa_t via spa->spa_cluster.
 */
typedef struct cluster_spa {
	/* Back-pointer to parent spa */
	struct spa			*cspa_spa;

	/* Identity */
	cluster_node_id_t		cspa_local_id;	/* this node's ID */
	boolean_t			cspa_is_coordinator;
	cluster_node_role_t		cspa_role;

	/* Configuration */
	cluster_config_t		cspa_config;

	/* Membership */
	cluster_membership_t		cspa_membership;

	/* TXG coordination */
	cluster_txg_t			cspa_txg;

	/* Metaslab partitioning */
	cluster_ms_assign_t		*cspa_ms_assign;	/* per-vdev */
	uint64_t			cspa_ms_assign_count;

	/* Lock manager */
	cluster_dlm_t			cspa_dlm;

	/* Cluster ZIL */
	cluster_zil_t			cspa_zil;

	/* Fencing */
	cluster_vdev_fence_t		*cspa_vdev_fences;
	uint64_t			cspa_vdev_fence_count;

	/* Messaging infrastructure */
	kmutex_t			cspa_msg_lock;
	uint64_t			cspa_msg_seq;
	list_t				cspa_msg_pending;	/* outbound queue */
	list_t				cspa_msg_handlers;	/* registered handlers */

	/* Recovery state */
	boolean_t			cspa_recovering;
	cluster_node_id_t		cspa_recover_target;
	uint64_t			cspa_recover_epoch;
	boolean_t			cspa_shutting_down;

	/* Statistics */
	kstat_t				*cspa_kstat;
} cluster_spa_t;

/* ------------------------------------------------------------------ */
/*  Cluster Statistics                                                 */
/* ------------------------------------------------------------------ */

typedef struct cluster_kstat {
	kstat_named_t	cks_enabled;
	kstat_named_t	cks_node_id;
	kstat_named_t	cks_role;
	kstat_named_t	cks_epoch;
	kstat_named_t	cks_coordinator;
	kstat_named_t	cks_num_active;
	kstat_named_t	cks_has_quorum;
	kstat_named_t	cks_txg_open;
	kstat_named_t	cks_txg_syncing;
	kstat_named_t	cks_txg_synced;
	kstat_named_t	cks_total_dirty;
	kstat_named_t	cks_local_dirty;
	kstat_named_t	cks_local_ms_count;
	kstat_named_t	cks_lock_grants;
	kstat_named_t	cks_lock_pending;
	kstat_named_t	cks_msg_sent;
	kstat_named_t	cks_msg_recv;
} cluster_kstat_t;

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_CLUSTER_CLUSTER_TYPES_H */
