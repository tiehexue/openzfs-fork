/*
 * CDDL HEADER START ... (see LICENSE)
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2026, OpenZFS Remote VDEV contributors.
 *
 * zfs_remoted - ZFS Remote Block Device Daemon for Windows
 *
 * Serves a block device over TCP using the remote VDEV RPC protocol.
 * Supports up to 64 concurrent ZFS kernel clients.
 *
 * Usage:
 *   zfs_remoted -f <image_file> -p <port>   (file backend)
 *   zfs_remoted -d <disk_id>    -p <port>   (disk backend)
 */

#include "zfs_remoted.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>

#pragma comment(lib, "Ws2_32.lib")

/* ---- constants ---- */
#define MAX_CLIENTS 64

/* ---- daemon state ---- */
static volatile LONG    g_running = 1;
static block_backend_t  g_backend;
static CRITICAL_SECTION g_backend_lock;

/* ---- client-thread tracking ---- */
static HANDLE           g_threads[MAX_CLIENTS];
static LONG             g_thread_count = 0;
static CRITICAL_SECTION g_threads_lock;

/* ---- forward declarations ---- */
static void handle_read(SOCKET s, rpc_hdr_t *hdr);
static void handle_write(SOCKET s, rpc_hdr_t *hdr);
static void handle_flush(SOCKET s);
static void handle_trim(SOCKET s, rpc_hdr_t *hdr);
static void handle_info(SOCKET s);
static DWORD WINAPI handle_client_thread(LPVOID param);

/* ---- network helpers ---- */
int
recv_all(SOCKET s, void *buf, int len)
{
	char *p = (char *)buf;
	int remaining = len;
	while (remaining > 0) {
		int n = recv(s, p, remaining, 0);
		if (n <= 0) return -1;
		p += n; remaining -= n;
	}
	return 0;
}

int
send_all(SOCKET s, const void *buf, int len)
{
	const char *p = (const char *)buf;
	int remaining = len;
	while (remaining > 0) {
		int n = send(s, p, remaining, 0);
		if (n <= 0) return -1;
		p += n; remaining -= n;
	}
	return 0;
}

/* ---- RPC command handlers ---- */

static void handle_read(SOCKET s, rpc_hdr_t *hdr)
{
	rpc_hdr_t resp;

	if (hdr->size == 0 || hdr->size > (256 * 1024 * 1024)) {
		resp.cmd = CMD_READ; resp.status = STATUS_ERR_INVAL;
		resp.offset = hdr->offset; resp.size = 0; resp.reserved = 0;
		send_all(s, &resp, sizeof(resp));
		return;
	}

	uint8_t *buf = (uint8_t *)malloc(hdr->size);
	if (!buf) {
		resp.cmd = CMD_READ; resp.status = STATUS_ERR_IO;
		resp.offset = hdr->offset; resp.size = 0; resp.reserved = 0;
		send_all(s, &resp, sizeof(resp));
		return;
	}

	EnterCriticalSection(&g_backend_lock);
	int err = g_backend.bb_read(&g_backend, buf, hdr->size, hdr->offset);
	LeaveCriticalSection(&g_backend_lock);

	if (err) {
		resp.cmd = CMD_READ; resp.status = STATUS_ERR_IO;
		resp.offset = hdr->offset; resp.size = 0; resp.reserved = 0;
		send_all(s, &resp, sizeof(resp));
		free(buf);
		return;
	}

	resp.cmd = CMD_READ; resp.status = STATUS_OK;
	resp.offset = hdr->offset; resp.size = hdr->size; resp.reserved = 0;
	if (send_all(s, &resp, sizeof(resp)) == 0)
		send_all(s, buf, hdr->size);
	free(buf);
}

