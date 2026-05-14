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
 * zfs_remoted - ZFS Remote Block Device Daemon for Windows
 *
 * This daemon listens on a TCP port and serves a raw image file as a
 * block device. It implements the remote VDEV RPC protocol used by the
 * OpenZFS vdev_remote kernel module.
 *
 * Usage: zfs_remoted.exe -f <image_file> -p <port>
 *
 * Protocol:
 *   Header: [4 bytes cmd][4 bytes status][8 bytes offset][4 bytes size][4 bytes reserved]
 *   Commands:
 *     READ  (0x52454144): server reads from img and sends [header + data]
 *     WRITE (0x57524954): client sends [header + data], server writes to img
 *     FLUSH (0x464C5553): server calls _commit / FlushFileBuffers
 *     TRIM  (0x5452494D): server deallocates (not implemented for file, no-op)
 *     INFO  (0x494E464F): server returns device size / block info
 */

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#pragma comment(lib, "Ws2_32.lib")

/*
 * RPC header - must match vdev_remote.h exactly.
 */
#pragma pack(push, 1)
typedef struct rpc_hdr {
	uint32_t	cmd;
	uint32_t	status;
	uint64_t	offset;
	uint32_t	size;
	uint32_t	reserved;
} rpc_hdr_t;
#pragma pack(pop)

/* Command opcodes - must match vdev_remote.h */
#define CMD_READ  0x52454144
#define CMD_WRITE 0x57524954
#define CMD_FLUSH 0x464C5553
#define CMD_TRIM  0x5452494D
#define CMD_INFO  0x494E464F

/* Status codes */
#define STATUS_OK       0
#define STATUS_ERR_IO   1
#define STATUS_ERR_INVAL 2
#define STATUS_ERR_NOSPC 3

static volatile LONG g_running = 1;
static HANDLE g_file_handle = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_file_lock;

static void
print_usage(const char *prog)
{
	fprintf(stderr,
	    "Usage: %s -f <image_file> -p <port>\n"
	    "  -f <image_file>  Path to the raw image file to serve\n"
	    "  -p <port>        TCP port to listen on\n"
	    "  -h               Show this help\n",
	    prog);
}

static BOOL
ctrl_handler(DWORD ctrl_type)
{
	(void)ctrl_type;
	InterlockedExchange(&g_running, 0);
	return TRUE;
}

/*
 * Receive exactly 'len' bytes from socket.
 * Returns 0 on success, -1 on error.
 */
static int
recv_all(SOCKET s, void *buf, int len)
{
	char *p = (char *)buf;
	int remaining = len;

	while (remaining > 0) {
		int n = recv(s, p, remaining, 0);
		if (n <= 0) {
			if (n == 0)
				fprintf(stderr, "Connection closed by peer\n");
			else
				fprintf(stderr, "recv error: %d\n",
				    WSAGetLastError());
			return (-1);
		}
		p += n;
		remaining -= n;
	}
	return (0);
}

/*
 * Send exactly 'len' bytes to socket.
 * Returns 0 on success, -1 on error.
 */
static int
send_all(SOCKET s, const void *buf, int len)
{
	const char *p = (const char *)buf;
	int remaining = len;

	while (remaining > 0) {
		int n = send(s, p, remaining, 0);
		if (n <= 0) {
			fprintf(stderr, "send error: %d\n", WSAGetLastError());
			return (-1);
		}
		p += n;
		remaining -= n;
	}
	return (0);
}

/*
 * Handle a READ command.
 * 1. Read data from the image file at the given offset
 * 2. Send response header
 * 3. Send the data
 */
