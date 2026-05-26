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

#ifndef _SYS_CLUSTER_CLUSTER_SPA_H
#define	_SYS_CLUSTER_CLUSTER_SPA_H

#include <sys/cluster/cluster_types.h>

struct spa;

int cluster_spa_init(struct spa *spa, cluster_node_id_t node_id,
    boolean_t is_coordinator);
void cluster_spa_fini(struct spa *spa);
int cluster_spa_import(struct spa *spa, cluster_node_id_t node_id);
int cluster_spa_export(struct spa *spa);
int cluster_spa_config_write(struct spa *spa);
int cluster_spa_config_read(struct spa *spa);
boolean_t cluster_spa_enabled(struct spa *spa);
boolean_t cluster_spa_is_coordinator(struct spa *spa);
void cluster_spa_sync_enter(struct spa *spa, uint64_t txg);
void cluster_spa_sync_exit(struct spa *spa, uint64_t txg);
int cluster_spa_prop_set(struct spa *spa, nvlist_t *props);

#endif	/* _SYS_CLUSTER_CLUSTER_SPA_H */