static void handle_write(SOCKET s, rpc_hdr_t *hdr)
{
	rpc_hdr_t resp;

	if (hdr->size == 0 || hdr->size > (256 * 1024 * 1024)) {
		resp.cmd = CMD_WRITE; resp.status = STATUS_ERR_INVAL;
		resp.offset = hdr->offset; resp.size = 0; resp.reserved = 0;
		send_all(s, &resp, sizeof(resp));
		return;
	}

	uint8_t *buf = (uint8_t *)malloc(hdr->size);
	if (!buf) {
		resp.cmd = CMD_WRITE; resp.status = STATUS_ERR_IO;
		resp.offset = hdr->offset; resp.size = 0; resp.reserved = 0;
		send_all(s, &resp, sizeof(resp));
		return;
	}
	if (recv_all(s, buf, hdr->size) != 0) { free(buf); return; }

	EnterCriticalSection(&g_backend_lock);
	int err = g_backend.bb_write(&g_backend, buf, hdr->size,
	    hdr->offset);
	LeaveCriticalSection(&g_backend_lock);
	free(buf);

	resp.cmd = CMD_WRITE;
	resp.status = err ? STATUS_ERR_IO : STATUS_OK;
	resp.offset = hdr->offset;
	resp.size   = err ? 0 : hdr->size;
	resp.reserved = 0;
	send_all(s, &resp, sizeof(resp));
}

static void handle_flush(SOCKET s)
{
	rpc_hdr_t resp;
	EnterCriticalSection(&g_backend_lock);
	int err = g_backend.bb_flush(&g_backend);
	LeaveCriticalSection(&g_backend_lock);

	resp.cmd = CMD_FLUSH;
	resp.status = err ? STATUS_ERR_IO : STATUS_OK;
	resp.offset = 0; resp.size = 0; resp.reserved = 0;
	send_all(s, &resp, sizeof(resp));
}

static void handle_trim(SOCKET s, rpc_hdr_t *hdr)
{
	rpc_hdr_t resp;
	resp.cmd = CMD_TRIM; resp.status = STATUS_OK;
	resp.offset = hdr->offset; resp.size = hdr->size; resp.reserved = 0;
	send_all(s, &resp, sizeof(resp));
}

static void
handle_info(SOCKET s)
{
	rpc_hdr_t resp;
	EnterCriticalSection(&g_backend_lock);
	resp.cmd = CMD_INFO; resp.status = STATUS_OK;
	resp.offset = g_backend.bb_dev_size;
	resp.size   = g_backend.bb_lbasize;
	resp.reserved = g_backend.bb_pbasize;
	LeaveCriticalSection(&g_backend_lock);
	send_all(s, &resp, sizeof(resp));
}

/* ---- thread scavenger: reap finished client threads ---- */
static void
scavenge_threads(void)
{
	EnterCriticalSection(&g_threads_lock);
	int write_idx = 0;
	for (int i = 0; i < g_thread_count; i++) {
		if (WaitForSingleObject(g_threads[i], 0) == WAIT_OBJECT_0) {
			/* thread has exited — clean up */
			CloseHandle(g_threads[i]);
		} else {
			/* thread still running — keep it */
			g_threads[write_idx++] = g_threads[i];
		}
	}
	g_thread_count = write_idx;
	LeaveCriticalSection(&g_threads_lock);
}

/* ---- per-client thread entry point ---- */
static DWORD WINAPI
handle_client_thread(LPVOID param)
{
	SOCKET client_sock = (SOCKET)(INT_PTR)param;
	rpc_hdr_t hdr;
	DWORD tid = GetCurrentThreadId();

	fprintf(stderr, "[tid %lu] connected (%s backend)\n",
	    tid, g_backend.bb_label);

	while (g_running) {
		/*
		 * Wait for the next RPC header with a 1 s timeout so we
		 * re-check g_running periodically and can shut down cleanly.
		 */
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(client_sock, &rfds);
		struct timeval tv = { 1, 0 };
		int sr = select(0, &rfds, NULL, NULL, &tv);
		if (sr < 0)
			break;                    /* socket error */
		if (sr == 0)
			continue;                /* timeout → re-check g_running */

		if (recv_all(client_sock, &hdr, sizeof(hdr)) != 0)
			break;                    /* client closed or error */

		switch (hdr.cmd) {
		case CMD_READ:
			handle_read(client_sock, &hdr);
			break;
		case CMD_WRITE:
			handle_write(client_sock, &hdr);
			break;
		case CMD_FLUSH:
			handle_flush(client_sock);
			break;
		case CMD_TRIM:
			handle_trim(client_sock, &hdr);
			break;
		case CMD_INFO:
			handle_info(client_sock);
			break;
		default:
			fprintf(stderr, "[tid %lu] unknown cmd 0x%08X\n",
			    tid, hdr.cmd);
			hdr.status = STATUS_ERR_INVAL;
			send_all(client_sock, &hdr, sizeof(hdr));
			break;
		}
	}

	closesocket(client_sock);
	fprintf(stderr, "[tid %lu] disconnected\n", tid);
	return 0;
}

