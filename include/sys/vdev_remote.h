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
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2026, OpenZFS Remote VDEV contributors.
 */

#ifndef _SYS_VDEV_REMOTE_H
#define _SYS_VDEV_REMOTE_H

#include <sys/vdev.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Remote VDEV RPC protocol definitions.
 *
 * A remote VDEV communicates over TCP with a daemon that serves a raw
 * image file as a block device. The protocol is a simple binary RPC
 * with fixed-size header followed by optional data payload.
 */

/*
 * Maximum host:port string length for remote connections.
 */
#define VDEV_REMOTE_HOST_MAX		256

/*
 * RPC header sent for every command.
 */
typedef struct vdev_remote_rpc_hdr {
	uint32_t	vr_cmd;		/* command opcode */
	uint32_t	vr_status;	/* response status (0 = success) */
	uint64_t	vr_offset;	/* byte offset in the remote device */
	uint32_t	vr_size;	/* size of optional data payload */
	uint32_t	vr_reserved;	/* padding */
} vdev_remote_rpc_hdr_t;

/*
 * RPC command opcodes.
 */
#define VDEV_REMOTE_CMD_READ		0x52454144	/* "READ" */
#define VDEV_REMOTE_CMD_WRITE		0x57524954	/* "WRIT" */
#define VDEV_REMOTE_CMD_FLUSH		0x464C5553	/* "FLUS" */
#define VDEV_REMOTE_CMD_TRIM		0x5452494D	/* "TRIM" */
#define VDEV_REMOTE_CMD_INFO		0x494E464F	/* "INFO" */

/*
 * RPC status codes.
 */
#define VDEV_REMOTE_STATUS_OK		0
#define VDEV_REMOTE_STATUS_ERR_IO	1
#define VDEV_REMOTE_STATUS_ERR_INVAL	2
#define VDEV_REMOTE_STATUS_ERR_NOSPC	3

/*
 * Per-instance state for a remote VDEV.
 */
typedef struct vdev_remote {
	char			vr_host[VDEV_REMOTE_HOST_MAX];
	uint16_t		vr_port;
	void			*vr_os_priv;	/* OS-specific state */
	uint64_t		vr_device_size;
	uint32_t		vr_block_size;
	uint32_t		vr_phys_block_size;
	kmutex_t		vr_lock;
	boolean_t		vr_connected;
} vdev_remote_t;

/*
 * Core remote vdev module lifecycle.
 */
extern void vdev_remote_init(void);
extern void vdev_remote_fini(void);

/*
 * OS-specific functions that must be implemented by each platform.
 *
 * These are declared here so the core vdev_remote.c can call them,
 * and each OS layer (e.g. module/os/windows/zfs/vdev_remote_os.c)
 * provides the implementation.
 */
int vdev_remote_os_connect(vdev_remote_t *vr);
void vdev_remote_os_disconnect(vdev_remote_t *vr);
int vdev_remote_os_io(vdev_remote_t *vr, uint32_t cmd,
    uint64_t offset, void *data, uint32_t size);
int vdev_remote_os_info(vdev_remote_t *vr,
    uint64_t *size, uint32_t *blksz, uint32_t *pblksz);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_VDEV_REMOTE_H */
