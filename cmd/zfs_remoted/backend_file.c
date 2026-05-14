/*
 * CDDL HEADER START ... (see LICENSE)
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2026, OpenZFS Remote VDEV contributors.
 *
 * File backend -- serves a raw image file.
 */
#include "zfs_remoted.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct {
	HANDLE fh;
} file_priv_t;

static int
file_open(block_backend_t *bb, const char *path)
{
	fprintf(stderr, "file_open: '%s'\n", path);
	fflush(stderr);

	file_priv_t *fp = (file_priv_t *)calloc(1, sizeof(*fp));
	if (!fp) { fprintf(stderr, "file_open: calloc failed\n"); return -1; }

	fp->fh = CreateFileA(
	    path,
	    GENERIC_READ | GENERIC_WRITE,
	    0,                    /* exclusive */
	    NULL,
	    OPEN_EXISTING,
	    FILE_ATTRIBUTE_NORMAL,
	    NULL);

	if (fp->fh == INVALID_HANDLE_VALUE) { free(fp); return -1; }

	LARGE_INTEGER sz;
	bb->bb_dev_size = (GetFileSizeEx(fp->fh, &sz))
	    ? (uint64_t)sz.QuadPart : 0;
	bb->bb_lbasize = 512;
	bb->bb_pbasize = 4096;
	bb->bb_priv = fp;
	return 0;
}

static void
file_close(block_backend_t *bb)
{
	file_priv_t *fp = (file_priv_t *)bb->bb_priv;
	if (fp) {
		if (fp->fh != INVALID_HANDLE_VALUE) CloseHandle(fp->fh);
		free(fp);
		bb->bb_priv = NULL;
	}
}

static int
file_read(block_backend_t *bb, void *buf, uint32_t size, uint64_t offset)
{
	file_priv_t *fp = (file_priv_t *)bb->bb_priv;
	LARGE_INTEGER li;
	DWORD bytes = 0;

	li.QuadPart = (LONGLONG)offset;
	if (!SetFilePointerEx(fp->fh, li, NULL, FILE_BEGIN)) return -1;
	if (!ReadFile(fp->fh, buf, size, &bytes, NULL))        return -1;
	return (bytes == size) ? 0 : -1;
}

static int
file_write(block_backend_t *bb, const void *buf, uint32_t size,
    uint64_t offset)
{
	file_priv_t *fp = (file_priv_t *)bb->bb_priv;
	LARGE_INTEGER li;
	DWORD bytes = 0;

	li.QuadPart = (LONGLONG)offset;
	if (!SetFilePointerEx(fp->fh, li, NULL, FILE_BEGIN)) return -1;
	if (!WriteFile(fp->fh, buf, size, &bytes, NULL))       return -1;
	return (bytes == size) ? 0 : -1;
}

static int
file_flush(block_backend_t *bb)
{
	file_priv_t *fp = (file_priv_t *)bb->bb_priv;
	return FlushFileBuffers(fp->fh) ? 0 : -1;
}

const block_backend_t file_backend = {
	"file",
	file_open, file_close, file_read, file_write, file_flush,
	0, 512, 4096, NULL
};
