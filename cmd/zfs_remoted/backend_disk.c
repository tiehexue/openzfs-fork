/*
 * CDDL HEADER START ... (see LICENSE)
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2026, OpenZFS Remote VDEV contributors.
 *
 * Disk backend -- serves a physical disk (e.g. \\.\PhysicalDrive0).
 *
 * Disk specifier resolution:
 *   "0"                  -> \\.\PhysicalDrive0
 *   "physicaldrive2"     -> \\.\PhysicalDrive2  (case-insensitive)
 *   "\\.\PhysicalDrive3" -> literal NT path
 */

#include "zfs_remoted.h"
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

typedef struct {
	HANDLE dh;
} disk_priv_t;

static int
resolve_path(const char *spec, char *out, size_t outlen)
{
	/* Already a full NT path? */
	if (spec[0] == '\\' && spec[1] == '\\' && spec[2] == '.') {
		strncpy(out, spec, outlen - 1);
		out[outlen - 1] = '\0';
		return 0;
	}

	/* "physicaldriveN" (case-insensitive) */
	if (_strnicmp(spec, "physicaldrive", 13) == 0) {
		snprintf(out, outlen, "\\\\.\\%s", spec);
		return 0;
	}

	/* Plain number "N" -> PhysicalDriveN */
	int all_digits = 1;
	for (const char *p = spec; *p; p++) {
		if (!isdigit((unsigned char)*p)) { all_digits = 0; break; }
	}
	if (all_digits && spec[0] != '\0') {
		snprintf(out, outlen, "\\\\.\\PhysicalDrive%s", spec);
		return 0;
	}

	return -1;
}

static int
disk_open(block_backend_t *bb, const char *spec)
{
	char path[256];
	HANDLE h;

	if (resolve_path(spec, path, sizeof(path)) != 0) {
		fprintf(stderr, "disk: cannot resolve '%s'\n", spec);
		return -1;
	}

	h = CreateFileA(path,
	    GENERIC_READ | GENERIC_WRITE,
	    0,                    /* exclusive */
	    NULL, OPEN_EXISTING,
	    FILE_ATTRIBUTE_NORMAL,
	    NULL);

	if (h == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "disk: CreateFile(%s) failed: %lu\n",
		    path, GetLastError());
		return -1;
	}

	/* --- disk size --- */
	GET_LENGTH_INFORMATION gli = { 0 };
	DWORD junk;
	if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO,
	    NULL, 0, &gli, sizeof(gli), &junk, NULL)) {
		bb->bb_dev_size = gli.Length.QuadPart;
	} else {
		DISK_GEOMETRY_EX dg = { 0 };
		if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
		    NULL, 0, &dg, sizeof(dg), &junk, NULL)) {
			bb->bb_dev_size = dg.DiskSize.QuadPart;
		} else {
			bb->bb_dev_size = 0;
		}
	}

	/* --- sector sizes --- */
	STORAGE_PROPERTY_QUERY spq = { 0 };
	spq.PropertyId = StorageAccessAlignmentProperty;
	spq.QueryType = PropertyStandardQuery;

	STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR sa = { 0 };
	if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
	    &spq, sizeof(spq), &sa, sizeof(sa), &junk, NULL)) {
		bb->bb_lbasize = sa.BytesPerLogicalSector;
		bb->bb_pbasize = sa.BytesPerPhysicalSector;
	} else {
		bb->bb_lbasize = 512;
		bb->bb_pbasize = 4096;
	}

	disk_priv_t *dp = (disk_priv_t *)calloc(1, sizeof(*dp));
	if (!dp) { CloseHandle(h); return -1; }
	dp->dh = h;
	bb->bb_priv = dp;

	fprintf(stderr, "disk: opened %s (%llu MiB, %u/%u byte sectors)\n",
	    path,
	    (unsigned long long)(bb->bb_dev_size / (1024 * 1024)),
	    bb->bb_lbasize, bb->bb_pbasize);
	return 0;
}

static void
disk_close(block_backend_t *bb)
{
	disk_priv_t *dp = (disk_priv_t *)bb->bb_priv;
	if (dp) {
		if (dp->dh != INVALID_HANDLE_VALUE) CloseHandle(dp->dh);
		free(dp);
		bb->bb_priv = NULL;
	}
}

/* Helper: overlapped I/O with GetOverlappedResult wait */
static int
disk_io(HANDLE dh, void *buf, uint32_t size, uint64_t offset,
    int is_write)
{
	OVERLAPPED ov = { 0 };
	DWORD bytes = 0;

	ov.Offset     = (DWORD)(offset & 0xFFFFFFFF);
	ov.OffsetHigh = (DWORD)(offset >> 32);

	BOOL ok = is_write
	    ? WriteFile(dh, buf, size, &bytes, &ov)
	    : ReadFile(dh, buf, size, &bytes, &ov);

	if (!ok) {
		if (GetLastError() != ERROR_IO_PENDING ||
		    !GetOverlappedResult(dh, &ov, &bytes, TRUE))
			return -1;
	}
	return (bytes == size) ? 0 : -1;
}

static int
disk_read(block_backend_t *bb, void *buf, uint32_t size, uint64_t offset)
{
	return disk_io(((disk_priv_t *)bb->bb_priv)->dh,
	    buf, size, offset, 0);
}

static int
disk_write(block_backend_t *bb, const void *buf, uint32_t size,
    uint64_t offset)
{
	return disk_io(((disk_priv_t *)bb->bb_priv)->dh,
	    (void *)buf, size, offset, 1);
}

static int
disk_flush(block_backend_t *bb)
{
	return FlushFileBuffers(((disk_priv_t *)bb->bb_priv)->dh) ? 0 : -1;
}

const block_backend_t disk_backend = {
	"disk",
	disk_open, disk_close, disk_read, disk_write, disk_flush,
	0, 512, 4096, NULL
};
