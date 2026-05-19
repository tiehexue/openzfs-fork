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
 * Windows remote VDEV using Winsock Kernel (WSK).
 *
 * CRITICAL: All WSK calls require IRPs. Event-based synchronous wrappers
 * used here are safe because taskq workers run at PASSIVE_LEVEL.
 *
 * WSK socket dispatch hierarchy:
 *   Global WSK_PROVIDER_DISPATCH:  WskSocket, WskSocketConnect
 *   Socket WSK_PROVIDER_CONNECTION_DISPATCH: WskBind, WskConnect,
 *     WskSend, WskReceive, WskDisconnect, WskCloseSocket(Basic)
 */

#include <sys/zfs_context.h>
#include <sys/vdev_remote.h>
#include <sys/vdev_impl.h>
#include <sys/spa.h>

#include <ntddk.h>
#include <wsk.h>

static __inline uint16_t
wsk_htons(uint16_t hostshort)
{
	return (RtlUshortByteSwap(hostshort));
}

/*
 * Synchronous IRP completion routine.
 * Signals the event, returns MORE_PROCESSING so I/O mgr doesn't free the IRP
 * (we own it and free it after inspecting IoStatus).
 */
static NTSTATUS
wsk_sync_comp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
	(void)DeviceObject;
	(void)Irp;
	KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
	return (STATUS_MORE_PROCESSING_REQUIRED);
}

/* Issue a WSK call synchronously: wait on event if PENDING, return status.
 *
 * timeout_ms — if the IRP stays pending longer than this, IoCancelIrp is
 * issued and we wait for the cancelled completion.  This prevents an
 * unresponsive remote peer from blocking a taskq thread indefinitely.
 */
static NTSTATUS
wsk_sync_timeout(KEVENT *event, PIRP irp, NTSTATUS st, ULONG timeout_ms)
{
	if (st == STATUS_PENDING) {
		LARGE_INTEGER li;
		li.QuadPart = -(LONGLONG)timeout_ms * 10000;
		NTSTATUS wait_st = KeWaitForSingleObject(event, Executive,
		    KernelMode, FALSE, &li);
		if (wait_st == STATUS_TIMEOUT) {
			/*
			 * Cancel the IRP.  WSK owns the cancel routine so
			 * IoCancelIrp is safe.  Returns TRUE if cancel was
			 * queued; in that case we must wait for completion.
			 * Returns FALSE if the IRP already completed (or is
			 * about to), so IoStatus is already valid.
			 */
			if (IoCancelIrp(irp)) {
				KeWaitForSingleObject(event, Executive,
				    KernelMode, FALSE, NULL);
			}
		}
		st = irp->IoStatus.Status;
	}
	return (st);
}

/*
 * WSK global state with interlocked init.
 * Using InterlockedCompareExchange avoids needing mutex_init before first use.
 */
static WSK_REGISTRATION	wsk_reg = { 0 };
static WSK_PROVIDER_NPI	wsk_npi;
static WSK_CLIENT_NPI	wsk_cli = { NULL, NULL };
static LONG		wsk_ready = 0;  /* 0=not init, 1=initing, 2=ready */

static const WSK_CLIENT_DISPATCH wsk_disp = {
	MAKE_WSK_VERSION(1, 0), 0, NULL
};

static int vdev_remote_wsk_init(void);
static int vdev_remote_wsk_connect(vdev_remote_t *vr,
    PWSK_SOCKET *out);
static void vdev_remote_wsk_close(PWSK_SOCKET sock, boolean_t is_dead);
static int vdev_remote_wsk_send_recv(PWSK_SOCKET sock,
    vdev_remote_rpc_hdr_t *hdr, void *data, uint32_t dlen, uint32_t cmd);

/*
 * Lazy one-shot WSK init. Interlocked state machine ensures exactly one
 * thread does WskRegister/WskCaptureProviderNPI. Other threads spin-wait.
 */
