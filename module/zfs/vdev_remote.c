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
 *
 * Virtual device vector for remote block devices accessed via TCP/RPC.
 *
 * A remote VDEV connects to a TCP daemon (zfs_remoted) that serves a raw
 * image file as a block device. All I/O operations are marshalled over
 * the RPC protocol to the remote daemon.
 *
 * NOTE: Currently only supported on Windows.
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/vdev_remote.h>
#include <sys/vdev_impl.h>
#include <sys/zio.h>
#include <sys/fs/zfs.h>
#include <sys/fm/fs/zfs.h>
#include <sys/abd.h>

#if defined(_WIN32) && defined(_KERNEL)
/*
 * On Windows kernel, the OS-specific functions are implemented in
 * module/os/windows/zfs/vdev_remote_os.c using Winsock Kernel (WSK).
 */
#else
/*
 * Stubs for non-kernel or non-Windows platforms.
 * In user-space (libzpool), remote VDEV TCP is handled by the
 * kernel driver; these stubs satisfy the linker.
 */
int
vdev_remote_os_connect(vdev_remote_t *vr)
{
	(void) vr;
	return (SET_ERROR(ENOTSUP));
}

void
vdev_remote_os_disconnect(vdev_remote_t *vr)
{
	(void) vr;
}

int
vdev_remote_os_io(vdev_remote_t *vr, uint32_t cmd,
    uint64_t offset, void *data, uint32_t size)
{
	(void) vr;
	(void) cmd;
	(void) offset;
	(void) data;
	(void) size;
	return (SET_ERROR(ENOTSUP));
}

int
vdev_remote_os_info(vdev_remote_t *vr,
    uint64_t *size, uint32_t *blksz, uint32_t *pblksz)
{
	(void) vr;
	(void) size;
	(void) blksz;
	(void) pblksz;
	return (SET_ERROR(ENOTSUP));
}
#endif /* _WIN32 && _KERNEL */

/*
 * Parse a "remote://host:port" URI into host and port components.
 * Returns 0 on success, non-zero on parse error.
 */
static int
vdev_remote_parse_uri(const char *path, char *host, size_t hostlen,
    uint16_t *port)
{
	const char *p;
	size_t len;

	if (path == NULL)
		return (SET_ERROR(EINVAL));

	/* Must start with "remote://" */
	if (strncmp(path, "remote://", 9) != 0)
		return (SET_ERROR(EINVAL));

	p = path + 9;

	/* Find the colon separating host:port */
	const char *colon = strrchr(p, ':');
	if (colon == NULL || colon == p)
		return (SET_ERROR(EINVAL));

	/* Extract host */
	len = (size_t)(colon - p);
	if (len >= hostlen)
		return (SET_ERROR(EINVAL));

	memcpy(host, p, len);
	host[len] = '\0';

	/* Parse port */
	unsigned long port_val = 0;
	const char *port_str = colon + 1;
	while (*port_str >= '0' && *port_str <= '9') {
		port_val = port_val * 10 + (unsigned long)(*port_str - '0');
		port_str++;
	}
	if (port_val == 0 || port_val > 65535)
		return (SET_ERROR(EINVAL));

	*port = (uint16_t)port_val;
	return (0);
}

/*
 * Task queue for asynchronous I/O dispatch.
 * Remote I/O is dispatched to this taskq to avoid blocking the ZIO pipeline
 * on network latency.
 */
static taskq_t *vdev_remote_taskq;

void
vdev_remote_init(void)
{
	vdev_remote_taskq = taskq_create("z_vdev_remote", MAX(boot_ncpus, 16),
	    minclsyspri, boot_ncpus, INT_MAX, TASKQ_DYNAMIC);
	VERIFY(vdev_remote_taskq);
}

void
vdev_remote_fini(void)
{
	if (vdev_remote_taskq != NULL) {
		taskq_destroy(vdev_remote_taskq);
		vdev_remote_taskq = NULL;
	}
}

static void
vdev_remote_hold(vdev_t *vd)
{
	ASSERT3P(vd->vdev_path, !=, NULL);
}

static void
vdev_remote_rele(vdev_t *vd)
{
	ASSERT3P(vd->vdev_path, !=, NULL);
}