static void
handle_read(SOCKET client_sock, rpc_hdr_t *hdr)
{
	rpc_hdr_t resp;
	uint8_t *buf = NULL;

	if (hdr->size == 0 || hdr->size > (256 * 1024 * 1024)) {
		/* Sanity: max 256MB per read */
		resp.cmd = CMD_READ;
		resp.status = STATUS_ERR_INVAL;
		resp.offset = hdr->offset;
		resp.size = 0;
		resp.reserved = 0;
		send_all(client_sock, &resp, sizeof (resp));
		return;
	}

	buf = (uint8_t *)malloc(hdr->size);
	if (buf == NULL) {
		resp.cmd = CMD_READ;
		resp.status = STATUS_ERR_IO;
		resp.offset = hdr->offset;
		resp.size = 0;
		resp.reserved = 0;
		send_all(client_sock, &resp, sizeof (resp));
		return;
	}

	EnterCriticalSection(&g_file_lock);

	LARGE_INTEGER liOffset;
	liOffset.QuadPart = (LONGLONG)hdr->offset;

	/* Seek to offset */
	if (!SetFilePointerEx(g_file_handle, liOffset, NULL, FILE_BEGIN)) {
		LeaveCriticalSection(&g_file_lock);
		fprintf(stderr, "ReadFile seek to %llu failed: %lu\n",
		    (unsigned long long)hdr->offset, GetLastError());
		resp.cmd = CMD_READ;
		resp.status = STATUS_ERR_IO;
		resp.offset = hdr->offset;
		resp.size = 0;
		resp.reserved = 0;
		send_all(client_sock, &resp, sizeof (resp));
		free(buf);
		return;
	}

	DWORD bytes_read = 0;
	BOOL ok = ReadFile(g_file_handle, buf, hdr->size, &bytes_read, NULL);

	LeaveCriticalSection(&g_file_lock);

	if (!ok || bytes_read != hdr->size) {
		fprintf(stderr, "ReadFile failed at offset %llu size %u: %lu\n",
		    (unsigned long long)hdr->offset, hdr->size,
		    ok ? 0 : GetLastError());
		resp.cmd = CMD_READ;
		resp.status = STATUS_ERR_IO;
		resp.offset = hdr->offset;
		resp.size = 0;
		resp.reserved = 0;
		send_all(client_sock, &resp, sizeof (resp));
		free(buf);
		return;
	}

	/* Send success response header */
	resp.cmd = CMD_READ;
	resp.status = STATUS_OK;
	resp.offset = hdr->offset;
	resp.size = bytes_read;
	resp.reserved = 0;

	if (send_all(client_sock, &resp, sizeof (resp)) != 0) {
		free(buf);
		return;
	}

	/* Send the data */
	send_all(client_sock, buf, bytes_read);
	free(buf);
}

/*
 * Handle a WRITE command.
 * 1. Receive data from socket
 * 2. Write data to the image file
 * 3. Send response header
 */
static void
handle_write(SOCKET client_sock, rpc_hdr_t *hdr)
{
	rpc_hdr_t resp;
	uint8_t *buf = NULL;

	if (hdr->size == 0 || hdr->size > (256 * 1024 * 1024)) {
		resp.cmd = CMD_WRITE;
		resp.status = STATUS_ERR_INVAL;
		resp.offset = hdr->offset;
		resp.size = 0;
		resp.reserved = 0;
		send_all(client_sock, &resp, sizeof (resp));
		return;
	}

	buf = (uint8_t *)malloc(hdr->size);
	if (buf == NULL) {
		resp.cmd = CMD_WRITE;
		resp.status = STATUS_ERR_IO;
		resp.offset = hdr->offset;
		resp.size = 0;
		resp.reserved = 0;
		send_all(client_sock, &resp, sizeof (resp));
		return;
	}

	/* Receive the write data */
	if (recv_all(client_sock, buf, hdr->size) != 0) {
		free(buf);
		return;
	}

	EnterCriticalSection(&g_file_lock);

	LARGE_INTEGER liOffset;
	liOffset.QuadPart = (LONGLONG)hdr->offset;

	/* Seek to offset */
	if (!SetFilePointerEx(g_file_handle, liOffset, NULL, FILE_BEGIN)) {
		LeaveCriticalSection(&g_file_lock);
		free(buf);
		fprintf(stderr, "WriteFile seek to %llu failed: %lu\n",
		    (unsigned long long)hdr->offset, GetLastError());
		resp.cmd = CMD_WRITE;
		resp.status = STATUS_ERR_IO;
		resp.offset = hdr->offset;
		resp.size = 0;
		resp.reserved = 0;
		send_all(client_sock, &resp, sizeof (resp));
		return;
	}

	DWORD bytes_written = 0;
	BOOL ok = WriteFile(g_file_handle, buf, hdr->size, &bytes_written, NULL);

	LeaveCriticalSection(&g_file_lock);
	free(buf);

	if (!ok || bytes_written != hdr->size) {
		fprintf(stderr, "WriteFile failed at offset %llu size %u: %lu\n",
		    (unsigned long long)hdr->offset, hdr->size,
		    ok ? 0 : GetLastError());
		resp.cmd = CMD_WRITE;
		resp.status = STATUS_ERR_IO;
		resp.offset = hdr->offset;
		resp.size = 0;
		resp.reserved = 0;
		send_all(client_sock, &resp, sizeof (resp));
		return;
	}

	resp.cmd = CMD_WRITE;
	resp.status = STATUS_OK;
	resp.offset = hdr->offset;
	resp.size = bytes_written;
	resp.reserved = 0;
	send_all(client_sock, &resp, sizeof (resp));
}