static int
vdev_remote_wsk_init(void)
{
	LONG prev;

	/* Quick check: already ready? */
	if (wsk_ready == 2)
		return (0);

	/* Try to claim the "initing" slot */
	prev = InterlockedCompareExchange(&wsk_ready, 1, 0);
	if (prev == 2)
		return (0); /* another thread finished while we were racing */

	if (prev == 1) {
		/* Another thread is initializing — wait for it */
		LONG spin = 0;
		while (wsk_ready == 1 && spin < 100000) {
			KeStallExecutionProcessor(10);
			spin++;
		}
		if (wsk_ready != 2) {
			dprintf("vdev_remote: WSK init timed out\n");
			return (SET_ERROR(ENODEV));
		}
		return (0);
	}

	/* We own the init slot (prev == 0) */
	wsk_cli.Dispatch = (PWSK_CLIENT_DISPATCH)&wsk_disp;

	NTSTATUS st = WskRegister(&wsk_cli, &wsk_reg);
	if (!NT_SUCCESS(st)) {
		wsk_ready = 0;
		dprintf("vdev_remote: WskRegister: 0x%lx\n",
		    (unsigned long)st);
		return (SET_ERROR(ENODEV));
	}

	st = WskCaptureProviderNPI(&wsk_reg, WSK_INFINITE_WAIT, &wsk_npi);
	if (!NT_SUCCESS(st)) {
		WskDeregister(&wsk_reg);
		wsk_ready = 0;
		dprintf("vdev_remote: WskCaptureProviderNPI: 0x%lx\n",
		    (unsigned long)st);
		return (SET_ERROR(ENODEV));
	}

	wsk_ready = 2;
	return (0);
}

/*
 * Connect to remote daemon using WskSocketConnect: creates socket,
 * binds locally, and connects — all in one IRP round-trip.
 */
static int
vdev_remote_wsk_connect(vdev_remote_t *vr, PWSK_SOCKET *out)
{
	SOCKADDR_IN local = { 0 };
	SOCKADDR_IN remote = { 0 };
	PIRP irp;
	KEVENT ev;
	NTSTATUS st;
	PCSTR term = NULL;

	*out = NULL;

	remote.sin_family = AF_INET;
	remote.sin_port = wsk_htons(vr->vr_port);

	st = RtlIpv4StringToAddressA(vr->vr_host, TRUE,
	    &term, &remote.sin_addr);
	if (!NT_SUCCESS(st)) {
		dprintf("vdev_remote: bad IPv4 '%s': 0x%lx\n",
		    vr->vr_host, (unsigned long)st);
		return (SET_ERROR(EINVAL));
	}

	local.sin_family = AF_INET;
	local.sin_addr.s_addr = INADDR_ANY;
	local.sin_port = 0;

	KeInitializeEvent(&ev, SynchronizationEvent, FALSE);
	irp = IoAllocateIrp(1, FALSE);
	if (!irp) return (SET_ERROR(ENOMEM));
	IoSetCompletionRoutine(irp, wsk_sync_comp, &ev, TRUE, TRUE, TRUE);

	st = wsk_sync_timeout(&ev, irp,
	    wsk_npi.Dispatch->WskSocketConnect(
	    wsk_npi.Client, SOCK_STREAM, IPPROTO_TCP,
	    (PSOCKADDR)&local, (PSOCKADDR)&remote,
	    0, NULL, NULL, NULL, NULL, NULL, irp),
	    VDEV_REMOTE_WSK_TIMEOUT_MS);

	if (!NT_SUCCESS(st)) {
		dprintf("vdev_remote: WskSocketConnect %s:%u: 0x%lx\n",
		    vr->vr_host, vr->vr_port, (unsigned long)st);
		IoFreeIrp(irp);
		return (SET_ERROR(ECONNREFUSED));
	}
	*out = (PWSK_SOCKET)irp->IoStatus.Information;
	IoFreeIrp(irp);
	return (0);
}