static int
vdev_remote_open(vdev_t *vd, uint64_t *psize, uint64_t *max_psize,
    uint64_t *logical_ashift, uint64_t *physical_ashift)
{
	vdev_remote_t *vr;
	int error;

	/*
	 * Remote devices are always non-rotational.
	 */
	vd->vdev_nonrot = B_TRUE;

	/*
	 * TRIM is supported via the remote protocol.
	 */
	vd->vdev_has_trim = B_TRUE;
	vd->vdev_has_securetrim = B_FALSE;

	/* We must have a pathname starting with "remote://" */
	if (vd->vdev_path == NULL ||
	    strncmp(vd->vdev_path, "remote://", 9) != 0) {
		vd->vdev_stat.vs_aux = VDEV_AUX_BAD_LABEL;
		return (SET_ERROR(EINVAL));
	}

	/* Reopen: reconnect if needed, then re-read device info */
	if (vd->vdev_tsd != NULL) {
		ASSERT(vd->vdev_reopening);
		vr = vd->vdev_tsd;

		/*
		 * If the connection was lost (e.g. server restart, network
		 * blip) proactively re-establish the link so that
		 * vdev_remote_os_info below succeeds.
		 *
		 * vdev_remote_os_connect resets backoff and tears down any
		 * stale socket, so this is safe to call unconditionally.
		 */
		if (!vr->vr_connected) {
			error = vdev_remote_os_connect(vr);
			if (error == 0)
				vr->vr_connected = B_TRUE;
		}
		goto skip_open;
	}

	vr = vd->vdev_tsd = kmem_zalloc(sizeof (vdev_remote_t), KM_SLEEP);
	mutex_init(&vr->vr_lock, NULL, MUTEX_DEFAULT, NULL);

	/* Parse host:port from the URI */
	error = vdev_remote_parse_uri(vd->vdev_path,
	    vr->vr_host, sizeof (vr->vr_host), &vr->vr_port);
	if (error != 0) {
		vd->vdev_stat.vs_aux = VDEV_AUX_BAD_LABEL;
		mutex_destroy(&vr->vr_lock);
		kmem_free(vr, sizeof (vdev_remote_t));
		vd->vdev_tsd = NULL;
		return (error);
	}

	/* Connect to the remote daemon */
	error = vdev_remote_os_connect(vr);
	if (error != 0) {
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
		mutex_destroy(&vr->vr_lock);
		kmem_free(vr, sizeof (vdev_remote_t));
		vd->vdev_tsd = NULL;
		return (error);
	}

	vr->vr_connected = B_TRUE;

skip_open:
	/* Query device info from remote daemon */
	error = vdev_remote_os_info(vr, &vr->vr_device_size,
	    &vr->vr_block_size, &vr->vr_phys_block_size);
	if (error != 0) {
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
		if (!vd->vdev_reopening) {
			vdev_remote_os_disconnect(vr);
			mutex_destroy(&vr->vr_lock);
			kmem_free(vr, sizeof (vdev_remote_t));
			vd->vdev_tsd = NULL;
		}
		return (error);
	}

	*psize = vr->vr_device_size;
	*max_psize = vr->vr_device_size;

	/* Set ashift based on the remote block size */
	if (vr->vr_block_size == 0)
		vr->vr_block_size = DEV_BSIZE;
	if (vr->vr_phys_block_size == 0)
		vr->vr_phys_block_size = vr->vr_block_size;

	*logical_ashift = highbit64(vr->vr_block_size) - 1;
	*physical_ashift = highbit64(vr->vr_phys_block_size) - 1;

	return (0);
}

static void
vdev_remote_close(vdev_t *vd)
{
	vdev_remote_t *vr = vd->vdev_tsd;

	if (vd->vdev_reopening || vr == NULL)
		return;

	if (vr->vr_connected) {
		vdev_remote_os_disconnect(vr);
		vr->vr_connected = B_FALSE;
	}

	mutex_destroy(&vr->vr_lock);
	kmem_free(vr, sizeof (vdev_remote_t));
	vd->vdev_tsd = NULL;
	vd->vdev_delayed_close = B_FALSE;
}

/*
 * Background I/O strategy: performs the actual RPC call.
 * Runs on the vdev_remote_taskq.
 *
 * The OS layer (vdev_remote_os_io) handles connection state, reconnect,
 * and backoff internally, so we just call through unconditionally.
 */
static void
vdev_remote_io_strategy(void *arg)
{
	zio_t *zio = (zio_t *)arg;
	vdev_t *vd = zio->io_vd;
	vdev_remote_t *vr = vd->vdev_tsd;
	void *buf = NULL;
	uint32_t size = (uint32_t)zio->io_size;

	if (vr == NULL) {
		zio->io_error = SET_ERROR(ENXIO);
		zio_delay_interrupt(zio);
		return;
	}

	if (zio->io_type == ZIO_TYPE_READ) {
		buf = abd_borrow_buf(zio->io_abd, size);
		zio->io_error = vdev_remote_os_io(vr, VDEV_REMOTE_CMD_READ,
		    zio->io_offset, buf, size);
		if (zio->io_error == 0)
			abd_return_buf_copy(zio->io_abd, buf, size);
		else
			abd_return_buf(zio->io_abd, buf, size);
	} else {
		/* ZIO_TYPE_WRITE */
		buf = abd_borrow_buf_copy(zio->io_abd, size);
		zio->io_error = vdev_remote_os_io(vr, VDEV_REMOTE_CMD_WRITE,
		    zio->io_offset, buf, size);
		abd_return_buf(zio->io_abd, buf, size);
	}

	zio_delay_interrupt(zio);
}

