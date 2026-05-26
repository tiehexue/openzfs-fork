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
 * Cluster ZFS - Inter-Node Messaging Implementation
 *
 * MOS-BACKED QUEUES
 *   Each node has two ZAP objects in the MOS pool directory:
 *     "cluster_msgs_in_N"  — messages addressed TO node N
 *     "cluster_msgs_out_N" — messages sent BY node N (for retry/ack)
 *
 *   A message is stored as:
 *     key   = "msg_<seq>"   (monotonically increasing sequence)
 *     value = <serialised header + payload>
 *
 *   The receiving node reads and removes entries from its inbox.
 *
 * IN-MEMORY FAST-PATH
 *   We iterate the global spa list (spa_next()) to find another
 *   spa_t that has spa_cluster->cspa_local_id matching the
 *   destination.  When found, we deliver directly into its handler
 *   list under cspa_msg_lock.
 */

#include <sys/cluster/cluster_types.h>
#include <sys/cluster/cluster_messaging.h>
#include <sys/cluster/cluster_spa.h>
#include <sys/spa_impl.h>
#include <sys/dsl_pool.h>
#include <sys/dmu_tx.h>
#include <sys/zap.h>
#include <sys/zfs_context.h>

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                    */
/* ------------------------------------------------------------------ */

#define	CLUSTER_MSG_INBOX_PREFIX	"cluster_msgs_in_"
#define	CLUSTER_MSG_OUTBOX_PREFIX	"cluster_msgs_out_"
#define	CLUSTER_MSG_MAX_RETRIES		5

/*
 * Build the MOS object name for a node's inbox or outbox.
 */
static void
cluster_msg_mos_name(char *buf, size_t bufsz, const char *prefix,
    cluster_node_id_t node_id)
{
	(void) snprintf(buf, bufsz, "%s%u", prefix, (unsigned int)node_id);
}

/*
 * Find (or create) the ZAP object for a node's message queue.
 */
static int
cluster_msg_get_queue(spa_t *spa, cluster_node_id_t node_id,
    const char *prefix, uint64_t *obj, dmu_tx_t *tx)
{
	dsl_pool_t *dp = spa->spa_dsl_pool;
	objset_t *mos = dp->dp_meta_objset;
	char name[64];
	int error;

	cluster_msg_mos_name(name, sizeof (name), prefix, node_id);

	error = zap_lookup(mos, DMU_POOL_DIRECTORY_OBJECT, name,
	    sizeof (uint64_t), 1, obj);
	if (error == ENOENT && tx != NULL) {
		*obj = zap_create(mos, DMU_OTN_ZAP_METADATA,
		    DMU_OT_NONE, 0, tx);
		error = zap_add(mos, DMU_POOL_DIRECTORY_OBJECT, name,
		    sizeof (uint64_t), 1, obj, tx);
	}
	return (error);
}

/*
 * Find a loaded spa_t for a specific cluster node (same-kernel fast-path).
 * Returns NULL if the destination is not loaded locally.
 */
static cluster_spa_t *
cluster_msg_find_destination(spa_t *src_spa, cluster_node_id_t node_id)
{
	/*
	 * In the single-machine simulation, each "node" is a
	 * sequential import/export, so there is never more than
	 * one spa_t loaded at a time.  Fast-path delivery is a
	 * no-op here.  In a real multi-node cluster, this would
	 * use a network socket or RDMA queue pair.
	 */
	(void)src_spa;
	(void)node_id;
	return (NULL);
}

/*
 * Serialise a message header and payload into a contiguous buffer.
 * Caller must kmem_free the returned buffer.
 */
static uint8_t *
cluster_msg_serialise(const cluster_msg_header_t *hdr, const void *payload,
    uint32_t payload_len, uint32_t *total_len)
{
	uint32_t hdr_len = sizeof (cluster_msg_header_t);

	*total_len = hdr_len + payload_len;
	uint8_t *buf = kmem_alloc(*total_len, KM_SLEEP);

	memcpy(buf, hdr, hdr_len);
	if (payload != NULL && payload_len > 0)
		memcpy(buf + hdr_len, payload, payload_len);

	return (buf);
}

/*
 * Deserialise a buffer into header + payload.
 */
static int
cluster_msg_deserialise(const uint8_t *buf, uint32_t buf_len,
    cluster_msg_header_t *hdr, void *payload, uint32_t *payload_len)
{
	uint32_t hdr_len = sizeof (cluster_msg_header_t);

	if (buf_len < hdr_len)
		return (EINVAL);

	memcpy(hdr, buf, hdr_len);

	if (payload != NULL && hdr->cmh_payload_len > 0) {
		uint32_t p_len = MIN(hdr->cmh_payload_len,
		    buf_len - hdr_len);
		memcpy(payload, buf + hdr_len, p_len);
		if (payload_len != NULL)
			*payload_len = p_len;
	}

	return (0);
}

