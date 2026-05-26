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
 * Cluster ZFS - Inter-Node Messaging Layer
 *
 * This module implements the communication substrate between cluster
 * nodes.  Because there is no real network stack in the ZFS kernel
 * module, we use two complementary mechanisms:
 *
 *   MOS-BACKED MESSAGE QUEUES (primary)
 *     Each node reads/writes messages in reserved ZAP objects in the
 *     MOS.  This is the ground truth: messages are persistent and
 *     survive node reboots.  It works because the coordinator is the
 *     sole MOS writer — it serialises all MOS updates.
 *
 *   IN-MEMORY DELIVERY (fast‑path, same‑host fallback)
 *     When the destination node's spa_t is loaded in the same kernel
 *     (the common case for the export/import simulation), we deliver
 *     messages directly via the registered handler list, avoiding
 *     MOS round‑trips.
 *
 * MESSAGE FORMAT
 *   Every message has a fixed 64‑byte header (cluster_msg_header_t)
 *   followed by an optional type‑specific payload.
 *
 * RELIABILITY
 *   Messages are acknowledged.  Unacknowledged messages are
 *   re‑delivered on the next heartbeat cycle.  After
 *   CLUSTER_MSG_MAX_RETRIES the destination is marked SUSPECT.
 */

#ifndef _SYS_CLUSTER_CLUSTER_MESSAGING_H
#define	_SYS_CLUSTER_CLUSTER_MESSAGING_H

#include <sys/cluster/cluster_types.h>

struct spa;

/* ------------------------------------------------------------------ */
/*  Message handler callback type                                      */
/* ------------------------------------------------------------------ */

typedef void (*cluster_msg_handler_fn)(struct spa *spa,
    const cluster_msg_header_t *hdr, const void *payload);

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                          */
/* ------------------------------------------------------------------ */

int  cluster_messaging_init(cluster_spa_t *cspa);
void cluster_messaging_fini(cluster_spa_t *cspa);

/* ------------------------------------------------------------------ */
/*  Sending                                                            */
/* ------------------------------------------------------------------ */

/*
 * Send a message to a specific node or broadcast (dst = CLUSTER_NODE_ID_NONE).
 * Returns 0 on success, or an errno.
 *
 * If the destination spa is loaded in the local kernel, the message
 * is delivered synchronously via the handler list.  Otherwise it is
 * queued in the MOS for the destination to pick up on its next
 * heartbeat / poll cycle.
 */
int cluster_msg_send(cluster_spa_t *cspa, cluster_msg_type_t type,
    cluster_node_id_t dst, const void *payload, uint32_t payload_len,
    boolean_t urgent);

/*
 * Send a message and wait for acknowledgement (synchronous).
 * Only works when the destination is loaded in the same kernel.
 */
int cluster_msg_send_sync(cluster_spa_t *cspa, cluster_msg_type_t type,
    cluster_node_id_t dst, const void *payload, uint32_t payload_len,
    boolean_t urgent);

/* ------------------------------------------------------------------ */
/*  Receiving / Polling                                                */
/* ------------------------------------------------------------------ */

/*
 * Poll the MOS for any messages addressed to this node.
 * Delivers each message to the registered handler.
 * Called from the heartbeat / sync thread.
 */
void cluster_msg_poll(cluster_spa_t *cspa);

/*
 * Register a handler for a specific message type.
 */
int cluster_msg_register_handler(cluster_spa_t *cspa,
    cluster_msg_type_t type, cluster_msg_handler_fn fn);

/* ------------------------------------------------------------------ */
/*  Convenience: broadcast TXG transitions                             */
/* ------------------------------------------------------------------ */

void cluster_msg_broadcast_txg_open(cluster_spa_t *cspa, uint64_t txg);
void cluster_msg_broadcast_txg_quiesce(cluster_spa_t *cspa, uint64_t txg);
void cluster_msg_broadcast_txg_sync_start(cluster_spa_t *cspa, uint64_t txg);
void cluster_msg_broadcast_txg_sync_done(cluster_spa_t *cspa, uint64_t txg);

/* ------------------------------------------------------------------ */
/*  Convenience: membership messages                                   */
/* ------------------------------------------------------------------ */

void cluster_msg_send_heartbeat(cluster_spa_t *cspa, cluster_node_id_t dst);
void cluster_msg_send_join_request(cluster_spa_t *cspa);
void cluster_msg_send_leave_notice(cluster_spa_t *cspa);

#endif	/* _SYS_CLUSTER_CLUSTER_MESSAGING_H */
