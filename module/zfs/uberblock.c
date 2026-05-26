// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2013, 2017 by Delphix. All rights reserved.
 */

#include <sys/zfs_context.h>
#include <sys/uberblock_impl.h>
#include <sys/vdev_impl.h>
#include <sys/mmp.h>
#include <sys/spa_impl.h>
#include <sys/cluster/cluster_spa.h>

int
uberblock_verify(uberblock_t *ub)
{
	if (ub->ub_magic == BSWAP_64((uint64_t)UBERBLOCK_MAGIC))
		byteswap_uint64_array(ub, sizeof (uberblock_t));

	if (ub->ub_magic != UBERBLOCK_MAGIC)
		return (SET_ERROR(EINVAL));

	return (0);
}

/*
 * Update the uberblock and return TRUE if anything changed in this
 * transaction group.
 */
boolean_t
uberblock_update(uberblock_t *ub, vdev_t *rvd, uint64_t txg, uint64_t mmp_delay)
{
	spa_t *spa = rvd->vdev_spa;

	ASSERT(ub->ub_txg < txg);

	/*
	 * We explicitly do not set ub_version here, so that older versions
	 * continue to be written with the previous uberblock version.
	 */
	ub->ub_magic = UBERBLOCK_MAGIC;
	ub->ub_txg = txg;
	ub->ub_guid_sum = rvd->vdev_guid_sum;
	ub->ub_timestamp = gethrestime_sec();
	ub->ub_software_version = SPA_VERSION;
	ub->ub_mmp_magic = MMP_MAGIC;
	if (spa_multihost(spa)) {
		ub->ub_mmp_delay = mmp_delay;
		ub->ub_mmp_config = MMP_SEQ_SET(0) |
		    MMP_INTERVAL_SET(zfs_multihost_interval) |
		    MMP_FAIL_INT_SET(zfs_multihost_fail_intervals);
	} else {
		ub->ub_mmp_delay = 0;
		ub->ub_mmp_config = 0;
	}

	/*
	 * Cluster: Encode cluster information into the uberblock.
	 * We repurpose upper bits of ub_mmp_config and ub_mmp_delay:
	 *   - ub_mmp_config bits 48-63: coordinator node ID
	 *   - ub_mmp_delay: cluster epoch (when cluster is active)
	 * This allows any node reading the uberblock to identify
	 * which coordinator wrote it and the cluster epoch.
	 */
	if (spa->spa_cluster != NULL && cluster_spa_is_coordinator(spa)) {
		cluster_spa_t *cspa = spa->spa_cluster;
		/*
		 * Encode coordinator node ID + 1 in bits 48-63 so
		 * node 0 is non-zero (0 means "no cluster").
		 */
		ub->ub_mmp_config |=
		    ((uint64_t)(cspa->cspa_local_id + 1) << 48);
		ub->ub_mmp_delay =
		    cspa->cspa_membership.cm_epoch;
	}

	ub->ub_checkpoint_txg = 0;

	return (BP_GET_LOGICAL_BIRTH(&ub->ub_rootbp) == txg);
}
