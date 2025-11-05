#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	RESP_OK_NVL(client, nvl) do { \
		size_t _len = 0; \
		char *_json = zed_json_from_nvlist((nvl), &_len); \
		if (_json) RESP_OK_JSON((client), _len, _json); \
		else RESP_ERR((client), ERROR_OUTOFMEMORY); \
	} while (0)

extern char *zed_json_from_nvlist(nvlist_t *nvl, size_t *out_len);
extern DWORD WriteAll(HANDLE h, const void *buf, DWORD len);
extern DWORD ReadAll(HANDLE h, void *buf, DWORD need);

extern void RESP_LIBZFS_ERR(HANDLE client, const char *fallback);


#ifdef __cplusplus
}
#endif
