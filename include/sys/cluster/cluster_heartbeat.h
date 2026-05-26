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

#ifndef _SYS_CLUSTER_CLUSTER_HEARTBEAT_H
#define	_SYS_CLUSTER_CLUSTER_HEARTBEAT_H

#include <sys/cluster/cluster_types.h>

struct spa;
struct uberblock;
struct cluster_msg_header;

void cluster_heartbeat_encode_ub(struct spa *spa, struct uberblock *ub);
boolean_t cluster_heartbeat_decode_ub(struct spa *spa, struct uberblock *ub,
    cluster_node_id_t *node_id, uint64_t *epoch, hrtime_t *hb_time);
void cluster_heartbeat_send_all(cluster_spa_t *cspa);
void cluster_heartbeat_receive(struct spa *spa,
    const struct cluster_msg_header *hdr, const void *payload);
void cluster_heartbeat_check(cluster_spa_t *cspa);

#endif	/* _SYS_CLUSTER_CLUSTER_HEARTBEAT_H */
