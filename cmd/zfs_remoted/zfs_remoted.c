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
 *
 * Usage:
 *   zfs_remoted -f <image_file> -p <port>   (file backend)
 *   zfs_remoted -d <disk_id>    -p <port>   (disk backend)
 */

#include "zfs_remoted.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "Ws2_32.lib")

/* ---- daemon state ---- */
static volatile LONG    g_running = 1;
static block_backend_t  g_backend;
static CRITICAL_SECTION g_backend_lock;

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

static void handle_info(SOCKET s)
{
	rpc_hdr_t resp;
	resp.cmd = CMD_INFO; resp.status = STATUS_OK;
	resp.offset = g_backend.bb_dev_size;
	resp.size   = g_backend.bb_lbasize;
	resp.reserved = g_backend.bb_pbasize;
	send_all(s, &resp, sizeof(resp));
}

/* ---- client connection loop ---- */
static void
handle_client(SOCKET client_sock)
{
	rpc_hdr_t hdr;
	fprintf(stderr, "Client connected (%s backend)\n", g_backend.bb_label);

	while (g_running) {
		if (recv_all(client_sock, &hdr, sizeof(hdr)) != 0) break;

		switch (hdr.cmd) {
		case CMD_READ:  handle_read(client_sock, &hdr);  break;
		case CMD_WRITE: handle_write(client_sock, &hdr); break;
		case CMD_FLUSH: handle_flush(client_sock);        break;
		case CMD_TRIM:  handle_trim(client_sock, &hdr);   break;
		case CMD_INFO:  handle_info(client_sock);         break;
		default:
			fprintf(stderr, "Unknown cmd 0x%08X\n", hdr.cmd);
			hdr.status = STATUS_ERR_INVAL;
			send_all(client_sock, &hdr, sizeof(hdr));
			break;
		}
	}
	closesocket(client_sock);
	fprintf(stderr, "Client disconnected\n");
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
	fprintf(stderr, "zfs_remoted starting...\n");

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

	/* open backend */
	memset(&g_backend, 0, sizeof(g_backend));
	g_backend = file_spec ? file_backend : disk_backend;
	fprintf(stderr, "Opening %s backend with '%s'...\n",
	    g_backend.bb_label, file_spec ? file_spec : disk_spec);
	fflush(stderr);

	int rc = g_backend.bb_open(&g_backend,
	    file_spec ? file_spec : disk_spec);
	fprintf(stderr, "bb_open returned %d\n", rc);
	fflush(stderr);

	if (rc) {
		fprintf(stderr, "Failed to open backend: %lu\n", GetLastError());
		return 1;
	}
	fprintf(stderr, "Backend opened, %llu bytes\n",
	    (unsigned long long)g_backend.bb_dev_size);

	InitializeCriticalSection(&g_backend_lock);

	/* winsock */
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		fprintf(stderr, "WSAStartup failed: %d\n", WSAGetLastError());
		g_backend.bb_close(&g_backend);
		return 1;
	}

	SOCKET lsn = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (lsn == INVALID_SOCKET) {
		fprintf(stderr, "socket failed: %d\n", WSAGetLastError());
		WSACleanup();
		g_backend.bb_close(&g_backend);
		return 1;
	}

	int reuse = 1;
	setsockopt(lsn, SOL_SOCKET, SO_REUSEADDR,
	    (const char *)&reuse, sizeof(reuse));

	struct sockaddr_in srv = { 0 };
	srv.sin_family = AF_INET;
	srv.sin_port = htons(port);

	if (bind(lsn, (struct sockaddr *)&srv, sizeof(srv)) != 0) {
		fprintf(stderr, "bind port %u failed: %d\n",
		    port, WSAGetLastError());
		closesocket(lsn);
		WSACleanup();
		g_backend.bb_close(&g_backend);
		return 1;
	}

	if (listen(lsn, SOMAXCONN) != 0) {
		fprintf(stderr, "listen failed: %d\n", WSAGetLastError());
		closesocket(lsn);
		WSACleanup();
		g_backend.bb_close(&g_backend);
		return 1;
	}

	SetConsoleCtrlHandler(ctrl_handler, TRUE);
	fprintf(stderr, "zfs_remoted: port %u, %s backend, %llu MiB\n",
	    port, g_backend.bb_label,
	    (unsigned long long)(g_backend.bb_dev_size / (1024*1024)));
	fflush(stderr);

	while (g_running) {
		struct timeval tv = { 1, 0 };
		fd_set rfds; FD_ZERO(&rfds); FD_SET(lsn, &rfds);
		int sr = select(0, &rfds, NULL, NULL, &tv);
		if (sr < 0) {
			fprintf(stderr, "select error: %d\n", WSAGetLastError());
			break;
		}
		if (sr == 0) continue;

		SOCKET cli = accept(lsn, NULL, NULL);
		if (cli != INVALID_SOCKET) handle_client(cli);
	}

	fprintf(stderr, "Shutting down...\n");
	closesocket(lsn);
	WSACleanup();
	DeleteCriticalSection(&g_backend_lock);
	g_backend.bb_close(&g_backend);
	return 0;
}