/*
 * Flush (sync) operation: tells remote daemon to fsync.
 *
 * The OS layer handles reconnect internally.
 */
static void
vdev_remote_io_flush(void *arg)
{
	zio_t *zio = (zio_t *)arg;
	vdev_t *vd = zio->io_vd;
	vdev_remote_t *vr = vd->vdev_tsd;

	if (vr == NULL) {
		zio->io_error = SET_ERROR(ENXIO);
		zio_interrupt(zio);
		return;
	}

	zio->io_error = vdev_remote_os_io(vr, VDEV_REMOTE_CMD_FLUSH, 0, NULL, 0);
	zio_interrupt(zio);
}

/*
 * TRIM/UNMAP operation: tells remote daemon to deallocate.
 *
 * The OS layer handles reconnect internally.
 */
static void
vdev_remote_io_trim(void *arg)
{
	zio_t *zio = (zio_t *)arg;
	vdev_t *vd = zio->io_vd;
	vdev_remote_t *vr = vd->vdev_tsd;

	if (vr == NULL) {
		zio->io_error = SET_ERROR(ENXIO);
		zio_interrupt(zio);
		return;
	}

	zio->io_error = vdev_remote_os_io(vr, VDEV_REMOTE_CMD_TRIM,
	    zio->io_offset, NULL, (uint32_t)zio->io_size);
	zio_interrupt(zio);
}

/*
 * I/O start entry point: dispatches the appropriate async handler.
 */
static void
vdev_remote_io_start(zio_t *zio)
{
	vdev_t *vd = zio->io_vd;

	if (zio->io_type == ZIO_TYPE_FLUSH) {
		if (!vdev_readable(vd)) {
			zio->io_error = SET_ERROR(ENXIO);
			zio_interrupt(zio);
			return;
		}
		if (zfs_nocacheflush) {
			zio_interrupt(zio);
			return;
		}
		VERIFY3U(taskq_dispatch(vdev_remote_taskq,
		    vdev_remote_io_flush, zio, TQ_SLEEP), !=,
		    TASKQID_INVALID);
		return;
	}

	if (zio->io_type == ZIO_TYPE_TRIM) {
		ASSERT3U(zio->io_size, !=, 0);
		VERIFY3U(taskq_dispatch(vdev_remote_taskq,
		    vdev_remote_io_trim, zio, TQ_SLEEP), !=,
		    TASKQID_INVALID);
		return;
	}

	ASSERT(zio->io_type == ZIO_TYPE_READ ||
	    zio->io_type == ZIO_TYPE_WRITE);
	zio->io_target_timestamp = zio_handle_io_delay(zio);

	VERIFY3U(taskq_dispatch(vdev_remote_taskq,
	    vdev_remote_io_strategy, zio, TQ_SLEEP), !=,
	    TASKQID_INVALID);
}

static void
vdev_remote_io_done(zio_t *zio)
{
	(void) zio;
}

/*
 * Remote VDEV operations vector.
 */
vdev_ops_t vdev_remote_ops = {
	.vdev_op_init = NULL,
	.vdev_op_fini = NULL,
	.vdev_op_open = vdev_remote_open,
	.vdev_op_close = vdev_remote_close,
	.vdev_op_psize_to_asize = vdev_default_asize,
	.vdev_op_asize_to_psize = vdev_default_psize,
	.vdev_op_min_asize = vdev_default_min_asize,
	.vdev_op_min_alloc = NULL,
	.vdev_op_io_start = vdev_remote_io_start,
	.vdev_op_io_done = vdev_remote_io_done,
	.vdev_op_state_change = NULL,
	.vdev_op_need_resilver = NULL,
	.vdev_op_hold = vdev_remote_hold,
	.vdev_op_rele = vdev_remote_rele,
	.vdev_op_remap = NULL,
	.vdev_op_xlate = vdev_default_xlate,
	.vdev_op_rebuild_asize = NULL,
	.vdev_op_metaslab_init = NULL,
	.vdev_op_config_generate = NULL,
	.vdev_op_nparity = NULL,
	.vdev_op_ndisks = NULL,
	.vdev_op_type = VDEV_TYPE_REMOTE,	/* name of this vdev type */
	.vdev_op_leaf = B_TRUE			/* leaf vdev */
};