/* ---- usage / ctrl-c ---- */
static void
print_usage(const char *prog)
{
	fprintf(stderr,
	    "Usage:\n"
	    "  %s -f <image_file> -p <port>     serve a raw image file\n"
	    "  %s -d <disk>       -p <port>     serve a physical disk\n"
	    "\nDisk examples:  -d 0   -d physicaldrive1   -d \\\\.\\PhysicalDrive2\n",
	    prog, prog);
}

static BOOL WINAPI
ctrl_handler(DWORD ctrl_type)
{
	(void)ctrl_type;
	InterlockedExchange(&g_running, 0);
	return TRUE;
}

/* ---- main ---- */
int
main(int argc, char *argv[])
{
	const char *file_spec = NULL;
	const char *disk_spec = NULL;
	uint16_t port = 0;

	/* ensure output immediately visible */
	setvbuf(stderr, NULL, _IONBF, 0);
	fprintf(stderr, "zfs_remoted starting (multi-client mode)...\n");

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
			file_spec = argv[++i];
		else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
			disk_spec = argv[++i];
		else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
			port = (uint16_t)atoi(argv[++i]);
		else if (strcmp(argv[i], "-h") == 0)
			{ print_usage(argv[0]); return 0; }
		else {
			fprintf(stderr, "Unknown: %s\n", argv[i]);
			print_usage(argv[0]); return 1;
		}
	}

	if ((!file_spec && !disk_spec) || port == 0)
		{ print_usage(argv[0]); return 1; }
	if (file_spec && disk_spec)
		{ fprintf(stderr, "Use -f OR -d, not both.\n"); return 1; }

	/* ---- open backend ---- */
	memset(&g_backend, 0, sizeof(g_backend));
	g_backend = file_spec ? file_backend : disk_backend;

	{
		int rc = g_backend.bb_open(&g_backend,
		    file_spec ? file_spec : disk_spec);
		if (rc) {
			fprintf(stderr, "Failed to open backend: %lu\n",
			    GetLastError());
			return 1;
		}
	}

	fprintf(stderr, "Backend opened, %llu bytes\n",
	    (unsigned long long)g_backend.bb_dev_size);

	InitializeCriticalSection(&g_backend_lock);
	InitializeCriticalSection(&g_threads_lock);

	/* winsock */
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		fprintf(stderr, "WSAStartup failed: %d\n", WSAGetLastError());
		g_backend.bb_close(&g_backend);
		DeleteCriticalSection(&g_backend_lock);
		DeleteCriticalSection(&g_threads_lock);
		return 1;
	}

	SOCKET lsn = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (lsn == INVALID_SOCKET) {
		fprintf(stderr, "socket failed: %d\n", WSAGetLastError());
		WSACleanup();
		g_backend.bb_close(&g_backend);
		DeleteCriticalSection(&g_backend_lock);
		DeleteCriticalSection(&g_threads_lock);
		return 1;
	}

	{
		int reuse = 1;
		setsockopt(lsn, SOL_SOCKET, SO_REUSEADDR,
		    (const char *)&reuse, sizeof(reuse));
	}

	struct sockaddr_in srv = { 0 };
	srv.sin_family = AF_INET;
	srv.sin_port = htons(port);

	if (bind(lsn, (struct sockaddr *)&srv, sizeof(srv)) != 0) {
		fprintf(stderr, "bind port %u failed: %d\n",
		    port, WSAGetLastError());
		closesocket(lsn);
		WSACleanup();
		g_backend.bb_close(&g_backend);
		DeleteCriticalSection(&g_backend_lock);
		DeleteCriticalSection(&g_threads_lock);
		return 1;
	}

	if (listen(lsn, SOMAXCONN) != 0) {
		fprintf(stderr, "listen failed: %d\n", WSAGetLastError());
		closesocket(lsn);
		WSACleanup();
		g_backend.bb_close(&g_backend);
		DeleteCriticalSection(&g_backend_lock);
		DeleteCriticalSection(&g_threads_lock);
		return 1;
	}

	SetConsoleCtrlHandler(ctrl_handler, TRUE);
	fprintf(stderr,
	    "zfs_remoted: port %u, %s backend, %llu MiB, max %d clients\n",
	    port, g_backend.bb_label,
	    (unsigned long long)(g_backend.bb_dev_size / (1024 * 1024)),
	    MAX_CLIENTS);

	/* ---- accept loop ---- */
	while (g_running) {
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(lsn, &rfds);
		struct timeval tv = { 1, 0 };
		int sr = select(0, &rfds, NULL, NULL, &tv);
		if (sr < 0) {
			fprintf(stderr, "select error: %d\n",
			    WSAGetLastError());
			break;
		}
		if (sr == 0) {
			/* no incoming connections — reap finished threads */
			scavenge_threads();
			continue;
		}

		SOCKET cli = accept(lsn, NULL, NULL);
		if (cli == INVALID_SOCKET) {
			if (g_running)
				fprintf(stderr, "accept error: %d\n",
				    WSAGetLastError());
			continue;
		}

		/* spawn a worker thread for this client */
		EnterCriticalSection(&g_threads_lock);
		if (g_thread_count >= MAX_CLIENTS) {
			LeaveCriticalSection(&g_threads_lock);
			fprintf(stderr,
			    "max clients (%d) reached, rejecting connection\n",
			    MAX_CLIENTS);
			closesocket(cli);
			continue;
		}

		HANDLE h = CreateThread(NULL, 0, handle_client_thread,
		    (LPVOID)(INT_PTR)cli, 0, NULL);
		if (h) {
			g_threads[g_thread_count++] = h;
			fprintf(stderr, "client accepted (%d/%d active)\n",
			    (int)g_thread_count, MAX_CLIENTS);
		} else {
			fprintf(stderr, "CreateThread failed: %lu\n",
			    GetLastError());
			closesocket(cli);
		}
		LeaveCriticalSection(&g_threads_lock);
	}

	/* ---- graceful shutdown ---- */
	fprintf(stderr, "Shutting down, waiting for %d client(s)...\n",
	    (int)g_thread_count);

	/* Close the listen socket so no new connections arrive. */
	closesocket(lsn);

	/*
	 * Wait for every client thread to finish.  Each thread checks
	 * g_running at least once per second and will exit promptly.
	 * Use a 10 s per-thread timeout as a safety net.
	 */
	EnterCriticalSection(&g_threads_lock);
	for (int i = 0; i < g_thread_count; i++) {
		DWORD wr = WaitForSingleObject(g_threads[i], 10000);
		if (wr == WAIT_TIMEOUT) {
			fprintf(stderr,
			    "warning: thread %d did not exit in time\n", i);
			TerminateThread(g_threads[i], 1);
		}
		CloseHandle(g_threads[i]);
	}
	g_thread_count = 0;
	LeaveCriticalSection(&g_threads_lock);

	DeleteCriticalSection(&g_threads_lock);
	DeleteCriticalSection(&g_backend_lock);
	WSACleanup();
	g_backend.bb_close(&g_backend);

	fprintf(stderr, "zfs_remoted stopped.\n");
	return 0;
}