/*
 * Close a WSK socket.
 *
 * When 'is_dead' is true the socket is known (or suspected) to be broken;
 * skip the graceful WskDisconnect and go straight to WskCloseSocket to
 * avoid blocking on a dead peer.
 */
static void
vdev_remote_wsk_close(PWSK_SOCKET sock, boolean_t is_dead)
{
	PIRP irp;
	KEVENT ev;
	PWSK_PROVIDER_CONNECTION_DISPATCH d;

	if (!sock) return;
	d = (PWSK_PROVIDER_CONNECTION_DISPATCH)sock->Dispatch;

	if (!is_dead) {
		KeInitializeEvent(&ev, SynchronizationEvent, FALSE);
		irp = IoAllocateIrp(1, FALSE);
		if (irp) {
			IoSetCompletionRoutine(irp, wsk_sync_comp, &ev,
			    TRUE, TRUE, TRUE);
			(void)wsk_sync_timeout(&ev, irp,
			    d->WskDisconnect(sock, NULL, 0, irp),
			    VDEV_REMOTE_WSK_TIMEOUT_MS);
			IoFreeIrp(irp);
		}
	}

	/* Close (Basic dispatch — IRP is optional but we pass one) */
	d->WskCloseSocket(sock, NULL);
}

/*
 * Round-trip RPC: send header [+data if write], recv header [+data if read].
 *
 * WSK_BUF.Mdl == NULL → WSK uses Irp->MdlAddress. We build a simple MDL
 * for the buffer pages to ensure correct transfer regardless of WSK provider
 * implementation (some require MDL, some accept UserBuffer).
 */