/*
 * Deliver a message directly to a local destination's handler.
 */
static void
cluster_msg_deliver_local(cluster_spa_t *dst_cspa,
    const cluster_msg_header_t *hdr, const void *payload)
{
	/* Find and invoke the registered handler */
	mutex_enter(&dst_cspa->cspa_msg_lock);

	/*
	 * Walk the handler list.  For now we use a simple linear
	 * search because the list is tiny (< 20 entries).
	 */
	/* Handler storage: we embed the handler function pointer in a
	 * simple array-like pattern.  The list cspa_msg_handlers stores
	 * cluster_msg_handler_entry_t items. */
	typedef struct {
		list_node_t		link;
		cluster_msg_type_t	type;
		cluster_msg_handler_fn	fn;
	} handler_entry_t;

	handler_entry_t *he;
	for (he = list_head(&dst_cspa->cspa_msg_handlers); he != NULL;
	    he = list_next(&dst_cspa->cspa_msg_handlers, he)) {
		if (he->type == hdr->cmh_type && he->fn != NULL) {
			mutex_exit(&dst_cspa->cspa_msg_lock);
			he->fn(dst_cspa->cspa_spa, hdr, payload);
			return;
		}
	}

	mutex_exit(&dst_cspa->cspa_msg_lock);
}

/*
 * Store a message in the destination's MOS inbox.
 */
static int
cluster_msg_store_mos(spa_t *spa, cluster_node_id_t dst,
    const cluster_msg_header_t *hdr, const void *payload)
{
	dsl_pool_t *dp = spa->spa_dsl_pool;
	objset_t *mos = dp->dp_meta_objset;
	dmu_tx_t *tx;
	uint64_t obj;
	uint32_t total_len;
	uint8_t *buf;
	char key[32];
	int error;

	buf = cluster_msg_serialise(hdr, payload, hdr->cmh_payload_len,
	    &total_len);

	tx = dmu_tx_create_dd(dp->dp_mos_dir);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error != 0) {
		dmu_tx_abort(tx);
		kmem_free(buf, total_len);
		return (error);
	}

	error = cluster_msg_get_queue(spa, dst, CLUSTER_MSG_INBOX_PREFIX,
	    &obj, tx);
	if (error != 0) {
		dmu_tx_abort(tx);
		kmem_free(buf, total_len);
		return (error);
	}

	(void) snprintf(key, sizeof (key), "msg_%llu",
	    (u_longlong_t)hdr->cmh_seq);
	error = zap_update(mos, obj, key, sizeof (uint64_t), 1,
	    &hdr->cmh_type, tx);
	if (error == 0) {
		/* Store the full serialised message as a separate entry */
		char datakey[40];
		(void) snprintf(datakey, sizeof (datakey), "data_%llu",
		    (u_longlong_t)hdr->cmh_seq);
		error = zap_update(mos, obj, datakey, 1,
		    total_len, buf, tx);
	}
	dmu_tx_commit(tx);
	kmem_free(buf, total_len);
	return (error);
}

/* ------------------------------------------------------------------ */
/*  Initialization / Cleanup                                           */
/* ------------------------------------------------------------------ */

int
cluster_messaging_init(cluster_spa_t *cspa)
{
	/* cspa_msg_lock and cspa_msg_handlers are initialised in
	 * cluster_spa_init().  This function is a no‑op for now. */
	(void)cspa;
	return (0);
}

void
cluster_messaging_fini(cluster_spa_t *cspa)
{
	/* Free any pending messages and handler entries */
	mutex_enter(&cspa->cspa_msg_lock);

	typedef struct {
		list_node_t		link;
		cluster_msg_type_t	type;
		cluster_msg_handler_fn	fn;
	} handler_entry_t;

	handler_entry_t *he;
	while ((he = list_remove_head(&cspa->cspa_msg_handlers)) != NULL)
		kmem_free(he, sizeof (*he));

	mutex_exit(&cspa->cspa_msg_lock);
}

/* ------------------------------------------------------------------ */
/*  Sending                                                            */
/* ------------------------------------------------------------------ */