/*
 * Handle a FLUSH command - flush file buffers to disk.
 */
static void
handle_flush(SOCKET client_sock, rpc_hdr_t *hdr)
{
	rpc_hdr_t resp;

	EnterCriticalSection(&g_file_lock);
	BOOL ok = FlushFileBuffers(g_file_handle);
	LeaveCriticalSection(&g_file_lock);

	resp.cmd = CMD_FLUSH;
	resp.status = ok ? STATUS_OK : STATUS_ERR_IO;
	resp.offset = 0;
	resp.size = 0;
	resp.reserved = 0;
	send_all(client_sock, &resp, sizeof (resp));
}

/*
 * Handle TRIM - deallocate region.
 * For file-backed storage on Windows, this is a no-op
 * (could use FSCTL_SET_ZERO_DATA but that zeroes, not trims).
 */
static void
handle_trim(SOCKET client_sock, rpc_hdr_t *hdr)
{
	rpc_hdr_t resp;

	/* For files, TRIM is a no-op. Return success. */
	resp.cmd = CMD_TRIM;
	resp.status = STATUS_OK;
	resp.offset = hdr->offset;
	resp.size = hdr->size;
	resp.reserved = 0;
	send_all(client_sock, &resp, sizeof (resp));

	fprintf(stderr, "TRIM range [%llu, %llu] (no-op for file)\n",
	    (unsigned long long)hdr->offset,
	    (unsigned long long)(hdr->offset + hdr->size));
}

/*
 * Handle INFO - return device information.
 * Response: offset=file_size, size=logical_block, reserved=phys_block
 */
static void
handle_info(SOCKET client_sock)
{
	rpc_hdr_t resp;

	EnterCriticalSection(&g_file_lock);

	LARGE_INTEGER file_size;
	BOOL ok = GetFileSizeEx(g_file_handle, &file_size);

	LeaveCriticalSection(&g_file_lock);

	if (!ok) {
		resp.cmd = CMD_INFO;
		resp.status = STATUS_ERR_IO;
		resp.offset = 0;
		resp.size = 0;
		resp.reserved = 0;
	} else {
		resp.cmd = CMD_INFO;
		resp.status = STATUS_OK;
		resp.offset = (uint64_t)file_size.QuadPart;  /* device size */
		resp.size = 512;     /* logical block size */
		resp.reserved = 4096; /* physical block size */
	}

	send_all(client_sock, &resp, sizeof (resp));
}

/*
 * Handle a single client connection.
 */
static void
handle_client(SOCKET client_sock)
{
	rpc_hdr_t hdr;

	fprintf(stderr, "Client connected\n");

	while (g_running) {
		/* Receive the RPC header */
		if (recv_all(client_sock, &hdr, sizeof (hdr)) != 0)
			break;

		switch (hdr.cmd) {
		case CMD_READ:
			handle_read(client_sock, &hdr);
			break;

		case CMD_WRITE:
			handle_write(client_sock, &hdr);
			break;

		case CMD_FLUSH:
			handle_flush(client_sock, &hdr);
			break;

		case CMD_TRIM:
			handle_trim(client_sock, &hdr);
			break;

		case CMD_INFO:
			handle_info(client_sock);
			break;

		default:
			fprintf(stderr, "Unknown command: 0x%08X\n", hdr.cmd);
			hdr.status = STATUS_ERR_INVAL;
			send_all(client_sock, &hdr, sizeof (hdr));
			break;
		}
	}

	closesocket(client_sock);
	fprintf(stderr, "Client disconnected\n");
}

