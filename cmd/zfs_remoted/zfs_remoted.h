/*
 * CDDL HEADER START ... (see LICENSE)
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2026, OpenZFS Remote VDEV contributors.
 *
 * Shared header for zfs_remoted: RPC protocol and block backend interface.
 */

#ifndef ZFS_REMOTED_H
#define ZFS_REMOTED_H

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * RPC Protocol -- must match sys/vdev_remote.h exactly
 * ================================================================ */
#pragma pack(push, 1)
typedef struct rpc_hdr {
	uint32_t	cmd;
	uint32_t	status;
	uint64_t	offset;
	uint32_t	size;
	uint32_t	reserved;
} rpc_hdr_t;
#pragma pack(pop)

#define CMD_READ  0x52454144  /* "READ" */
#define CMD_WRITE 0x57524954  /* "WRIT" */
#define CMD_FLUSH 0x464C5553  /* "FLUS" */
#define CMD_TRIM  0x5452494D  /* "TRIM" */
#define CMD_INFO  0x494E464F  /* "INFO" */

#define STATUS_OK       0
#define STATUS_ERR_IO   1
#define STATUS_ERR_INVAL 2
#define STATUS_ERR_NOSPC 3

/* ================================================================
 * Block device backend abstraction
 * ================================================================ */
typedef struct block_backend {
	const char *bb_label;

	int     (*bb_open)(struct block_backend *bb, const char *spec);
	void    (*bb_close)(struct block_backend *bb);
	int     (*bb_read)(struct block_backend *bb,
	            void *buf, uint32_t size, uint64_t offset);
	int     (*bb_write)(struct block_backend *bb,
	            const void *buf, uint32_t size, uint64_t offset);
	int     (*bb_flush)(struct block_backend *bb);

	/* read-only after open */
	uint64_t bb_dev_size;
	uint32_t bb_lbasize;
	uint32_t bb_pbasize;

	/* opaque private data */
	void    *bb_priv;
} block_backend_t;

/* Pre-built templates (caller copies them and calls bb_open) */
extern const block_backend_t file_backend;
extern const block_backend_t disk_backend;

/* Network helpers */
int recv_all(SOCKET s, void *buf, int len);
int send_all(SOCKET s, const void *buf, int len);

#ifdef __cplusplus
}
#endif

#endif /* ZFS_REMOTED_H */