int
cluster_msg_send(cluster_spa_t *cspa, cluster_msg_type_t type,
    cluster_node_id_t dst, const void *payload, uint32_t payload_len,
    boolean_t urgent)
{
	spa_t *spa = cspa->cspa_spa;
	cluster_msg_header_t hdr;
	int error = 0;

	/* Build header */
	memset(&hdr, 0, sizeof (hdr));
	hdr.cmh_magic = CLUSTER_MSG_MAGIC;
	hdr.cmh_version = 1;
	hdr.cmh_type = type;
	hdr.cmh_src = cspa->cspa_local_id;
	hdr.cmh_dst = dst;
	hdr.cmh_epoch = cspa->cspa_membership.cm_epoch;
	hdr.cmh_seq = atomic_inc_64_nv(&cspa->cspa_msg_seq);
	hdr.cmh_timestamp = gethrtime();
	hdr.cmh_payload_len = payload_len;
	if (urgent)
		hdr.cmh_flags |= CLUSTER_MSG_FLAG_URGENT;

	/*
	 * Fast-path: try in-memory delivery to another spa_t in the
	 * same kernel.
	 */
	if (dst != CLUSTER_NODE_ID_NONE) {
		cluster_spa_t *dst_cspa =
		    cluster_msg_find_destination(spa, dst);
		if (dst_cspa != NULL) {
			cluster_msg_deliver_local(dst_cspa, &hdr, payload);
			return (0);
		}
	}

	/*
	 * If dst is CLUSTER_NODE_ID_NONE (broadcast), deliver to all
	 * locally-loaded cluster spa_t instances via MOS.
	 * In single-machine simulation, there are no other locally
	 * loaded spa_t instances, so this is a no-op for in-memory.
	 */
	if (dst == CLUSTER_NODE_ID_NONE) {
		/*
		 * Broadcast: messages are stored per-node in MOS.
		 * Handled below in the MOS slow-path.
		 */
	}

	/*
	 * Slow-path: store in MOS for the destination to poll.
	 * Only the coordinator can write the MOS, so participants
	 * must forward messages through the coordinator.
	 */
	if (cspa->cspa_is_coordinator ||
	    dst == CLUSTER_NODE_ID_COORDINATOR) {
		cluster_node_id_t store_dst = (dst == CLUSTER_NODE_ID_NONE)
		    ? CLUSTER_NODE_ID_COORDINATOR : dst;

		/*
		 * For broadcast: store in each active node's inbox.
		 * For point-to-point: store in the destination's inbox.
		 */
		if (dst == CLUSTER_NODE_ID_NONE) {
			cluster_membership_t *cm = &cspa->cspa_membership;
			cluster_node_t *cn;
			mutex_enter(&cm->cm_lock);
			for (cn = list_head(&cm->cm_nodes); cn != NULL;
			    cn = list_next(&cm->cm_nodes, cn)) {
				if (cn->cn_id == cspa->cspa_local_id)
					continue;
				if (cn->cn_state != CLUSTER_NODE_ACTIVE &&
				    cn->cn_state != CLUSTER_NODE_JOINING)
					continue;
				error = cluster_msg_store_mos(spa, cn->cn_id,
				    &hdr, payload);
			}
			mutex_exit(&cm->cm_lock);
		} else {
			error = cluster_msg_store_mos(spa, store_dst,
			    &hdr, payload);
		}
	}

	return (error);
}

int
cluster_msg_send_sync(cluster_spa_t *cspa, cluster_msg_type_t type,
    cluster_node_id_t dst, const void *payload, uint32_t payload_len,
    boolean_t urgent)
{
	/*
	 * In the single-machine simulation, send_sync is equivalent to
	 * send with the fast-path.  In a real cluster, this would wait
	 * for a network ACK.
	 */
	return (cluster_msg_send(cspa, type, dst, payload, payload_len,
	    urgent));
}

/* ------------------------------------------------------------------ */
/*  Polling: read messages from MOS inbox                              */
/* ------------------------------------------------------------------ */

void
cluster_msg_poll(cluster_spa_t *cspa)
{
	spa_t *spa = cspa->cspa_spa;
	dsl_pool_t *dp = spa->spa_dsl_pool;
	objset_t *mos = dp->dp_meta_objset;
	uint64_t obj;
	int error;

	if (dp == NULL || mos == NULL)
		return;

	error = cluster_msg_get_queue(spa, cspa->cspa_local_id,
	    CLUSTER_MSG_INBOX_PREFIX, &obj, NULL);
	if (error != 0)
		return;	/* no inbox yet */

	/*
	 * Read all messages from the inbox.
	 * We use zap_cursor to iterate.
	 */
	zap_cursor_t zc;
	zap_attribute_t za;

	for (zap_cursor_init(&zc, mos, obj);
	    zap_cursor_retrieve(&zc, &za) == 0;
	    zap_cursor_advance(&zc)) {

		/* Only process "data_*" entries (skip the type-indicator) */
		if (strncmp(za.za_name, "data_", 5) != 0)
			continue;

		/* Read the serialised message */
		uint32_t buf_len = za.za_num_integers;
		if (buf_len < sizeof (cluster_msg_header_t))
			continue;

		uint8_t *buf = kmem_alloc(buf_len, KM_SLEEP);
		error = zap_lookup(mos, obj, za.za_name, 1, buf_len, buf);
		if (error != 0) {
			kmem_free(buf, buf_len);
			continue;
		}

		cluster_msg_header_t hdr;
		error = cluster_msg_deserialise(buf, buf_len, &hdr,
		    NULL, NULL);
		if (error == 0 && hdr.cmh_magic == CLUSTER_MSG_MAGIC) {
			/* Deliver locally */
			cluster_msg_deliver_local(cspa, &hdr,
			    (buf_len > sizeof (hdr)) ?
			    buf + sizeof (hdr) : NULL);
		}

		kmem_free(buf, buf_len);

		/*
		 * Remove the processed message from MOS.
		 * We do a quick DMU tx for cleanup.
		 */
		dmu_tx_t *tx = dmu_tx_create_dd(dp->dp_mos_dir);
		if (dmu_tx_assign(tx, DMU_TX_WAIT) == 0) {
			(void) zap_remove(mos, obj, za.za_name, tx);
			dmu_tx_commit(tx);
		} else {
			dmu_tx_abort(tx);
		}
	}
	zap_cursor_fini(&zc);
}