static int
vdev_remote_wsk_send_recv(PWSK_SOCKET sock,
    vdev_remote_rpc_hdr_t *hdr, void *data, uint32_t dlen, uint32_t cmd)
{
	NTSTATUS st;
	WSK_BUF wb;
	PIRP irp;
	KEVENT ev;
	PMDL mdl;
	PWSK_PROVIDER_CONNECTION_DISPATCH d;

	d = (PWSK_PROVIDER_CONNECTION_DISPATCH)sock->Dispatch;

	/* Populate header */
	hdr->vr_cmd = cmd;
	hdr->vr_status = 0;
	hdr->vr_size = dlen;
	hdr->vr_reserved = 0;

	/*
	 * -- Send header --
	 * Build an MDL for the header so WSK always sees valid MdlAddress.
	 */
	mdl = IoAllocateMdl(hdr, (ULONG)sizeof(*hdr), FALSE, FALSE, NULL);
	if (!mdl) return (SET_ERROR(ENOMEM));
	MmBuildMdlForNonPagedPool(mdl);

	wb.Mdl = mdl;
	wb.Offset = 0;
	wb.Length = sizeof(*hdr);

	KeInitializeEvent(&ev, SynchronizationEvent, FALSE);
	irp = IoAllocateIrp(1, FALSE);
	if (!irp) { IoFreeMdl(mdl); return (SET_ERROR(ENOMEM)); }
	IoSetCompletionRoutine(irp, wsk_sync_comp, &ev, TRUE, TRUE, TRUE);
	irp->MdlAddress = mdl;

	st = wsk_sync_timeout(&ev, irp,
	    d->WskSend(sock, &wb, WSK_FLAG_NODELAY, irp),
	    VDEV_REMOTE_WSK_TIMEOUT_MS);
	IoFreeMdl(mdl);
	IoFreeIrp(irp);
	if (!NT_SUCCESS(st)) {
		dprintf("vdev_remote: send hdr err 0x%lx\n",
		    (unsigned long)st);
		return (SET_ERROR(EIO));
	}

	/* -- If writing, send data payload -- */
	if (cmd == VDEV_REMOTE_CMD_WRITE && data && dlen > 0) {
		mdl = IoAllocateMdl(data, dlen, FALSE, FALSE, NULL);
		if (!mdl) return (SET_ERROR(ENOMEM));
		MmBuildMdlForNonPagedPool(mdl);

		wb.Mdl = mdl;
		wb.Offset = 0;
		wb.Length = dlen;

		KeInitializeEvent(&ev, SynchronizationEvent, FALSE);
		irp = IoAllocateIrp(1, FALSE);
		if (!irp) { IoFreeMdl(mdl); return (SET_ERROR(ENOMEM)); }
		IoSetCompletionRoutine(irp, wsk_sync_comp, &ev,
		    TRUE, TRUE, TRUE);
		irp->MdlAddress = mdl;

		st = wsk_sync_timeout(&ev, irp,
		    d->WskSend(sock, &wb, WSK_FLAG_NODELAY, irp),
		    VDEV_REMOTE_WSK_TIMEOUT_MS);
		IoFreeMdl(mdl);
		IoFreeIrp(irp);
		if (!NT_SUCCESS(st)) {
			dprintf("vdev_remote: send dat err 0x%lx\n",
			    (unsigned long)st);
			return (SET_ERROR(EIO));
		}
	}

	/* -- Receive response header -- */
	mdl = IoAllocateMdl(hdr, (ULONG)sizeof(*hdr), FALSE, FALSE, NULL);
	if (!mdl) return (SET_ERROR(ENOMEM));
	MmBuildMdlForNonPagedPool(mdl);

	wb.Mdl = mdl;
	wb.Offset = 0;
	wb.Length = sizeof(*hdr);

	KeInitializeEvent(&ev, SynchronizationEvent, FALSE);
	irp = IoAllocateIrp(1, FALSE);
	if (!irp) { IoFreeMdl(mdl); return (SET_ERROR(ENOMEM)); }
	IoSetCompletionRoutine(irp, wsk_sync_comp, &ev, TRUE, TRUE, TRUE);
	irp->MdlAddress = mdl;

	st = wsk_sync_timeout(&ev, irp,
	    d->WskReceive(sock, &wb, WSK_FLAG_WAITALL, irp),
	    VDEV_REMOTE_WSK_TIMEOUT_MS);
	IoFreeMdl(mdl);
	IoFreeIrp(irp);
	if (!NT_SUCCESS(st)) {
		dprintf("vdev_remote: recv hdr err 0x%lx\n",
		    (unsigned long)st);
		return (SET_ERROR(EIO));
	}

	if (hdr->vr_status != VDEV_REMOTE_STATUS_OK) {
		dprintf("vdev_remote: srv err %u\n", hdr->vr_status);
		if (hdr->vr_status == VDEV_REMOTE_STATUS_ERR_NOSPC)
			return (SET_ERROR(ENOSPC));
		return (SET_ERROR(EIO));
	}

	/* -- If reading, receive data payload -- */
	if (cmd == VDEV_REMOTE_CMD_READ && data && hdr->vr_size > 0) {
		mdl = IoAllocateMdl(data, hdr->vr_size, FALSE, FALSE, NULL);
		if (!mdl) return (SET_ERROR(ENOMEM));
		MmBuildMdlForNonPagedPool(mdl);

		wb.Mdl = mdl;
		wb.Offset = 0;
		wb.Length = hdr->vr_size;

		KeInitializeEvent(&ev, SynchronizationEvent, FALSE);
		irp = IoAllocateIrp(1, FALSE);
		if (!irp) { IoFreeMdl(mdl); return (SET_ERROR(ENOMEM)); }
		IoSetCompletionRoutine(irp, wsk_sync_comp, &ev,
		    TRUE, TRUE, TRUE);
		irp->MdlAddress = mdl;

		st = wsk_sync_timeout(&ev, irp,
		    d->WskReceive(sock, &wb, WSK_FLAG_WAITALL, irp),
		    VDEV_REMOTE_WSK_TIMEOUT_MS);
		IoFreeMdl(mdl);
		IoFreeIrp(irp);
		if (!NT_SUCCESS(st)) {
			dprintf("vdev_remote: recv dat err 0x%lx\n",
			    (unsigned long)st);
			return (SET_ERROR(EIO));
		}
	}

	return (0);
}