/*
 * Main: parse arguments, open image file, listen for connections.
 */
int
main(int argc, char *argv[])
{
	const char *image_file = NULL;
	uint16_t port = 0;
	int opt;

	/* Parse arguments */
	for (opt = 1; opt < argc; opt++) {
		if (strcmp(argv[opt], "-f") == 0 && opt + 1 < argc) {
			image_file = argv[++opt];
		} else if (strcmp(argv[opt], "-p") == 0 && opt + 1 < argc) {
			port = (uint16_t)atoi(argv[++opt]);
		} else if (strcmp(argv[opt], "-h") == 0) {
			print_usage(argv[0]);
			return (0);
		} else {
			fprintf(stderr, "Unknown option: %s\n", argv[opt]);
			print_usage(argv[0]);
			return (1);
		}
	}

	if (image_file == NULL || port == 0) {
		fprintf(stderr, "Error: -f <image_file> and -p <port> "
		    "are required\n");
		print_usage(argv[0]);
		return (1);
	}

	/* Open the image file for synchronous I/O */
	g_file_handle = CreateFileA(
	    image_file,
	    GENERIC_READ | GENERIC_WRITE,
	    0, /* exclusive access */
	    NULL,
	    OPEN_EXISTING,
	    FILE_ATTRIBUTE_NORMAL,  /* synchronous, buffered */
	    NULL);

	if (g_file_handle == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "Failed to open image file '%s': %lu\n",
		    image_file, GetLastError());
		return (1);
	}

	fprintf(stderr, "Opened image file: %s\n", image_file);

	InitializeCriticalSection(&g_file_lock);

	/* Initialize Winsock */
	WSADATA wsa_data;
	if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
		fprintf(stderr, "WSAStartup failed: %d\n", WSAGetLastError());
		CloseHandle(g_file_handle);
		return (1);
	}

	/* Create listening socket */
	SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listen_sock == INVALID_SOCKET) {
		fprintf(stderr, "socket failed: %d\n", WSAGetLastError());
		WSACleanup();
		CloseHandle(g_file_handle);
		return (1);
	}

	/* Set SO_REUSEADDR */
	int reuse = 1;
	setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR,
	    (const char *)&reuse, sizeof (reuse));

	struct sockaddr_in server_addr;
	memset(&server_addr, 0, sizeof (server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = INADDR_ANY;
	server_addr.sin_port = htons(port);

	if (bind(listen_sock, (struct sockaddr *)&server_addr,
	    sizeof (server_addr)) == SOCKET_ERROR) {
		fprintf(stderr, "bind to port %u failed: %d\n",
		    port, WSAGetLastError());
		closesocket(listen_sock);
		WSACleanup();
		CloseHandle(g_file_handle);
		return (1);
	}

	if (listen(listen_sock, SOMAXCONN) == SOCKET_ERROR) {
		fprintf(stderr, "listen failed: %d\n", WSAGetLastError());
		closesocket(listen_sock);
		WSACleanup();
		CloseHandle(g_file_handle);
		return (1);
	}

	/* Set Ctrl+C handler for graceful shutdown */
	SetConsoleCtrlHandler((PHANDLER_ROUTINE)ctrl_handler, TRUE);

	fprintf(stderr, "zfs_remoted: listening on port %u, serving '%s'\n",
	    port, image_file);

	while (g_running) {
		struct sockaddr_in client_addr;
		int client_len = sizeof (client_addr);

		/* Set a timeout so we can check g_running periodically */
		struct timeval tv;
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		fd_set readfds;
		FD_ZERO(&readfds);
		FD_SET(listen_sock, &readfds);

		int sel_ret = select(0, &readfds, NULL, NULL, &tv);
		if (sel_ret < 0)
			break;
		if (sel_ret == 0)
			continue; /* timeout, check g_running */

		SOCKET client_sock = accept(listen_sock,
		    (struct sockaddr *)&client_addr, &client_len);
		if (client_sock == INVALID_SOCKET)
			continue;

		handle_client(client_sock);
	}

	fprintf(stderr, "Shutting down...\n");

	closesocket(listen_sock);
	WSACleanup();

	DeleteCriticalSection(&g_file_lock);
	CloseHandle(g_file_handle);

	return (0);
}