/* ------------------------------------------------------------------ */
/*  Handler registration                                               */
/* ------------------------------------------------------------------ */

int
cluster_msg_register_handler(cluster_spa_t *cspa,
    cluster_msg_type_t type, cluster_msg_handler_fn fn)
{
	typedef struct {
		list_node_t		link;
		cluster_msg_type_t	type;
		cluster_msg_handler_fn	fn;
	} handler_entry_t;

	handler_entry_t *he = kmem_alloc(sizeof (*he), KM_SLEEP);
	he->type = type;
	he->fn = fn;

	mutex_enter(&cspa->cspa_msg_lock);
	list_insert_tail(&cspa->cspa_msg_handlers, he);
	mutex_exit(&cspa->cspa_msg_lock);

	return (0);
}

/* ------------------------------------------------------------------ */
/*  Convenience: TXG broadcasts                                        */
/* ------------------------------------------------------------------ */

typedef struct cluster_txg_msg_payload {
	uint64_t txg;
} cluster_txg_msg_payload_t;

void
cluster_msg_broadcast_txg_open(cluster_spa_t *cspa, uint64_t txg)
{
	cluster_txg_msg_payload_t p = { .txg = txg };
	(void) cluster_msg_send(cspa, CLUSTER_MSG_TXG_OPEN,
	    CLUSTER_NODE_ID_NONE, &p, sizeof (p), B_FALSE);
}

void
cluster_msg_broadcast_txg_quiesce(cluster_spa_t *cspa, uint64_t txg)
{
	cluster_txg_msg_payload_t p = { .txg = txg };
	(void) cluster_msg_send(cspa, CLUSTER_MSG_TXG_QUIESCE,
	    CLUSTER_NODE_ID_NONE, &p, sizeof (p), B_TRUE);
}

void
cluster_msg_broadcast_txg_sync_start(cluster_spa_t *cspa, uint64_t txg)
{
	cluster_txg_msg_payload_t p = { .txg = txg };
	(void) cluster_msg_send(cspa, CLUSTER_MSG_TXG_SYNC_START,
	    CLUSTER_NODE_ID_NONE, &p, sizeof (p), B_TRUE);
}

void
cluster_msg_broadcast_txg_sync_done(cluster_spa_t *cspa, uint64_t txg)
{
	cluster_txg_msg_payload_t p = { .txg = txg };
	(void) cluster_msg_send(cspa, CLUSTER_MSG_TXG_SYNC_DONE,
	    CLUSTER_NODE_ID_NONE, &p, sizeof (p), B_FALSE);
}

/* ------------------------------------------------------------------ */
/*  Convenience: membership messages                                   */
/* ------------------------------------------------------------------ */

void
cluster_msg_send_heartbeat(cluster_spa_t *cspa, cluster_node_id_t dst)
{
	uint64_t payload = cspa->cspa_local_id;
	(void) cluster_msg_send(cspa, CLUSTER_MSG_HEARTBEAT,
	    dst, &payload, sizeof (payload), B_FALSE);
}

void
cluster_msg_send_join_request(cluster_spa_t *cspa)
{
	uint64_t payload = cspa->cspa_local_id;
	(void) cluster_msg_send(cspa, CLUSTER_MSG_JOIN_REQUEST,
	    CLUSTER_NODE_ID_COORDINATOR, &payload, sizeof (payload),
	    B_TRUE);
}

void
cluster_msg_send_leave_notice(cluster_spa_t *cspa)
{
	uint64_t payload = cspa->cspa_local_id;
	(void) cluster_msg_send(cspa, CLUSTER_MSG_LEAVE_REQUEST,
	    CLUSTER_NODE_ID_COORDINATOR, &payload, sizeof (payload),
	    B_FALSE);
}