/* ---- Public OS interface ---- */

/*
 * Reconnect to the remote daemon with exponential backoff.
 *
 * Must be called with vr_lock held.  On success vr_os_priv and
 * vr_connected are updated and the backoff state is reset.
 * On failure the backoff timer is advanced.
 *
 * Returns: 0 on success, errno on failure.
 */
static int
vdev_remote_os_reconnect(vdev_remote_t *vr)
{
	PWSK_SOCKET sock = NULL;
	int err;

	ASSERT(MUTEX_HELD(&vr->vr_lock));

	/*
	 * If we already have a live socket (e.g. racing reconnect from
	 * another path), just return success.
	 */
	if (vr->vr_connected && vr->vr_os_priv != NULL)
		return (0);

	/* Enforce backoff: don't retry before vr_reconnect_until. */
	if (vr->vr_reconnect_backoff > 0) {
		hrtime_t now = gethrtime();
		if (now < vr->vr_reconnect_until) {
			dprintf("vdev_remote: reconnect backoff %ums "
			    "(%lld ms remaining)\n",
			    vr->vr_reconnect_backoff,
			    (long long)((vr->vr_reconnect_until - now)
			    / (NANOSEC / MILLISEC)));
			return (SET_ERROR(EAGAIN));
		}
	}

	/* Clean up any stale socket before reconnecting. */
	if (vr->vr_os_priv != NULL) {
		vdev_remote_wsk_close((PWSK_SOCKET)vr->vr_os_priv, B_TRUE);
		vr->vr_os_priv = NULL;
	}
	vr->vr_connected = B_FALSE;

	err = vdev_remote_wsk_init();
	if (err)
		goto fail;

	err = vdev_remote_wsk_connect(vr, &sock);
	if (err)
		goto fail;

	vr->vr_os_priv = (void *)sock;
	vr->vr_connected = B_TRUE;

	/* Reset backoff on successful reconnect. */
	vr->vr_reconnect_backoff = 0;
	vr->vr_reconnect_until = 0;

	dprintf("vdev_remote: reconnected to %s:%u\n",
	    vr->vr_host, vr->vr_port);
	return (0);

fail:
	/* Advance exponential backoff. */
	if (vr->vr_reconnect_backoff == 0)
		vr->vr_reconnect_backoff = VDEV_REMOTE_RECONNECT_BACKOFF_MIN_MS;
	else
		vr->vr_reconnect_backoff = MIN(
		    vr->vr_reconnect_backoff * 2,
		    VDEV_REMOTE_RECONNECT_BACKOFF_MAX_MS);

	vr->vr_reconnect_until = gethrtime() +
	    (hrtime_t)vr->vr_reconnect_backoff * (NANOSEC / MILLISEC);

	dprintf("vdev_remote: reconnect %s:%u failed (err %d), "
	    "backoff %ums\n", vr->vr_host, vr->vr_port,
	    err, vr->vr_reconnect_backoff);
	return (err);
}

int
vdev_remote_os_connect(vdev_remote_t *vr)
{
	int err;
	PWSK_SOCKET sock = NULL;

	vr->vr_reconnect_backoff = 0;
	vr->vr_reconnect_until = 0;

	/* Clean up any stale socket before connecting. */
	if (vr->vr_os_priv != NULL) {
		vdev_remote_wsk_close((PWSK_SOCKET)vr->vr_os_priv, B_TRUE);
		vr->vr_os_priv = NULL;
	}
	vr->vr_connected = B_FALSE;

	err = vdev_remote_wsk_init();
	if (err) return (err);

	err = vdev_remote_wsk_connect(vr, &sock);
	if (err) return (err);

	vr->vr_os_priv = (void *)sock;
	return (0);
}

void
vdev_remote_os_disconnect(vdev_remote_t *vr)
{
	PWSK_SOCKET sock = (PWSK_SOCKET)vr->vr_os_priv;
	if (sock) {
		vdev_remote_wsk_close(sock, B_FALSE);
		vr->vr_os_priv = NULL;
	}
	vr->vr_reconnect_backoff = 0;
	vr->vr_reconnect_until = 0;
}

/*
 * Perform a single RPC round-trip over the current socket.
 * On transport-level failure the dead socket is closed and vr_connected
 * is cleared so a subsequent call will attempt reconnect.
 *
 * Returns 0 on success, errno on failure (EIO = transport error,
 * EAGAIN = backoff timer hasn't expired yet).
 */
static int
vdev_remote_os_io_locked(vdev_remote_t *vr, uint32_t cmd,
    uint64_t offset, void *data, uint32_t size)
{
	vdev_remote_rpc_hdr_t hdr;
	PWSK_SOCKET sock;
	int err;

	ASSERT(MUTEX_HELD(&vr->vr_lock));

	/* Try to (re)connect if needed. */
	if (!vr->vr_connected || vr->vr_os_priv == NULL) {
		err = vdev_remote_os_reconnect(vr);
		if (err)
			return (err);
	}

	sock = (PWSK_SOCKET)vr->vr_os_priv;
	ASSERT(sock != NULL);

	memset(&hdr, 0, sizeof(hdr));
	hdr.vr_cmd = cmd;
	hdr.vr_offset = offset;
	hdr.vr_size = size;

	err = vdev_remote_wsk_send_recv(sock, &hdr, data, size, cmd);

	/*
	 * If the transport failed the socket is dead; close it now so the
	 * next caller will attempt reconnect (subject to backoff).
	 */
	if (err == EIO) {
		vdev_remote_wsk_close(sock, B_TRUE);
		vr->vr_os_priv = NULL;
		vr->vr_connected = B_FALSE;
		dprintf("vdev_remote: socket %s:%u marked dead "
		    "(cmd 0x%x, err %d)\n",
		    vr->vr_host, vr->vr_port, cmd, err);
	}

	return (err);
}

int
vdev_remote_os_io(vdev_remote_t *vr, uint32_t cmd,
    uint64_t offset, void *data, uint32_t size)
{
	int err;

	ASSERT(vr);
	mutex_enter(&vr->vr_lock);

	/*
	 * TRIM does not carry a data payload but otherwise follows the
	 * same send-recv pattern.
	 */
	if (cmd == VDEV_REMOTE_CMD_TRIM) {
		err = vdev_remote_os_io_locked(vr, cmd, offset, NULL, 0);
		mutex_exit(&vr->vr_lock);
		return (err);
	}

	err = vdev_remote_os_io_locked(vr, cmd, offset, data, size);
	mutex_exit(&vr->vr_lock);
	return (err);
}

int
vdev_remote_os_info(vdev_remote_t *vr,
    uint64_t *size, uint32_t *blksz, uint32_t *pblksz)
{
	vdev_remote_rpc_hdr_t hdr;
	PWSK_SOCKET sock;
	int err;

	ASSERT(vr);
	mutex_enter(&vr->vr_lock);

	/* Try to (re)connect if needed. */
	if (!vr->vr_connected || vr->vr_os_priv == NULL) {
		err = vdev_remote_os_reconnect(vr);
		if (err) {
			mutex_exit(&vr->vr_lock);
			return (err);
		}
	}

	sock = (PWSK_SOCKET)vr->vr_os_priv;
	ASSERT(sock != NULL);

	memset(&hdr, 0, sizeof(hdr));
	err = vdev_remote_wsk_send_recv(sock, &hdr, NULL, 0,
	    VDEV_REMOTE_CMD_INFO);

	if (err == EIO) {
		vdev_remote_wsk_close(sock, B_TRUE);
		vr->vr_os_priv = NULL;
		vr->vr_connected = B_FALSE;
	}

	mutex_exit(&vr->vr_lock);
	if (err == 0) {
		*size = hdr.vr_offset;
		*blksz = hdr.vr_size;
		*pblksz = hdr.vr_reserved;
	}
	return (err);
}
